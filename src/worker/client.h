#ifndef WORKER_CLIENT_H
#define WORKER_CLIENT_H

#include <stdint.h>

#define WORKER_DEFAULT_HOST "127.0.0.1"
#define WORKER_DEFAULT_PORT 9090
#define WORKER_DEFAULT_ID 0
#define WORKER_HEARTBEAT_INTERVAL_SEC 2

/* Runtime configuration parsed from argv/environment before connecting. */
typedef struct {
    const char *host;
    int port;
    uint32_t worker_id;
} WorkerConfig;

int worker_connect(const char *host, int port);

/* Connect, register, process tasks until disconnect/shutdown, then drain work. */
int worker_run(const WorkerConfig *config);

#endif /* WORKER_CLIENT_H */
