#include "server.h"
#include "../shared/protocol.h"
#include "../ui/dashboard.h"

/*
 * Master process entry point and orchestration layer.
 *
 * This file owns the listening socket, worker/submit client threads, job
 * summary bookkeeping, dashboard-facing submission API, and video pipeline
 * helpers. Lower-level scheduling and queue operations are delegated to
 * scheduler.c and queue_mgr.c so this file can focus on network lifecycle and
 * job-level coordination.
 */

#include <dirent.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define VIDEO_DIR_DEFAULT "/videos"
#define VIDEO_SEGMENT_SECONDS_DEFAULT 10u

typedef struct {
    unsigned id;
    char path[1024];
} SegmentFile;

static const char *now(void) {
    static char buf[9];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static const char *cmd_name(uint32_t cmd) {
    switch ((CommandCode)cmd) {
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

static void print_task_fields(const NetworkPayload *task) {
    if ((CommandCode)task->command_code == CMD_PRIME_RANGE) {
        printf("task_id=%u cmd=%u(%s) arg=%u range_end=%u",
               task->task_id, task->command_code, cmd_name(task->command_code),
               task->argument, task->result);
    } else if ((CommandCode)task->command_code == CMD_MATRIX_PARALLEL) {
        printf("task_id=%u cmd=%u(%s) row_start=%u matrix_size=%u",
               task->task_id, task->command_code, cmd_name(task->command_code),
               task->argument, task->result);
    } else {
        printf("task_id=%u cmd=%u(%s) arg=%u",
               task->task_id, task->command_code, cmd_name(task->command_code),
               task->argument);
    }
}

static int is_ffmpeg_result_success(uint32_t command_code, uint32_t result) {
    if ((CommandCode)command_code != CMD_FFMPEG_SEGMENT &&
        (CommandCode)command_code != CMD_FFMPEG_SCRIPT)
        return 1;

    return result == FFMPEG_RESULT_SUCCESS;
}

static void cleanup_completed_ffmpeg_segment(const NetworkPayload *task, uint32_t result) {
    char segment_path[512];

    if (!task || !is_ffmpeg_result_success(task->command_code, result))
        return;
    if ((CommandCode)task->command_code != CMD_FFMPEG_SEGMENT &&
        (CommandCode)task->command_code != CMD_FFMPEG_SCRIPT)
        return;

    snprintf(segment_path, sizeof(segment_path), "%s/segments/part_%03u.mp4",
             master_video_dir(), task->argument);

    if (unlink(segment_path) == 0) {
        printf("[%s][master] segment_cleanup: task_id=%u segment=%u path=%s\n",
               now(), task->task_id, task->argument, segment_path);
    } else if (errno != ENOENT) {
        fprintf(stderr, "[%s][master] segment_cleanup_failed: task_id=%u"
                " segment=%u path=%s error=%s\n",
                now(), task->task_id, task->argument, segment_path, strerror(errno));
    }
}

const char *master_video_dir(void) {
    const char *dir = getenv("VIDEO_DIR");

    if (dir && *dir)
        return dir;
    if (access("./videos", R_OK | X_OK) == 0)
        return "./videos";

    return VIDEO_DIR_DEFAULT;
}

static int valid_video_name(const char *name) {
    /*
     * The dashboard accepts a file name from videos/input. Reject path
     * separators so UI input cannot escape the configured video directory.
     */
    return name && *name &&
        strchr(name, '/') == NULL &&
        strchr(name, '\\') == NULL &&
        strcmp(name, ".") != 0 &&
        strcmp(name, "..") != 0;
}

static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

static int remove_matching_files(const char *dir, const char *pattern) {
    DIR *dp = opendir(dir);
    struct dirent *entry;

    if (!dp)
        return 0;

    while ((entry = readdir(dp)) != NULL) {
        char path[512];

        if (fnmatch(pattern, entry->d_name, 0) != 0)
            continue;

        /* Best-effort cleanup: missing files are harmless during reruns. */
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        unlink(path);
    }

    closedir(dp);
    return 0;
}

static int run_process(char *const argv[]) {
    pid_t pid = fork();
    int status = 0;

    if (pid < 0)
        return -1;

    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        return -1;
    }

    if (!WIFEXITED(status))
        return -1;

    return WEXITSTATUS(status);
}

static int parse_part_id(const char *name, unsigned *id_out) {
    unsigned id;
    char extra;

    if (sscanf(name, "part_%u.mp4%c", &id, &extra) != 1)
        return -1;

    *id_out = id;
    return 0;
}

static int count_segment_files(const char *dir) {
    DIR *dp = opendir(dir);
    struct dirent *entry;
    int count = 0;

    if (!dp)
        return 0;

    while ((entry = readdir(dp)) != NULL) {
        unsigned id;

        if (parse_part_id(entry->d_name, &id) == 0)
            count++;
    }

    closedir(dp);
    return count;
}

static int compare_segments(const void *left, const void *right) {
    const SegmentFile *a = (const SegmentFile *)left;
    const SegmentFile *b = (const SegmentFile *)right;

    if (a->id < b->id)
        return -1;
    if (a->id > b->id)
        return 1;
    return 0;
}

static int collect_processed_segments(const char *dir,
                                      SegmentFile *segments,
                                      int max_segments) {
    DIR *dp = opendir(dir);
    struct dirent *entry;
    int count = 0;

    if (!dp)
        return 0;

    while ((entry = readdir(dp)) != NULL && count < max_segments) {
        unsigned id;

        if (parse_part_id(entry->d_name, &id) != 0)
            continue;

        segments[count].id = id;
        snprintf(segments[count].path, sizeof(segments[count].path),
                 "%s/%s", dir, entry->d_name);
        count++;
    }

    closedir(dp);
    qsort(segments, (size_t)count, sizeof(segments[0]), compare_segments);
    return count;
}

static void input_stem(const char *input_name, char *stem, size_t stem_len) {
    const char *dot;

    snprintf(stem, stem_len, "%s", input_name);
    dot = strrchr(stem, '.');
    if (dot && dot != stem)
        stem[dot - stem] = '\0';
}

static void job_init(JobSummary *job) {
    memset(job, 0, sizeof(*job));
    pthread_mutex_init(&job->lock, NULL);
}

static void job_destroy(JobSummary *job) {
    pthread_mutex_destroy(&job->lock);
}

static void job_start(JobSummary *job, uint32_t command_code) {
    pthread_mutex_lock(&job->lock);
    job->active = 1;
    job->job_id++;
    job->command_code = command_code;
    job->expected = 0;
    job->completed = 0;
    job->failed = 0;
    job->sum_results = 0;
    job->sum_arguments = 0;
    job->started_at_ms = monotonic_ms();
    job->completed_at_ms = 0;
    memset(job->task_ids, 0, sizeof(job->task_ids));
    pthread_mutex_unlock(&job->lock);
}

static void job_add_task(JobSummary *job, const NetworkPayload *task) {
    pthread_mutex_lock(&job->lock);
    if (job->active && job->expected < MAX_JOB_TASKS) {
        job->task_ids[job->expected] = task->task_id;
        job->expected++;
        job->sum_arguments += task->argument;
    }
    pthread_mutex_unlock(&job->lock);
}

static int job_contains_task(const JobSummary *job, uint32_t task_id) {
    for (uint32_t i = 0; i < job->expected; i++) {
        if (job->task_ids[i] == task_id)
            return 1;
    }
    return 0;
}

static void job_record_result(JobSummary *job, const NetworkPayload *task, uint32_t result) {
    pthread_mutex_lock(&job->lock);
    if (!job->active || !job_contains_task(job, task->task_id)) {
        pthread_mutex_unlock(&job->lock);
        return;
    }

    job->completed++;
    job->sum_results += result;
    if (!is_ffmpeg_result_success(task->command_code, result))
        job->failed++;

    if (job->expected > 0 && job->completed >= job->expected) {
        uint64_t elapsed_ms;

        job->completed_at_ms = monotonic_ms();
        elapsed_ms = job->completed_at_ms >= job->started_at_ms
            ? job->completed_at_ms - job->started_at_ms
            : 0;

        printf("[%s][master] job_complete: job=%u cmd=%u(%s)"
               " tasks=%u failed=%u sum_results=%llu elapsed=%llums",
               now(), job->job_id, job->command_code, cmd_name(job->command_code),
               job->completed, job->failed,
               (unsigned long long)job->sum_results,
               (unsigned long long)elapsed_ms);

        if ((CommandCode)job->command_code == CMD_MONTE_CARLO && job->sum_arguments > 0) {
            double pi = 4.0 * (double)job->sum_results / (double)job->sum_arguments;
            printf(" pi_estimate=%.6f", pi);
        } else if ((CommandCode)job->command_code == CMD_PRIME_RANGE) {
            printf(" total_primes=%llu", (unsigned long long)job->sum_results);
        } else if ((CommandCode)job->command_code == CMD_MATRIX_PARALLEL) {
            printf(" matrix_checksum=%llu", (unsigned long long)job->sum_results);
        } else if ((CommandCode)job->command_code == CMD_FFMPEG_SEGMENT ||
                   (CommandCode)job->command_code == CMD_FFMPEG_SCRIPT) {
            printf(" successful_segments=%u", job->completed - job->failed);
        }

        printf("\n");
        job->active = 0;
    }
    pthread_mutex_unlock(&job->lock);
}

/* ── Per-worker thread ───────────────────────────────────────────────────── */

/* Dashboard submission API. */
int master_submit_tasks(MasterState *ms,
                        uint32_t command_code,
                        uint32_t count,
                        uint32_t argument,
                        uint32_t step,
                        uint32_t range_end,
                        uint32_t start_id) {
    uint32_t submitted = 0;

    if (!ms || command_code < CMD_PRIME || command_code > CMD_MATRIX_PARALLEL ||
        count == 0 || count > MAX_JOB_TASKS)
        return -1;

    job_start(&ms->job, command_code);

    for (uint32_t i = 0; i < count; i++) {
        NetworkPayload task;

        memset(&task, 0, sizeof(task));
        task.type = MSG_TASK;
        task.task_id = start_id + i;
        task.command_code = command_code;
        task.argument = argument + i * step;

        if (command_code == CMD_PRIME_RANGE) {
            task.result = step > 0
                ? task.argument + step - 1
                : range_end;
        } else if (command_code == CMD_MATRIX_PARALLEL) {
            task.result = range_end;
        }

        if (queue_enqueue(&ms->queue, &task) == 0) {
            submitted++;
            job_add_task(&ms->job, &task);
            printf("[%s][dashboard] task_submitted: ", now());
            print_task_fields(&task);
            printf(" queue_depth=%d\n", queue_size(&ms->queue));
        } else {
            fprintf(stderr, "[%s][dashboard] queue failed for task_id=%u\n",
                    now(), task.task_id);
        }
    }

    if (submitted == 0) {
        pthread_mutex_lock(&ms->job.lock);
        ms->job.active = 0;
        pthread_mutex_unlock(&ms->job.lock);
    }

    return (int)submitted;
}

int master_process_video(MasterState *ms,
                         const char *input_name,
                         uint32_t segment_seconds,
                         uint32_t start_id,
                         uint32_t *segment_count_out,
                         char *message,
                         size_t message_len) {
    const char *vdir = master_video_dir();
    char input[512];
    char segments_dir[512];
    char processed_dir[512];
    char final_dir[512];
    char output_pattern[1024];
    char segment_time[16];
    char last_input_path[512];
    FILE *last_input;
    int segment_count;
    int submitted;

    if (segment_count_out)
        *segment_count_out = 0;

    if (!ms || !valid_video_name(input_name)) {
        snprintf(message, message_len, "Invalid video name.");
        return -1;
    }

    if (segment_seconds == 0)
        segment_seconds = VIDEO_SEGMENT_SECONDS_DEFAULT;

    snprintf(input, sizeof(input), "%s/input/%s", vdir, input_name);
    snprintf(segments_dir, sizeof(segments_dir), "%s/segments", vdir);
    snprintf(processed_dir, sizeof(processed_dir), "%s/processed", vdir);
    snprintf(final_dir, sizeof(final_dir), "%s/final", vdir);
    snprintf(output_pattern, sizeof(output_pattern), "%s/part_%%03d.mp4", segments_dir);

    if (access(input, R_OK) != 0) {
        snprintf(message, message_len, "Video not found: %s", input_name);
        return -1;
    }

    if (ensure_dir(segments_dir) != 0 ||
        ensure_dir(processed_dir) != 0 ||
        ensure_dir(final_dir) != 0) {
        snprintf(message, message_len, "Could not create video directories.");
        return -1;
    }

    remove_matching_files(segments_dir, "part_*.mp4");
    remove_matching_files(processed_dir, "part_*.mp4");
    remove_matching_files(processed_dir, ".part_*.tmp.*.mp4");

    snprintf(segment_time, sizeof(segment_time), "%u", segment_seconds);
    char *argv[] = {
        "ffmpeg", "-y",
        "-i", input,
        "-c", "copy",
        "-map", "0",
        "-segment_time", segment_time,
        "-f", "segment",
        output_pattern,
        NULL
    };

    if (run_process(argv) != 0) {
        snprintf(message, message_len, "ffmpeg split failed for %s.", input_name);
        return -1;
    }

    segment_count = count_segment_files(segments_dir);
    if (segment_count <= 0) {
        snprintf(message, message_len, "No segments were created.");
        return -1;
    }
    if (segment_count > MAX_JOB_TASKS) {
        snprintf(message, message_len, "Too many segments: %d.", segment_count);
        return -1;
    }

    snprintf(last_input_path, sizeof(last_input_path), "%s/.last_input_name", vdir);
    last_input = fopen(last_input_path, "w");
    if (last_input) {
        fprintf(last_input, "%s\n", input_name);
        fclose(last_input);
    }

    submitted = master_submit_tasks(ms, CMD_FFMPEG_SEGMENT,
                                    (uint32_t)segment_count,
                                    0, 1, 0, start_id);
    if (submitted != segment_count) {
        snprintf(message, message_len, "Queued %d of %d segments.",
                 submitted, segment_count);
        return -1;
    }

    if (segment_count_out)
        *segment_count_out = (uint32_t)segment_count;
    snprintf(message, message_len, "Queued %d segment task(s) for %s.",
             segment_count, input_name);
    return 0;
}

int master_merge_video(const char *input_name,
                       char *output_path,
                       size_t output_path_len,
                       char *message,
                       size_t message_len) {
    const char *vdir = master_video_dir();
    SegmentFile segments[MAX_JOB_TASKS];
    char processed_dir[512];
    char final_dir[512];
    char list_path[1024];
    char output[1024];
    char stem[256];
    FILE *list;
    int count;

    if (!valid_video_name(input_name)) {
        snprintf(message, message_len, "Invalid video name.");
        return -1;
    }

    snprintf(processed_dir, sizeof(processed_dir), "%s/processed", vdir);
    snprintf(final_dir, sizeof(final_dir), "%s/final", vdir);
    snprintf(list_path, sizeof(list_path), "%s/list.txt", final_dir);
    input_stem(input_name, stem, sizeof(stem));
    snprintf(output, sizeof(output), "%s/%s_modified.mp4", final_dir, stem);

    if (ensure_dir(final_dir) != 0) {
        snprintf(message, message_len, "Could not create final directory.");
        return -1;
    }

    count = collect_processed_segments(processed_dir, segments, MAX_JOB_TASKS);
    if (count <= 0) {
        snprintf(message, message_len, "No processed segments to merge.");
        return -1;
    }

    list = fopen(list_path, "w");
    if (!list) {
        snprintf(message, message_len, "Could not write merge list.");
        return -1;
    }

    for (int i = 0; i < count; i++) {
        char absolute_path[1024];
        const char *path = realpath(segments[i].path, absolute_path)
            ? absolute_path
            : segments[i].path;

        fprintf(list, "file '%s'\n", path);
    }
    fclose(list);

    char *argv[] = {
        "ffmpeg", "-y",
        "-f", "concat",
        "-safe", "0",
        "-i", list_path,
        "-c", "copy",
        output,
        NULL
    };

    if (run_process(argv) != 0) {
        snprintf(message, message_len, "ffmpeg merge failed.");
        return -1;
    }

    if (output_path && output_path_len > 0)
        snprintf(output_path, output_path_len, "%s", output);
    snprintf(message, message_len, "Merged output: %s", output);
    return 0;
}

/* Per-worker thread. */
/**
 * WorkerArg struct holds arguments for the worker thread.
 * - sock_fd: Socket file descriptor for communication with the worker.
 * - worker_id: Unique ID assigned to the worker.
 * - ms: Pointer to the MasterState for accessing shared resources.
 */
typedef struct {
    int          sock_fd;
    uint32_t     worker_id;
    MasterState *ms;
} WorkerArg;

/**
 * worker_thread function handles communication with a single worker.
 * It processes incoming messages (heartbeats, results), manages task assignment,
 * and handles disconnection by requeueing orphaned tasks.
 *
 * @param arg Pointer to WorkerArg struct.
 * @return NULL
 */
static void *worker_thread(void *arg) {
    WorkerArg   *wa  = (WorkerArg *)arg;
    int          fd  = wa->sock_fd;
    uint32_t     wid = wa->worker_id;
    MasterState *ms  = wa->ms;
    free(wa);

    NetworkPayload pkt;
    while (recv_full(fd, &pkt, sizeof(pkt)) == 0) {
        /*
         * Every socket read arrives in network byte order. Convert once at the
         * boundary so the rest of the master logic can compare normal integers.
         */
        payload_to_host(&pkt);

        switch ((MessageType)pkt.type) {

        case MSG_HEARTBEAT:
            /*
             * Heartbeats are lightweight liveness updates. The scheduler and
             * dashboard read the same registry, so writes stay under the lock.
             */
            pthread_mutex_lock(&ms->registry.lock);
            {
                WorkerInfo *w = registry_get(&ms->registry, wid);
                if (w)
                    w->last_heartbeat = time(NULL);
            }
            pthread_mutex_unlock(&ms->registry.lock);
            break;

        case MSG_RESULT:
            /*
             * A result completes the worker's current task. Update runtime
             * metrics, then clear the task snapshot so a later disconnect does
             * not requeue finished work.
             */
            pthread_mutex_lock(&ms->registry.lock);
            {
                WorkerInfo *w = registry_get(&ms->registry, wid);
                if (w) {
                    time_t t = time(NULL);
                    NetworkPayload finished_task = w->current_task;
                    uint64_t runtime_ms = 0;
                    if (w->task_started_at > 0)
                        runtime_ms = (uint64_t)(t - w->task_started_at) * 1000ULL;
                    w->total_runtime_ms += runtime_ms;
                    w->tasks_completed++;
                    w->task_started_at = 0;
                    w->status = WORKER_IDLE;
                    memset(&w->current_task, 0, sizeof(NetworkPayload));

                    uint64_t avg_ms = w->tasks_completed > 0
                        ? w->total_runtime_ms / w->tasks_completed : 0ULL;
                    printf("[%s][master] worker_finished: worker=%u task_id=%u"
                           " cmd=%u(%s) arg=%u result=%u runtime=%llums"
                           " completed=%u avg=%llums status=idle\n",
                           now(), wid, pkt.task_id,
                           finished_task.command_code,
                           cmd_name(finished_task.command_code),
                           finished_task.argument, pkt.result,
                           (unsigned long long)runtime_ms,
                           w->tasks_completed, (unsigned long long)avg_ms);
                    job_record_result(&ms->job, &finished_task, pkt.result);
                    cleanup_completed_ffmpeg_segment(&finished_task, pkt.result);
                }
            }
            pthread_mutex_unlock(&ms->registry.lock);
            break;

        default:
            fprintf(stderr, "[%s][master] unexpected msg type %u from worker %u\n",
                    now(), pkt.type, wid);
            break;
        }
    }

    /*
     * Worker disconnected. If it was busy, move its task back into the FIFO so
     * another worker can complete it. The registry lock is released before
     * queue_requeue() to avoid holding two subsystem locks at the same time.
     */
    fprintf(stderr, "[%s][master] worker %u disconnected\n", now(), wid);
    pthread_mutex_lock(&ms->registry.lock);
    {
        WorkerInfo *dead = registry_get(&ms->registry, wid);
        if (dead && dead->status == WORKER_BUSY && dead->current_task.task_id != 0) {
            NetworkPayload orphan = dead->current_task;
            pthread_mutex_unlock(&ms->registry.lock);
            fprintf(stderr, "[%s][master] requeueing orphaned task %u\n", now(), orphan.task_id);
            queue_requeue(&ms->queue, &orphan);
        } else {
            pthread_mutex_unlock(&ms->registry.lock);
        }
    }
    registry_set_offline(&ms->registry, wid);
    close(fd);
    return NULL;
}

/* ── Server lifecycle ────────────────────────────────────────────────────── */

int server_init(MasterState *ms, int port) {
    queue_init(&ms->queue);
    registry_init(&ms->registry);
    scheduler_init(&ms->scheduler, &ms->queue, &ms->registry);
    job_init(&ms->job);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    ms->listen_fd = fd;
    printf("[%s][master] listening on port %d\n", now(), port);
    return 0;
}

void server_run(MasterState *ms) {
    scheduler_start(&ms->scheduler);

    uint32_t next_worker_id = 1;
    struct sockaddr_in client_addr;
    socklen_t addrlen = sizeof(client_addr);

    while (1) {
        int cfd = accept(ms->listen_fd, (struct sockaddr *)&client_addr, &addrlen);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }

        /*
         * The first packet must be a registration handshake. This keeps random
         * TCP clients from being added to the worker registry accidentally.
         */
        NetworkPayload reg;
        if (recv_full(cfd, &reg, sizeof(reg)) != 0) {
            fprintf(stderr, "[%s][master] registration recv failed, dropping connection\n", now());
            close(cfd);
            continue;
        }
        payload_to_host(&reg);

        if ((MessageType)reg.type == MSG_SUBMIT) {
            /*
             * Submit clients are never added to the worker registry. Read their
             * MSG_TASK packets directly here and enqueue each one, then close.
             */
            printf("[%s][master] submit client connected from %s\n",
                   now(), inet_ntoa(client_addr.sin_addr));
            NetworkPayload task;
            unsigned submitted = 0;
            int job_started = 0;
            while (recv_full(cfd, &task, sizeof(task)) == 0) {
                payload_to_host(&task);
                if ((MessageType)task.type == MSG_TASK) {
                    if (!job_started) {
                        job_start(&ms->job, task.command_code);
                        job_started = 1;
                    }
                    int status = queue_enqueue(&ms->queue, &task);
                    int depth = queue_size(&ms->queue);
                    if (status == 0) {
                        submitted++;
                        job_add_task(&ms->job, &task);
                        printf("[%s][master] task_submitted: source=%s ",
                               now(), inet_ntoa(client_addr.sin_addr));
                        print_task_fields(&task);
                        printf(" queue_depth=%d\n", depth);
                    } else {
                        fprintf(stderr, "[%s][master] queue full/allocation failed for task_id=%u\n",
                                now(), task.task_id);
                    }
                }
            }
            printf("[%s][master] submit client disconnected after %u task(s)\n",
                   now(), submitted);
            close(cfd);
            continue;
        }

        if ((MessageType)reg.type != MSG_REGISTER) {
            fprintf(stderr, "[%s][master] expected MSG_REGISTER, got type %u — dropping\n",
                    now(), reg.type);
            close(cfd);
            continue;
        }

        uint32_t wid = next_worker_id++;
        if (registry_add(&ms->registry, wid, cfd) < 0) {
            fprintf(stderr, "[%s][master] registry full — rejecting worker\n", now());
            close(cfd);
            continue;
        }
        printf("[%s][master] worker %u registered from %s\n",
               now(), wid, inet_ntoa(client_addr.sin_addr));

        WorkerArg *wa = malloc(sizeof(WorkerArg));
        if (!wa) {
            fprintf(stderr, "[%s][master] malloc failed for WorkerArg\n", now());
            close(cfd);
            continue;
        }
        wa->sock_fd   = cfd;
        wa->worker_id = wid;
        wa->ms        = ms;

        /*
         * Each worker gets a detached reader thread. The main server loop stays
         * dedicated to accepting new workers while these threads handle results.
         */
        pthread_t       tid;
        pthread_attr_t  attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, worker_thread, wa);
        pthread_attr_destroy(&attr);
    }
}

