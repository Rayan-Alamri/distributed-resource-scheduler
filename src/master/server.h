#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>

#include "scheduler.h"
#include "queue_mgr.h"

/*
 * MasterState is the shared root object for the master process. Each subsystem
 * owns its own lock internally, so callers should use the public APIs below
 * rather than reaching into nested queue/registry/job fields directly.
 */

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
    uint64_t       started_at_ms;
    uint64_t       completed_at_ms;
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

typedef enum {
    VIDEO_PROCESS_SEGMENTS,
    VIDEO_PROCESS_TIME_RANGE
} VideoProcessMode;

/* Initialize server socket, queue, registry, and scheduler. Returns 0 on success. */
int  server_init(MasterState *ms, int port);

/* Block in accept loop, spawning a detached pthread per worker. Never returns normally. */
void server_run(MasterState *ms);

/* Stop scheduler thread and close listen socket. */
void server_shutdown(MasterState *ms);

/* Submit a batch directly from in-process UI code. Returns queued task count. */
int master_submit_tasks(MasterState *ms,
                        uint32_t command_code,
                        uint32_t count,
                        uint32_t argument,
                        uint32_t step,
                        uint32_t range_end,
                        uint32_t start_id);

const char *master_video_dir(void);

int master_process_video(MasterState *ms,
                         const char *input_name,
                         VideoProcessMode mode,
                         uint32_t segment_seconds,
                         uint32_t start_id,
                         uint32_t *segment_count_out,
                         char *message,
                         size_t message_len);

int master_merge_video(const char *input_name,
                       char *output_path,
                       size_t output_path_len,
                       char *message,
                       size_t message_len);

#endif /* SERVER_H */
