#include "scheduler.h"
#include "../shared/protocol.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

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
    default:                 return "unknown";
    }
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

/* ── WorkerRegistry ─────────────────────────────────────────────────────── */

/**
 * registry_init initializes the WorkerRegistry.
 * Sets all slots to unused and initializes the mutex.
 *
 * @param r Pointer to WorkerRegistry.
 */
void registry_init(WorkerRegistry *r) {
    memset(r, 0, sizeof(WorkerRegistry));
    /* sock_fd == -1 marks an unused slot without needing a separate flag. */
    for (int i = 0; i < MAX_WORKERS; i++)
        r->workers[i].sock_fd = -1;
    r->count    = 0;
    r->rr_index = 0;
    pthread_mutex_init(&r->lock, NULL);
}

int registry_add(WorkerRegistry *r, uint32_t worker_id, int sock_fd) {
    pthread_mutex_lock(&r->lock);
    if (r->count >= MAX_WORKERS) {
        pthread_mutex_unlock(&r->lock);
        return -1;
    }
    /*
     * Reuse the first free array slot instead of compacting the table. This
     * keeps worker pointers stable while other threads inspect the registry.
     */
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (r->workers[i].sock_fd == -1) {
            memset(&r->workers[i], 0, sizeof(WorkerInfo));
            r->workers[i].worker_id      = worker_id;
            r->workers[i].sock_fd        = sock_fd;
            r->workers[i].status         = WORKER_IDLE;
            r->workers[i].last_heartbeat = time(NULL);
            r->count++;
            pthread_mutex_unlock(&r->lock);
            return i;
        }
    }
    pthread_mutex_unlock(&r->lock);
    return -1;
}

void registry_set_status(WorkerRegistry *r, uint32_t worker_id, WorkerStatus s) {
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (r->workers[i].sock_fd != -1 && r->workers[i].worker_id == worker_id) {
            r->workers[i].status = s;
            break;
        }
    }
    pthread_mutex_unlock(&r->lock);
}

void registry_set_offline(WorkerRegistry *r, uint32_t worker_id) {
    pthread_mutex_lock(&r->lock);
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (r->workers[i].sock_fd != -1 && r->workers[i].worker_id == worker_id) {
            r->workers[i].status  = WORKER_OFFLINE;
            r->workers[i].sock_fd = -1;
            r->count--;
            break;
        }
    }
    pthread_mutex_unlock(&r->lock);
}

/* Caller must hold r->lock.
 * Selects the idle worker with the fewest completed tasks (least-loaded).
 * Ties are broken by rr_index so no single worker is always preferred. */
int registry_find_idle(WorkerRegistry *r) {
    int best_idx   = -1;
    uint32_t best  = UINT32_MAX;

    for (int i = 0; i < MAX_WORKERS; i++) {
        int idx = (r->rr_index + i) % MAX_WORKERS;
        WorkerInfo *w = &r->workers[idx];
        if (w->sock_fd == -1 || w->status != WORKER_IDLE) continue;
        if (best_idx == -1 || w->tasks_completed < best) {
            best_idx = idx;
            best     = w->tasks_completed;
        }
    }

    if (best_idx != -1)
        r->rr_index = (best_idx + 1) % MAX_WORKERS;

    return best_idx;
}

/* Caller must hold r->lock. */
WorkerInfo *registry_get(WorkerRegistry *r, uint32_t worker_id) {
    for (int i = 0; i < MAX_WORKERS; i++) {
        if (r->workers[i].sock_fd != -1 && r->workers[i].worker_id == worker_id)
            return &r->workers[i];
    }
    return NULL;
}

/* ── Scheduler thread ────────────────────────────────────────────────────── */

static void *scheduler_loop(void *arg) {
    Scheduler *s = (Scheduler *)arg;
    NetworkPayload task;

    while (s->running) {
        /*
         * Use a non-blocking dequeue so scheduler_stop() can end the thread
         * without needing to inject a sentinel task into the queue.
         */
        if (queue_dequeue_nowait(s->queue, &task) != 0) {
            usleep(5000); /* 5 ms — no work to do */
            continue;
        }

        pthread_mutex_lock(&s->registry->lock);
        int idx = registry_find_idle(s->registry);
        if (idx == -1) {
            pthread_mutex_unlock(&s->registry->lock);
            /* No idle worker; put the task back and wait a bit */
            queue_requeue(s->queue, &task);
            usleep(10000); /* 10 ms */
            continue;
        }

        /*
         * Mark the worker busy before sending. If the socket fails afterward,
         * the task is requeued and the worker is removed from scheduling.
         */
        WorkerInfo *w = &s->registry->workers[idx];
        uint32_t completed = w->tasks_completed;
        uint64_t avg_ms = completed > 0 ? w->total_runtime_ms / completed : 0ULL;
        w->status          = WORKER_BUSY;
        w->current_task    = task;
        w->task_started_at = time(NULL);
        int      fd  = w->sock_fd;
        uint32_t wid = w->worker_id;
        pthread_mutex_unlock(&s->registry->lock);

        NetworkPayload pkt = task;
        pkt.type      = MSG_TASK;
        pkt.worker_id = wid;
        payload_to_net(&pkt);

        printf("[%s][scheduler] dispatch: ", now());
        print_task_fields(&task);
        printf(" -> worker=%u worker_completed=%u worker_avg=%llums queue_depth=%d\n",
               wid, completed, (unsigned long long)avg_ms, queue_size(s->queue));

        if (send_full(fd, &pkt, sizeof(pkt)) != 0) {
            /*
             * Send failure means the worker cannot be trusted to complete the
             * task, so preserve at-least-once execution by returning it to FIFO.
             */
            fprintf(stderr, "[%s][scheduler] send to worker %u failed, requeueing task %u\n",
                    now(), wid, task.task_id);
            queue_requeue(s->queue, &task);
            registry_set_offline(s->registry, wid);
        }
    }
    return NULL;
}

/* ── Scheduler lifecycle ─────────────────────────────────────────────────── */

void scheduler_init(Scheduler *s, TaskQueue *q, WorkerRegistry *r) {
    s->queue    = q;
    s->registry = r;
    s->running  = 0;
}

void scheduler_start(Scheduler *s) {
    s->running = 1;
    pthread_create(&s->thread, NULL, scheduler_loop, s);
}

void scheduler_stop(Scheduler *s) {
    if (!s->running)
        return;

    s->running = 0;
    pthread_join(s->thread, NULL);
}