void server_shutdown(MasterState *ms) {
    scheduler_stop(&ms->scheduler);
    close(ms->listen_fd);
    queue_destroy(&ms->queue);
    job_destroy(&ms->job);
}

/* ── Demo task seeder ────────────────────────────────────────────────────── */

static void *seed_demo_tasks(void *arg) {
    MasterState *ms = (MasterState *)arg;
    const char *env = getenv("DEMO_TASKS");
    int count = 6;

    if (env && *env != '\0') {
        int n = atoi(env);
        if (n >= 0 && n <= 100)
            count = n;
    }

    if (count == 0)
        return NULL; /* DEMO_TASKS=0 suppresses seeding */

    /* Wait for workers to connect before flooding the queue */
    printf("[%s][demo] will enqueue %d tasks in 3 seconds...\n", now(), count);
    sleep(3);

    for (int i = 0; i < count; i++) {
        NetworkPayload task;
        memset(&task, 0, sizeof(task));
        task.type         = MSG_TASK;
        task.task_id      = (uint32_t)(i + 1);
        task.command_code = 1;  /* all prime tasks — slow enough to observe */
        task.argument     = 5000000 + i * 1000000;
        queue_enqueue(&ms->queue, &task);
        printf("[%s][demo] enqueued task %d: cmd=%u arg=%u\n",
               now(), i + 1, task.command_code, task.argument);
    }
    printf("[%s][demo] all %d tasks enqueued\n", now(), count);
    return NULL;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

static void *server_thread_main(void *arg) {
    server_run((MasterState *)arg);
    return NULL;
}

static int env_is_false(const char *value) {
    return value && (
        strcmp(value, "0") == 0 ||
        strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "off") == 0 ||
        strcasecmp(value, "no") == 0);
}

