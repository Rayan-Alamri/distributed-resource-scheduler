#include "executor.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../shared/models.h"

#define FFMPEG_VIDEO_DIR_DEFAULT "/videos"
#define FFMPEG_TIMEOUT_SEC       120

/* Reads VIDEO_DIR env var at runtime; falls back to /videos (the Docker path). */
static const char *video_dir(void) {
    const char *d = getenv("VIDEO_DIR");

    if (d && *d)
        return d;
    if (access("./videos", R_OK | X_OK) == 0)
        return "./videos";

    return FFMPEG_VIDEO_DIR_DEFAULT;
}
#define MANDELBROT_WIDTH     800
#define MANDELBROT_HEIGHT    600
#define MANDELBROT_MAX_ITER  100
#define MANDELBROT_ROW_CHUNK 100
#define MATRIX_ROW_CHUNK     100

/* ── Algorithmic workloads ─────────────────────────────────────────────────── */

static uint32_t count_primes(uint32_t limit) {
    uint32_t count = 0;
    for (uint32_t n = 2; n <= limit; n++) {
        int is_prime = 1;
        for (uint32_t d = 2; d <= n / d; d++) {
            if (n % d == 0) { is_prime = 0; break; }
        }
        if (is_prime) count++;
    }
    return count;
}

static uint32_t count_primes_range(uint32_t range_start, uint32_t range_end) {
    uint32_t count = 0;
    if (range_start < 2) range_start = 2;
    for (uint32_t n = range_start; n <= range_end; n++) {
        int is_prime = 1;
        for (uint32_t d = 2; d <= n / d; d++) {
            if (n % d == 0) { is_prime = 0; break; }
        }
        if (is_prime) count++;
    }
    return count;
}

static uint32_t matrix_checksum(uint32_t size) {
    if (size == 0) return 0;
    uint32_t checksum = 0;
    for (uint32_t row = 0; row < size; row++) {
        for (uint32_t col = 0; col < size; col++) {
            uint32_t a = (row + 1U) * (col + 3U);
            uint32_t b = (col + 1U) * (row + 5U);
            checksum += a * b;
        }
    }
    return checksum;
}

static uint32_t matrix_chunk_checksum(uint32_t row_start, uint32_t size) {
    if (size == 0 || row_start >= size) return 0;

    uint32_t checksum = 0;
    uint32_t row_end = row_start + MATRIX_ROW_CHUNK;
    if (row_end > size)
        row_end = size;

    for (uint32_t row = row_start; row < row_end; row++) {
        for (uint32_t col = 0; col < size; col++) {
            uint32_t a = (row + 1U) * (col + 3U);
            uint32_t b = (col + 1U) * (row + 5U);
            checksum += a * b;
        }
    }
    return checksum;
}

/* Returns count of random points inside the unit quarter-circle.
 * Pi ≈ 4 * result / samples.  Seed varies per worker/run via pid+time. */
static uint32_t monte_carlo_pi(uint32_t samples) {
    unsigned int seed = (unsigned int)((uint32_t)time(NULL) ^ (uint32_t)getpid());
    uint32_t inside = 0;
    for (uint32_t i = 0; i < samples; i++) {
        double x = (double)rand_r(&seed) / RAND_MAX;
        double y = (double)rand_r(&seed) / RAND_MAX;
        if (x * x + y * y <= 1.0) inside++;
    }
    return inside;
}

static uint32_t mandelbrot_iter(double cr, double ci) {
    double zr = 0.0, zi = 0.0;
    uint32_t n;
    for (n = 0; n < MANDELBROT_MAX_ITER; n++) {
        double zr2 = zr * zr - zi * zi + cr;
        double zi2 = 2.0 * zr * zi + ci;
        zr = zr2; zi = zi2;
        if (zr * zr + zi * zi > 4.0) break;
    }
    return n;
}

/* Returns iteration-count checksum for row_chunk rows starting at row_start. */
static uint32_t mandelbrot_checksum(uint32_t row_start, uint32_t row_chunk) {
    uint32_t checksum = 0;
    uint32_t row_end = row_start + row_chunk;
    if (row_end > MANDELBROT_HEIGHT) row_end = MANDELBROT_HEIGHT;
    for (uint32_t row = row_start; row < row_end; row++) {
        for (uint32_t col = 0; col < MANDELBROT_WIDTH; col++) {
            double cr = -2.5 + (double)col / MANDELBROT_WIDTH  * 3.5;
            double ci = -1.25 + (double)row / MANDELBROT_HEIGHT * 2.5;
            checksum += mandelbrot_iter(cr, ci);
        }
    }
    return checksum;
}

/* ── FFmpeg execution helpers ──────────────────────────────────────────────── */

/* Poll waitpid until pid exits or timeout_sec elapses.
 * Returns: 0 = child exited, 1 = timeout (child killed), -1 = error. */
static int wait_with_timeout(pid_t pid, int *wstatus, int timeout_sec) {
    time_t start = time(NULL);
    for (;;) {
        pid_t ret = waitpid(pid, wstatus, WNOHANG);
        if (ret == pid) return 0;
        if (ret < 0) { if (errno == EINTR) continue; return -1; }
        if (time(NULL) - start >= timeout_sec) {
            kill(pid, SIGKILL);
            waitpid(pid, wstatus, 0);
            return 1;
        }
        usleep(200000); /* 200 ms poll */
    }
}

/* Fork-exec a NULL-terminated argv and wait up to timeout_sec.
 * Called from inside the outer child so any grandchild fds with FD_CLOEXEC
 * are closed automatically on execvp. */
