#include "client.h"
#include "executor.h"
#include "../shared/protocol.h"

/*
 * Worker process network client.
 *
 * A worker registers with the master, sends periodic heartbeat frames, accepts
 * task frames, runs each task through executor.c, and sends the result back.
 * Separate locks keep worker ID updates, socket writes, and active-task
 * shutdown accounting independent.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static const char *now(void) {
    static char buf[9];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

static const char *cmd_name(uint32_t cmd) {
    switch ((CommandCode)cmd) {
    case CMD_PRIME:          return "prime";
    case CMD_MATRIX:         return "matrix";
    case CMD_PRIME_RANGE:    return "prime_range";
    case CMD_MONTE_CARLO:    return "monte_carlo";
    case CMD_MANDELBROT:     return "mandelbrot";
    case CMD_FFMPEG_SEGMENT: return "ffmpeg_segment";
    case CMD_FFMPEG_SCRIPT:  return "ffmpeg_script";
    case CMD_MATRIX_PARALLEL: return "matrix_parallel";
    default:                 return "unknown";
    }
}

static uint64_t elapsed_ms(const struct timespec *start, const struct timespec *end) {
    time_t sec = end->tv_sec - start->tv_sec;
    long nsec = end->tv_nsec - start->tv_nsec;

    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }

    return (uint64_t)sec * 1000ULL + (uint64_t)nsec / 1000000ULL;
}

static void print_task_fields(const NetworkPayload *task) {
    if ((CommandCode)task->command_code == CMD_PRIME_RANGE) {
        printf("task_id=%u cmd=%u(%s) arg=%u range_end=%u",
               task->task_id, task->command_code, cmd_name(task->command_code),
               task->argument, task->result);
    } else {
        printf("task_id=%u cmd=%u(%s) arg=%u",
               task->task_id, task->command_code, cmd_name(task->command_code),
               task->argument);
    }
}

typedef struct {
    int sock_fd;
    int running;
    uint32_t worker_id;
    /* id_lock protects updates when the master assigns/echoes an ID in tasks. */
    pthread_mutex_t id_lock;
    /* send_lock prevents heartbeat and result threads from interleaving bytes. */
    pthread_mutex_t send_lock;
    /* task_lock protects shutdown state and active task accounting. */
    pthread_mutex_t task_lock;
    pthread_cond_t tasks_done;
    int active_tasks;
} WorkerRuntime;

typedef struct {
    WorkerRuntime *rt;
    NetworkPayload task;
} TaskThreadArg;

static int parse_int_arg(const char *value, int fallback) {
    char *end = NULL;
    long parsed;

    if (!value || *value == '\0')
        return fallback;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0 || parsed > 65535)
        return fallback;

    return (int)parsed;
}

static uint32_t parse_worker_id(const char *value, uint32_t fallback) {
    char *end = NULL;
    unsigned long parsed;

    if (!value || *value == '\0')
        return fallback;

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX)
        return fallback;

    return (uint32_t)parsed;
}

static uint32_t runtime_get_worker_id(WorkerRuntime *rt) {
    uint32_t worker_id;

    pthread_mutex_lock(&rt->id_lock);
    worker_id = rt->worker_id;
    pthread_mutex_unlock(&rt->id_lock);

    return worker_id;
}

static void runtime_set_worker_id(WorkerRuntime *rt, uint32_t worker_id) {
    pthread_mutex_lock(&rt->id_lock);
    rt->worker_id = worker_id;
    pthread_mutex_unlock(&rt->id_lock);
}

static int send_payload_locked(WorkerRuntime *rt, NetworkPayload *payload) {
    NetworkPayload wire = *payload;
    int status;

    /*
     * Convert a local copy so callers can keep using host-order payloads after
     * this function returns. This avoids accidental double conversion bugs.
     */
    payload_to_net(&wire);
    pthread_mutex_lock(&rt->send_lock);
    status = send_full(rt->sock_fd, &wire, sizeof(wire));
    pthread_mutex_unlock(&rt->send_lock);

    return status;
}

static int send_register(WorkerRuntime *rt) {
    NetworkPayload payload;

    memset(&payload, 0, sizeof(payload));
    payload.type = MSG_REGISTER;
    payload.worker_id = runtime_get_worker_id(rt);

    return send_payload_locked(rt, &payload);
}

static int send_result(WorkerRuntime *rt, const NetworkPayload *task, uint32_t result) {
    NetworkPayload payload;

    memset(&payload, 0, sizeof(payload));
    payload.type = MSG_RESULT;
    payload.worker_id = runtime_get_worker_id(rt);
    payload.task_id = task->task_id;
    payload.command_code = task->command_code;
    payload.argument = task->argument;
    payload.result = result;

    return send_payload_locked(rt, &payload);
}

static void runtime_task_started(WorkerRuntime *rt) {
    pthread_mutex_lock(&rt->task_lock);
    /* Track outstanding detached task threads so shutdown can wait cleanly. */
    rt->active_tasks++;
    pthread_mutex_unlock(&rt->task_lock);
}