static int dashboard_requested(void) {
    const char *env = getenv("MASTER_DASHBOARD");

    if (env && *env != '\0')
        return !env_is_false(env);

    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

static void redirect_logs_for_dashboard(void) {
    const char *path = getenv("MASTER_LOG_FILE");

    if (!path || *path == '\0')
        path = "logs/master.log";

    mkdir("logs", 0755);

    if (freopen(path, "a", stdout))
        setvbuf(stdout, NULL, _IONBF, 0);
    if (freopen(path, "a", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);

    printf("[%s][master] dashboard mode enabled; logging to %s\n", now(), path);
}

int main(void) {
    MasterState ms;
    int port = MASTER_PORT;
    const char *port_env = getenv("MASTER_PORT");
    int use_dashboard;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (port_env && *port_env != '\0') {
        int p = atoi(port_env);
        if (p > 0 && p <= 65535)
            port = p;
    }
    if (server_init(&ms, port) != 0)
        return 1;

    use_dashboard = dashboard_requested();
    if (use_dashboard && dashboard_init(&ms) == 0) {
        pthread_t server_tid;

        redirect_logs_for_dashboard();
        if (pthread_create(&server_tid, NULL, server_thread_main, &ms) != 0) {
            fprintf(stderr, "[%s][master] failed to start server thread\n", now());
            dashboard_stop();
            server_shutdown(&ms);
            return 1;
        }
        pthread_detach(server_tid);

        /* If DEMO_TASKS is set, spawn a thread that seeds sample tasks */
        if (getenv("DEMO_TASKS")) {
            pthread_t       demo_tid;
            pthread_attr_t  demo_attr;
            pthread_attr_init(&demo_attr);
            pthread_attr_setdetachstate(&demo_attr, PTHREAD_CREATE_DETACHED);
            pthread_create(&demo_tid, &demo_attr, seed_demo_tasks, &ms);
            pthread_attr_destroy(&demo_attr);
        }

        dashboard_run();
        dashboard_stop();
        server_shutdown(&ms);
        return 0;
    }

    if (use_dashboard)
        fprintf(stderr, "[%s][master] dashboard unavailable; using log mode\n", now());

    /* If DEMO_TASKS is set, spawn a thread that seeds sample tasks */
    if (getenv("DEMO_TASKS")) {
        pthread_t       demo_tid;
        pthread_attr_t  demo_attr;
        pthread_attr_init(&demo_attr);
        pthread_attr_setdetachstate(&demo_attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&demo_tid, &demo_attr, seed_demo_tasks, &ms);
        pthread_attr_destroy(&demo_attr);
    }

    server_run(&ms);
    server_shutdown(&ms);
    return 0;
}
