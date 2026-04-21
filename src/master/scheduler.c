#include "scheduler.h"
#include "../shared/protocol.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

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

/* Caller must hold r->lock. Advances rr_index for round-robin fairness. */
int registry_find_idle(WorkerRegistry *r) {
    for (int i = 0; i < MAX_WORKERS; i++) {
        int idx = (r->rr_index + i) % MAX_WORKERS;
        if (r->workers[idx].sock_fd != -1 && r->workers[idx].status == WORKER_IDLE) {
            r->rr_index = (idx + 1) % MAX_WORKERS;
            return idx;
        }
    }
    return -1;
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
        w->status       = WORKER_BUSY;
        w->current_task = task;
        int      fd  = w->sock_fd;
        uint32_t wid = w->worker_id;
        pthread_mutex_unlock(&s->registry->lock);

        NetworkPayload pkt = task;
        pkt.type      = MSG_TASK;
        pkt.worker_id = wid;
        payload_to_net(&pkt);

        printf("[scheduler] task=%u cmd=%u arg=%u -> worker=%u\n",
               task.task_id, task.command_code, task.argument, wid);

        if (send_full(fd, &pkt, sizeof(pkt)) != 0) {
            /*
             * Send failure means the worker cannot be trusted to complete the
             * task, so preserve at-least-once execution by returning it to FIFO.
             */
            fprintf(stderr, "[scheduler] send to worker %u failed, requeueing task %u\n",
                    wid, task.task_id);
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
    s->running = 0;
    pthread_join(s->thread, NULL);
}