static void runtime_task_finished(WorkerRuntime *rt) {
    pthread_mutex_lock(&rt->task_lock);
    rt->active_tasks--;
    if (rt->active_tasks == 0) {
        /* Wake the main thread if it is waiting for in-flight tasks to drain. */
        pthread_cond_signal(&rt->tasks_done);
    }
    pthread_mutex_unlock(&rt->task_lock);
}

static void runtime_wait_for_tasks(WorkerRuntime *rt) {
    pthread_mutex_lock(&rt->task_lock);
    while (rt->active_tasks > 0)
        pthread_cond_wait(&rt->tasks_done, &rt->task_lock);
    pthread_mutex_unlock(&rt->task_lock);
}

static int runtime_is_running(WorkerRuntime *rt) {
    int running;

    pthread_mutex_lock(&rt->task_lock);
    running = rt->running;
    pthread_mutex_unlock(&rt->task_lock);

    return running;
}

static void runtime_stop(WorkerRuntime *rt) {
    pthread_mutex_lock(&rt->task_lock);
    rt->running = 0;
    pthread_mutex_unlock(&rt->task_lock);
}

static void *heartbeat_loop(void *arg) {
    WorkerRuntime *rt = (WorkerRuntime *)arg;

    while (runtime_is_running(rt)) {
        NetworkPayload payload;

        sleep(WORKER_HEARTBEAT_INTERVAL_SEC);
        if (!runtime_is_running(rt))
            break;

        memset(&payload, 0, sizeof(payload));
        payload.type = MSG_HEARTBEAT;
        payload.worker_id = runtime_get_worker_id(rt);

        /*
         * A heartbeat send failure usually means the TCP connection is broken.
         * Shutting down the socket wakes the main recv loop so cleanup can run.
         */
        if (send_payload_locked(rt, &payload) != 0) {
            fprintf(stderr, "[%s][worker] heartbeat send failed; master may be disconnected\n", now());
            runtime_stop(rt);
            shutdown(rt->sock_fd, SHUT_RDWR);
            break;
        }
    }

    return NULL;
}

static int execute_and_send_task(WorkerRuntime *rt, const NetworkPayload *task) {
    uint32_t result = 0;
    int exec_status;
    struct timespec start;
    struct timespec end;
    uint32_t worker_id = runtime_get_worker_id(rt);

    clock_gettime(CLOCK_MONOTONIC, &start);

    printf("[%s][worker] task_started: worker=%u ", now(), worker_id);
    print_task_fields(task);
    printf("\n");

    /*
     * executor_run_task forks the real workload. The networking thread stays in
     * this process, so a bad workload cannot corrupt the worker's socket state.
     */
    exec_status = executor_run_task(task, &result);
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (exec_status != EXECUTOR_OK) {
        fprintf(stderr, "[%s][worker] task_failed: worker=%u task_id=%u"
                " status=%d duration=%llums\n",
                now(), worker_id, task->task_id, exec_status,
                (unsigned long long)elapsed_ms(&start, &end));
        result = 0;
    }

    printf("[%s][worker] task_finished: worker=%u task_id=%u cmd=%u(%s)"
           " result=%u duration=%llums local_status=%s\n",
           now(), worker_id, task->task_id, task->command_code,
           cmd_name(task->command_code), result,
           (unsigned long long)elapsed_ms(&start, &end),
           exec_status == EXECUTOR_OK ? "ok" : "failed");

    if (send_result(rt, task, result) != 0) {
        fprintf(stderr, "[%s][worker] result_send_failed: worker=%u task_id=%u\n",
                now(), worker_id, task->task_id);
        return -1;
    }

    printf("[%s][worker] task_completed: worker=%u task_id=%u result=%u"
           " duration=%llums result_sent=yes\n",
           now(), worker_id, task->task_id, result,
           (unsigned long long)elapsed_ms(&start, &end));
    return 0;
}

static void *task_thread_main(void *arg) {
    TaskThreadArg *tta = (TaskThreadArg *)arg;
    WorkerRuntime *rt = tta->rt;
    int status;

    status = execute_and_send_task(rt, &tta->task);
    free(tta);

    if (status != 0) {
        runtime_stop(rt);
        shutdown(rt->sock_fd, SHUT_RDWR);
    }

    runtime_task_finished(rt);
    return NULL;
}

int worker_connect(const char *host, int port) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    char port_buf[16];
    int sock_fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_buf, sizeof(port_buf), "%d", port);
    if (getaddrinfo(host, port_buf, &hints, &result) != 0)
        return -1;

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd < 0)
            continue;

        if (connect(sock_fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        close(sock_fd);
        sock_fd = -1;
    }

    freeaddrinfo(result);
    return sock_fd;
}