static uint32_t exec_with_timeout(char *const argv[], int timeout_sec) {
    pid_t pid = fork();
    if (pid < 0) return FFMPEG_RESULT_FAILURE;
    if (pid == 0) {
        /* Redirect stdout; keep stderr so logs appear in Docker output */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
        execvp(argv[0], argv);
        _exit(127);
    }
    int wstatus = 0;
    int ret = wait_with_timeout(pid, &wstatus, timeout_sec);
    if (ret == 1) return FFMPEG_RESULT_TIMEOUT;
    if (ret < 0)  return FFMPEG_RESULT_FAILURE;
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) return FFMPEG_RESULT_CMD_ERROR;
    return FFMPEG_RESULT_SUCCESS;
}

static uint32_t ffmpeg_segment(uint32_t segment_id) {
    char input[256], output[256], tmp_output[320];
    const char *vdir = video_dir();
    snprintf(input,  sizeof(input),  "%s/segments/part_%03u.mp4",  vdir, segment_id);
    snprintf(output, sizeof(output), "%s/processed/part_%03u.mp4", vdir, segment_id);
    snprintf(tmp_output, sizeof(tmp_output), "%s/processed/.part_%03u.tmp.%ld.mp4",
             vdir, segment_id, (long)getpid());

    if (access(input, R_OK) != 0) {
        fprintf(stderr, "[worker] segment not found: %s\n", input);
        return FFMPEG_RESULT_MISSING;
    }

    unlink(output);
    unlink(tmp_output);

    char *argv[] = {
        "ffmpeg", "-y",
        "-i", input,
        "-vf", "scale=1280:720,hue=s=0",
        "-c:v", "libx264", "-preset", "veryfast",
        "-c:a", "copy",
        tmp_output, NULL
    };
    uint32_t result = exec_with_timeout(argv, FFMPEG_TIMEOUT_SEC);
    if (result != FFMPEG_RESULT_SUCCESS) {
        unlink(tmp_output);
        return result;
    }

    if (rename(tmp_output, output) != 0) {
        fprintf(stderr, "[worker] failed to publish processed segment %s: %s\n",
                output, strerror(errno));
        unlink(tmp_output);
        return FFMPEG_RESULT_FAILURE;
    }

    return FFMPEG_RESULT_SUCCESS;
}

static uint32_t ffmpeg_script(uint32_t task_id, uint32_t segment_id) {
    char script[256], seg_arg[16];
    snprintf(script,  sizeof(script),  "%s/jobs/task_%u.sh", video_dir(), task_id);
    snprintf(seg_arg, sizeof(seg_arg), "%u", segment_id);

    if (access(script, R_OK | X_OK) != 0) {
        fprintf(stderr, "[worker] script not found or not executable: %s\n", script);
        return FFMPEG_RESULT_MISSING;
    }

    char *argv[] = { script, seg_arg, NULL };
    return exec_with_timeout(argv, FFMPEG_TIMEOUT_SEC);
}

/* ── Workload dispatcher ───────────────────────────────────────────────────── */

static uint32_t execute_workload(const NetworkPayload *task) {
    switch ((CommandCode)task->command_code) {
    case CMD_PRIME:
        return count_primes(task->argument);
    case CMD_MATRIX:
        return matrix_checksum(task->argument);
    case CMD_MATRIX_PARALLEL:
        return matrix_chunk_checksum(task->argument, task->result);
    case CMD_PRIME_RANGE:
        /* result field repurposed as range_end in task packets */
        return count_primes_range(task->argument, task->result);
    case CMD_MONTE_CARLO:
        return monte_carlo_pi(task->argument);
    case CMD_MANDELBROT:
        return mandelbrot_checksum(task->argument, MANDELBROT_ROW_CHUNK);
    case CMD_FFMPEG_SEGMENT:
        return ffmpeg_segment(task->argument);
    case CMD_FFMPEG_SCRIPT:
        return ffmpeg_script(task->task_id, task->argument);
    default:
        return task->argument;
    }
}

/* ── Pipe I/O helpers ──────────────────────────────────────────────────────── */

static int read_full_fd(int fd, void *buf, size_t len) {
    size_t total = 0;
    char *p = (char *)buf;
    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static int write_full_fd(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        total += (size_t)n;
    }
    return 0;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

int executor_run_task(const NetworkPayload *task, uint32_t *result_out) {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        perror("[worker] pipe");
        return EXECUTOR_ERR_PIPE;
    }
    /*
     * FD_CLOEXEC ensures that if the workload forks a grandchild (e.g. ffmpeg)
     * and that grandchild execs a new binary, the pipe ends are closed
     * automatically — preventing the grandchild from keeping the write end open.
     */
    fcntl(pipe_fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipe_fd[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        perror("[worker] fork");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return EXECUTOR_ERR_FORK;
    }

    if (pid == 0) {
        uint32_t result;
        close(pipe_fd[0]);
        result = execute_workload(task);
        if (write_full_fd(pipe_fd[1], &result, sizeof(result)) != 0) {
            close(pipe_fd[1]);
            _exit(2);
        }
        close(pipe_fd[1]);
        _exit(0);
    }

    close(pipe_fd[1]);

    uint32_t result = 0;
    int read_status = read_full_fd(pipe_fd[0], &result, sizeof(result));
    close(pipe_fd[0]);

    int child_status = 0;
    while (waitpid(pid, &child_status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("[worker] waitpid");
        return EXECUTOR_ERR_CHILD_STATUS;
    }

    if (read_status != 0) {
        fprintf(stderr, "[worker] child did not return a complete result\n");
        return EXECUTOR_ERR_CHILD_IO;
    }

    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        fprintf(stderr, "[worker] child exited abnormally for task %u\n", task->task_id);
        return EXECUTOR_ERR_CHILD_STATUS;
    }

    *result_out = result;
    return EXECUTOR_OK;
}
