#include "status.h"

/*
 * Read-only status projection for the dashboard and optional JSON clients.
 *
 * The master has multiple writer threads, so UI code must not walk live queue,
 * registry, or job structures directly. This module copies the needed fields
 * under their locks into a compact snapshot that can be rendered without
 * blocking scheduler or worker threads.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

const char *worker_status_name(WorkerStatus status) {
    switch (status) {
    case WORKER_IDLE:    return "idle";
    case WORKER_BUSY:    return "busy";
    case WORKER_OFFLINE: return "offline";
    default:             return "unknown";
    }
}

const char *command_name(uint32_t command_code) {
    switch ((CommandCode)command_code) {
    case CMD_PRIME:          return "prime";
    case CMD_MATRIX:         return "matrix";
    case CMD_PRIME_RANGE:    return "prime_range";
    case CMD_MONTE_CARLO:    return "monte_carlo";
    case CMD_MANDELBROT:     return "mandelbrot";
    case CMD_FFMPEG_SEGMENT: return "ffmpeg_segment";
    case CMD_FFMPEG_SCRIPT:  return "ffmpeg_script";
    case CMD_MATRIX_PARALLEL: return "matrix_parallel";
    default:                 return "unknown";
    }
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void master_get_snapshot(MasterState *ms, MasterSnapshot *out) {
    time_t now = time(NULL);
    uint64_t now_ms = monotonic_ms();

    memset(out, 0, sizeof(*out));
    out->generated_at = now;
    out->queue_depth = queue_size(&ms->queue);

    /*
     * Copy worker rows while holding the registry lock, then release it before
     * formatting or rendering. This keeps the UI responsive and avoids lock
     * chains with the scheduler.
     */
    pthread_mutex_lock(&ms->registry.lock);
    for (int i = 0; i < MAX_WORKERS; i++) {
        WorkerInfo *w = &ms->registry.workers[i];
        if (w->worker_id == 0)
            continue;

        WorkerSnapshot *dst = &out->workers[out->worker_count++];
        dst->worker_id = w->worker_id;
        dst->status = w->status;
        dst->current_task_id = w->current_task.task_id;
        dst->current_command_code = w->current_task.command_code;
        dst->current_argument = w->current_task.argument;
        dst->tasks_completed = w->tasks_completed;
        dst->total_runtime_ms = w->total_runtime_ms;
        dst->avg_runtime_ms = w->tasks_completed > 0
            ? w->total_runtime_ms / w->tasks_completed
            : 0;
        dst->current_runtime_ms = w->task_started_at > 0
            ? (uint64_t)(now - w->task_started_at) * 1000ULL
            : 0;
        dst->last_heartbeat_age_sec = w->last_heartbeat > 0
            ? (unsigned long)(now - w->last_heartbeat)
            : 0;
    }
    pthread_mutex_unlock(&ms->registry.lock);

    /* Job counters are protected separately from the worker registry. */
    pthread_mutex_lock(&ms->job.lock);
    out->job.active = ms->job.active;
    out->job.job_id = ms->job.job_id;
    out->job.command_code = ms->job.command_code;
    out->job.expected = ms->job.expected;
    out->job.completed = ms->job.completed;
    out->job.failed = ms->job.failed;
    out->job.sum_results = ms->job.sum_results;
    out->job.sum_arguments = ms->job.sum_arguments;
    if (ms->job.started_at_ms > 0) {
        uint64_t end_ms = ms->job.active ? now_ms : ms->job.completed_at_ms;
        out->job.elapsed_ms = end_ms >= ms->job.started_at_ms
            ? end_ms - ms->job.started_at_ms
            : 0;
    }
    pthread_mutex_unlock(&ms->job.lock);
}

static size_t append_json(char *buf, size_t len, size_t used, const char *fmt, ...) {
    va_list ap;
    int written;

    if (used >= len)
        return used;

    va_start(ap, fmt);
    written = vsnprintf(buf + used, len - used, fmt, ap);
    va_end(ap);

    if (written < 0)
        return used;

    /* Report truncation by returning len; the final caller null-terminates. */
    if ((size_t)written >= len - used)
        return len;

    return used + (size_t)written;
}

size_t master_snapshot_to_json(const MasterSnapshot *snapshot, char *buf, size_t len) {
    size_t used = 0;

    if (len == 0)
        return 0;

    used = append_json(buf, len, used,
        "{\"generated_at\":%lld,\"queue_depth\":%d,\"workers\":[",
        (long long)snapshot->generated_at, snapshot->queue_depth);

    for (int i = 0; i < snapshot->worker_count; i++) {
        const WorkerSnapshot *w = &snapshot->workers[i];
        used = append_json(buf, len, used,
            "%s{\"worker_id\":%u,\"status\":\"%s\","
            "\"current_task_id\":%u,\"current_command\":\"%s\","
            "\"current_argument\":%u,\"tasks_completed\":%u,"
            "\"total_runtime_ms\":%llu,\"avg_runtime_ms\":%llu,"
            "\"current_runtime_ms\":%llu,\"last_heartbeat_age_sec\":%lu}",
            i == 0 ? "" : ",",
            w->worker_id,
            worker_status_name(w->status),
            w->current_task_id,
            command_name(w->current_command_code),
            w->current_argument,
            w->tasks_completed,
            (unsigned long long)w->total_runtime_ms,
            (unsigned long long)w->avg_runtime_ms,
            (unsigned long long)w->current_runtime_ms,
            w->last_heartbeat_age_sec);
    }

    used = append_json(buf, len, used,
        "],\"job\":{\"active\":%s,\"job_id\":%u,\"command\":\"%s\","
        "\"expected\":%u,\"completed\":%u,\"failed\":%u,"
        "\"sum_results\":%llu,\"sum_arguments\":%llu,"
        "\"elapsed_ms\":%llu}}",
        snapshot->job.active ? "true" : "false",
        snapshot->job.job_id,
        command_name(snapshot->job.command_code),
        snapshot->job.expected,
        snapshot->job.completed,
        snapshot->job.failed,
        (unsigned long long)snapshot->job.sum_results,
        (unsigned long long)snapshot->job.sum_arguments,
        (unsigned long long)snapshot->job.elapsed_ms);

    if (used >= len)
        buf[len - 1] = '\0';

    return used < len ? used : len - 1;
}