static int dispatch_task(WorkerRuntime *rt, const NetworkPayload *task) {
    TaskThreadArg *tta;
    pthread_t tid;
    int create_status;

    /*
     * The master includes the assigned worker_id on task packets. Store it so
     * subsequent heartbeats and results identify this worker consistently.
     */
    if (task->worker_id != 0)
        runtime_set_worker_id(rt, task->worker_id);

    tta = malloc(sizeof(*tta));
    if (!tta) {
        fprintf(stderr, "[%s][worker] malloc failed for task thread argument\n", now());
        return execute_and_send_task(rt, task);
    }

    tta->rt = rt;
    tta->task = *task;

    runtime_task_started(rt);
    create_status = pthread_create(&tid, NULL, task_thread_main, tta);
    if (create_status != 0) {
        fprintf(stderr, "[%s][worker] pthread_create failed for task %u; running inline\n",
                now(), task->task_id);
        runtime_task_finished(rt);
        free(tta);
        return execute_and_send_task(rt, task);
    }

    /*
     * Task execution is detached from the receive loop. This lets the worker
     * accept future control messages while child processes compute workloads.
     */
    pthread_detach(tid);
    return 0;
}

int worker_run(const WorkerConfig *config) {
    WorkerRuntime rt;
    pthread_t heartbeat_thread;
    int heartbeat_started = 0;
    int status = 0;

    memset(&rt, 0, sizeof(rt));
    rt.sock_fd = worker_connect(config->host, config->port);
    if (rt.sock_fd < 0) {
        fprintf(stderr, "[%s][worker] failed to connect to master at %s:%d\n",
                now(), config->host, config->port);
        return 1;
    }

    rt.running = 1;
    rt.worker_id = config->worker_id;
    pthread_mutex_init(&rt.id_lock, NULL);
    pthread_mutex_init(&rt.send_lock, NULL);
    pthread_mutex_init(&rt.task_lock, NULL);
    pthread_cond_init(&rt.tasks_done, NULL);

    printf("[%s][worker] connected to master at %s:%d\n", now(), config->host, config->port);

    if (send_register(&rt) != 0) {
        fprintf(stderr, "[%s][worker] registration send failed\n", now());
        status = 1;
        goto cleanup;
    }
    printf("[%s][worker] registration sent requested_worker_id=%u pid=%ld\n",
           now(), config->worker_id, (long)getpid());

    /*
     * Heartbeats run in parallel with task execution, giving the master a
     * simple liveness signal even when workloads are CPU-bound.
     */
    if (pthread_create(&heartbeat_thread, NULL, heartbeat_loop, &rt) != 0) {
        fprintf(stderr, "[%s][worker] failed to start heartbeat thread\n", now());
        status = 1;
        goto cleanup;
    }
    heartbeat_started = 1;

    while (runtime_is_running(&rt)) {
        NetworkPayload payload;

        if (recv_full(rt.sock_fd, &payload, sizeof(payload)) != 0) {
            fprintf(stderr, "[%s][worker] master disconnected\n", now());
            runtime_stop(&rt);
            break;
        }

        /* Inbound frames are fixed-size NetworkPayload objects from the master. */
        payload_to_host(&payload);
        switch ((MessageType)payload.type) {
        case MSG_TASK:
            if (payload.worker_id != 0)
                runtime_set_worker_id(&rt, payload.worker_id);
            printf("[%s][worker] task_received: worker=%u ",
                   now(), runtime_get_worker_id(&rt));
            print_task_fields(&payload);
            printf("\n");
            if (dispatch_task(&rt, &payload) != 0) {
                runtime_stop(&rt);
                status = 1;
            }
            break;
        default:
            fprintf(stderr, "[%s][worker] unexpected message type %u from master\n", now(), payload.type);
            break;
        }
    }

cleanup:
    runtime_stop(&rt);
    shutdown(rt.sock_fd, SHUT_RDWR);
    if (heartbeat_started)
        pthread_join(heartbeat_thread, NULL);
    runtime_wait_for_tasks(&rt);
    close(rt.sock_fd);
    pthread_cond_destroy(&rt.tasks_done);
    pthread_mutex_destroy(&rt.task_lock);
    pthread_mutex_destroy(&rt.send_lock);
    pthread_mutex_destroy(&rt.id_lock);
    return status;
}

int main(int argc, char **argv) {
    WorkerConfig config;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGPIPE, SIG_IGN);

    config.host = getenv("MASTER_HOST");
    if (!config.host || *config.host == '\0')
        config.host = WORKER_DEFAULT_HOST;

    config.port = parse_int_arg(getenv("MASTER_PORT"), WORKER_DEFAULT_PORT);
    config.worker_id = parse_worker_id(getenv("WORKER_ID"), WORKER_DEFAULT_ID);

    if (argc > 1)
        config.host = argv[1];
    if (argc > 2)
        config.port = parse_int_arg(argv[2], config.port);
    if (argc > 3)
        config.worker_id = parse_worker_id(argv[3], config.worker_id);

    return worker_run(&config);
}
