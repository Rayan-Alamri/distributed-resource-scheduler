#ifndef STATUS_H
#define STATUS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "server.h"

/* Immutable copies used by the dashboard and JSON API after locks are released. */
typedef struct {
    uint32_t       worker_id;
    WorkerStatus   status;
    uint32_t       current_task_id;
    uint32_t       current_command_code;
    uint32_t       current_argument;
    uint32_t       tasks_completed;
    uint64_t       total_runtime_ms;
    uint64_t       avg_runtime_ms;
    uint64_t       current_runtime_ms;
    unsigned long  last_heartbeat_age_sec;
} WorkerSnapshot;

typedef struct {
    int       active;
    uint32_t  job_id;
    uint32_t  command_code;
    uint32_t  expected;
    uint32_t  completed;
    uint32_t  failed;
    uint64_t  sum_results;
    uint64_t  sum_arguments;
    uint64_t  elapsed_ms;
} JobSnapshot;

typedef struct {
    time_t          generated_at;
    int             queue_depth;
    int             worker_count;
    WorkerSnapshot  workers[MAX_WORKERS];
    JobSnapshot     job;
} MasterSnapshot;

/* Copies a thread-safe point-in-time view of the master state for UI/reporting. */
void master_get_snapshot(MasterState *ms, MasterSnapshot *out);

/* Converts a snapshot to JSON. Returns bytes written, excluding the null byte. */
size_t master_snapshot_to_json(const MasterSnapshot *snapshot, char *buf, size_t len);

const char *worker_status_name(WorkerStatus status);
const char *command_name(uint32_t command_code);

#endif /* STATUS_H */
