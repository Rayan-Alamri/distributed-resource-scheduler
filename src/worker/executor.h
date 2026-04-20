#ifndef WORKER_EXECUTOR_H
#define WORKER_EXECUTOR_H

#include <stdint.h>
#include "../shared/models.h"

typedef enum {
    EXECUTOR_OK = 0,
    EXECUTOR_ERR_PIPE = -1,
    EXECUTOR_ERR_FORK = -2,
    EXECUTOR_ERR_CHILD_IO = -3,
    EXECUTOR_ERR_CHILD_STATUS = -4
} ExecutorStatus;

int executor_run_task(const NetworkPayload *task, uint32_t *result_out);

#endif /* WORKER_EXECUTOR_H */
