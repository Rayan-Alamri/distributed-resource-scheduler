#ifndef SERVER_H
#define SERVER_H

#include "scheduler.h"
#include "queue_mgr.h"

#define MASTER_PORT 9090
#define BACKLOG     10

typedef struct {
    int            listen_fd;
    TaskQueue      queue;
    WorkerRegistry registry;
    Scheduler      scheduler;
} MasterState;

/* Initialize server socket, queue, registry, and scheduler. Returns 0 on success. */
int  server_init(MasterState *ms, int port);

/* Block in accept loop, spawning a detached pthread per worker. Never returns normally. */
void server_run(MasterState *ms);

/* Stop scheduler thread and close listen socket. */
void server_shutdown(MasterState *ms);

#endif /* SERVER_H */
