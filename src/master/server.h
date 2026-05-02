#ifndef SERVER_H
#define SERVER_H

#include "scheduler.h"
#include "queue_mgr.h"

#define MASTER_PORT 9090
#define BACKLOG     10
#define MAX_JOB_TASKS 1000

typedef struct {
    int            active;
    uint32_t       job_id;
    uint32_t       command_code;
    uint32_t       expected;
    uint32_t       completed;
    uint32_t       failed;
    uint64_t       sum_results;
    uint64_t       sum_arguments;
    uint32_t       task_ids[MAX_JOB_TASKS];
    pthread_mutex_t lock;
} JobSummary;

typedef struct {
    int            listen_fd;
    TaskQueue      queue;
    WorkerRegistry registry;
    Scheduler      scheduler;
    JobSummary     job;
} MasterState;

/* Initialize server socket, queue, registry, and scheduler. Returns 0 on success. */
int  server_init(MasterState *ms, int port);

/* Block in accept loop, spawning a detached pthread per worker. Never returns normally. */
void server_run(MasterState *ms);

/* Stop scheduler thread and close listen socket. */
void server_shutdown(MasterState *ms);

#endif /* SERVER_H */
