#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <pthread.h>
#include <time.h>
#include "queue_mgr.h"
#include "../shared/models.h"

typedef struct {
    uint32_t       worker_id;
    int            sock_fd;        /* -1 if slot is free */
    WorkerStatus   status;
    NetworkPayload current_task;   /* full task snapshot for requeue on disconnect */
    time_t         last_heartbeat;
} WorkerInfo;

typedef struct {
    WorkerInfo      workers[MAX_WORKERS];
    int             count;
    int             rr_index;      /* round-robin cursor, always under lock */
    pthread_mutex_t lock;
} WorkerRegistry;

typedef struct {
    TaskQueue      *queue;
    WorkerRegistry *registry;
    volatile int    running;
    pthread_t       thread;
} Scheduler;

/* WorkerRegistry API */
void        registry_init(WorkerRegistry *r);
int         registry_add(WorkerRegistry *r, uint32_t worker_id, int sock_fd);
void        registry_set_status(WorkerRegistry *r, uint32_t worker_id, WorkerStatus s);
void        registry_set_offline(WorkerRegistry *r, uint32_t worker_id);

/* Returns slot index of next idle worker (round-robin), or -1. Caller must hold r->lock. */
int         registry_find_idle(WorkerRegistry *r);

/* Lookup by worker_id. Caller must hold r->lock. Returns NULL if not found. */
WorkerInfo *registry_get(WorkerRegistry *r, uint32_t worker_id);

/* Scheduler API */
void scheduler_init(Scheduler *s, TaskQueue *q, WorkerRegistry *r);
void scheduler_start(Scheduler *s);
void scheduler_stop(Scheduler *s);

#endif /* SCHEDULER_H */
