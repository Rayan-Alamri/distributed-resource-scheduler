#ifndef MODELS_H
#define MODELS_H

#include <stdint.h>
#include <time.h>

typedef enum {
    MSG_HEARTBEAT = 0,
    MSG_TASK      = 1,
    MSG_RESULT    = 2,
    MSG_REGISTER  = 3
} MessageType;

typedef enum {
    WORKER_IDLE    = 0,
    WORKER_BUSY    = 1,
    WORKER_OFFLINE = 2
} WorkerStatus;

/* Fixed-size network payload — all fields in network byte order on the wire */
typedef struct {
    uint32_t type;          /* MessageType */
    uint32_t worker_id;
    uint32_t task_id;
    uint32_t command_code;  /* 1=prime, 2=matrix, etc. */
    uint32_t argument;
    uint32_t result;        /* 0 if not a result message */
} NetworkPayload;

#define MAX_WORKERS 32

#endif /* MODELS_H */
