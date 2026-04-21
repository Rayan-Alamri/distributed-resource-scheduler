#include "server.h"
#include "../shared/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Per-worker thread ───────────────────────────────────────────────────── */

typedef struct {
    int          sock_fd;
    uint32_t     worker_id;
    MasterState *ms;
} WorkerArg;

static void *worker_thread(void *arg) {
    WorkerArg   *wa  = (WorkerArg *)arg;
    int          fd  = wa->sock_fd;
    uint32_t     wid = wa->worker_id;
    MasterState *ms  = wa->ms;
    free(wa);

    NetworkPayload pkt;
    while (recv_full(fd, &pkt, sizeof(pkt)) == 0) {
        /*
         * Every socket read arrives in network byte order. Convert once at the
         * boundary so the rest of the master logic can compare normal integers.
         */
        payload_to_host(&pkt);

        switch ((MessageType)pkt.type) {

        case MSG_HEARTBEAT:
            /*
             * Heartbeats are lightweight liveness updates. The scheduler and
             * dashboard read the same registry, so writes stay under the lock.
             */
            pthread_mutex_lock(&ms->registry.lock);
            {
                WorkerInfo *w = registry_get(&ms->registry, wid);
                if (w)
                    w->last_heartbeat = time(NULL);
            }
            pthread_mutex_unlock(&ms->registry.lock);
            break;

        case MSG_RESULT:
            printf("[master] result: worker=%u task=%u result=%u\n",
                   pkt.worker_id, pkt.task_id, pkt.result);
            /*
             * A result completes the worker's current task. Clearing the task
             * snapshot prevents a later disconnect from requeueing finished work.
             */
            pthread_mutex_lock(&ms->registry.lock);
            {
                WorkerInfo *w = registry_get(&ms->registry, wid);
                if (w) {
                    w->status = WORKER_IDLE;
                    memset(&w->current_task, 0, sizeof(NetworkPayload));
                }
            }
            pthread_mutex_unlock(&ms->registry.lock);
            break;

        default:
            fprintf(stderr, "[master] unexpected msg type %u from worker %u\n",
                    pkt.type, wid);
            break;
        }
    }

    /*
     * Worker disconnected. If it was busy, move its task back into the FIFO so
     * another worker can complete it. The registry lock is released before
     * queue_requeue() to avoid holding two subsystem locks at the same time.
     */
    fprintf(stderr, "[master] worker %u disconnected\n", wid);
    pthread_mutex_lock(&ms->registry.lock);
    {
        WorkerInfo *dead = registry_get(&ms->registry, wid);
        if (dead && dead->status == WORKER_BUSY && dead->current_task.task_id != 0) {
            NetworkPayload orphan = dead->current_task;
            pthread_mutex_unlock(&ms->registry.lock);
            fprintf(stderr, "[master] requeueing orphaned task %u\n", orphan.task_id);
            queue_requeue(&ms->queue, &orphan);
        } else {
            pthread_mutex_unlock(&ms->registry.lock);
        }
    }
    registry_set_offline(&ms->registry, wid);
    close(fd);
    return NULL;
}

/* ── Server lifecycle ────────────────────────────────────────────────────── */

int server_init(MasterState *ms, int port) {
    queue_init(&ms->queue);
    registry_init(&ms->registry);
    scheduler_init(&ms->scheduler, &ms->queue, &ms->registry);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    ms->listen_fd = fd;
    printf("[master] listening on port %d\n", port);
    return 0;
}

void server_run(MasterState *ms) {
    scheduler_start(&ms->scheduler);

    uint32_t next_worker_id = 1;
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);

    while (1) {
        int cfd = accept(ms->listen_fd, (struct sockaddr *)&client_addr, &addrlen);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }

        /*
         * The first packet must be a registration handshake. This keeps random
         * TCP clients from being added to the worker registry accidentally.
         */
        NetworkPayload reg;
        if (recv_full(cfd, &reg, sizeof(reg)) != 0) {
            fprintf(stderr, "[master] registration recv failed, dropping connection\n");
            close(cfd);
            continue;
        }
        payload_to_host(&reg);
        if ((MessageType)reg.type != MSG_REGISTER) {
            fprintf(stderr, "[master] expected MSG_REGISTER, got type %u — dropping\n",
                    reg.type);
            close(cfd);
            continue;
        }

        uint32_t wid = next_worker_id++;
        if (registry_add(&ms->registry, wid, cfd) < 0) {
            fprintf(stderr, "[master] registry full — rejecting worker\n");
            close(cfd);
            continue;
        }
        printf("[master] worker %u registered from %s\n",
               wid, inet_ntoa(client_addr.sin_addr));

        WorkerArg *wa = malloc(sizeof(WorkerArg));
        if (!wa) {
            fprintf(stderr, "[master] malloc failed for WorkerArg\n");
            close(cfd);
            continue;
        }
        wa->sock_fd   = cfd;
        wa->worker_id = wid;
        wa->ms        = ms;

        /*
         * Each worker gets a detached reader thread. The main server loop stays
         * dedicated to accepting new workers while these threads handle results.
         */
        pthread_t       tid;
        pthread_attr_t  attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, worker_thread, wa);
        pthread_attr_destroy(&attr);
    }
}

void server_shutdown(MasterState *ms) {
    scheduler_stop(&ms->scheduler);
    close(ms->listen_fd);
    queue_destroy(&ms->queue);
}

/* ── Demo task seeder ────────────────────────────────────────────────────── */

static void *seed_demo_tasks(void *arg) {
    MasterState *ms = (MasterState *)arg;
    const char *env = getenv("DEMO_TASKS");
    int count = 6;

    if (env && *env != '\0') {
        int n = atoi(env);
        if (n > 0 && n <= 100)
            count = n;
    }

    /* Wait for workers to connect before flooding the queue */
    printf("[demo] will enqueue %d tasks in 3 seconds...\n", count);
    sleep(3);

    for (int i = 0; i < count; i++) {
        NetworkPayload task;
        memset(&task, 0, sizeof(task));
        task.type         = MSG_TASK;
        task.task_id      = (uint32_t)(i + 1);
        task.command_code = 1;  /* all prime tasks — slow enough to observe */
        task.argument     = 5000000 + i * 1000000;
        queue_enqueue(&ms->queue, &task);
        printf("[demo] enqueued task %d: command=%u arg=%u\n",
               i + 1, task.command_code, task.argument);
    }
    printf("[demo] all %d tasks enqueued\n", count);
    return NULL;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void) {
    MasterState ms;
    int port = MASTER_PORT;
    const char *port_env = getenv("MASTER_PORT");
    if (port_env && *port_env != '\0') {
        int p = atoi(port_env);
        if (p > 0 && p <= 65535)
            port = p;
    }
    if (server_init(&ms, port) != 0)
        return 1;

    /* If DEMO_TASKS is set, spawn a thread that seeds sample tasks */
    if (getenv("DEMO_TASKS")) {
        pthread_t       demo_tid;
        pthread_attr_t  demo_attr;
        pthread_attr_init(&demo_attr);
        pthread_attr_setdetachstate(&demo_attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&demo_tid, &demo_attr, seed_demo_tasks, &ms);
        pthread_attr_destroy(&demo_attr);
    }

    server_run(&ms);
    server_shutdown(&ms);
    return 0;
}
