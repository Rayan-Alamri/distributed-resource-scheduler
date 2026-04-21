#include "executor.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/**
 * count_primes function calculates the number of prime numbers up to a given limit.
 * Uses trial division for efficiency.
 *
 * @param limit The upper limit for prime counting.
 * @return The count of prime numbers <= limit.
 */
static uint32_t count_primes(uint32_t limit) {
    uint32_t count = 0;

    for (uint32_t n = 2; n <= limit; n++) {
        int is_prime = 1;
        /*
         * Stop at sqrt(n) without calling sqrt(): d <= n / d avoids floating
         * point dependencies and also prevents d * d overflow.
         */
        for (uint32_t d = 2; d <= n / d; d++) {
            if (n % d == 0) {
                is_prime = 0;
                break;
            }
        }
        if (is_prime)
            count++;
    }

    return count;
}

static uint32_t matrix_checksum(uint32_t size) {
    if (size == 0)
        return 0;

    /*
     * The matrix workload simulates CPU-heavy nested-loop work without storing
     * a full matrix. Only the checksum is returned to keep IPC payloads small.
     */
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

/**
 * execute_workload function dispatches the task based on command_code.
 * Command 1: Prime counting
 * Command 2: Matrix checksum
 *
 * @param command_code The type of workload (1 or 2).
 * @param argument The input parameter for the workload.
 * @return The computed result.
 */
static uint32_t execute_workload(uint32_t command_code, uint32_t argument) {
    switch (command_code) {
    case 1:
        return count_primes(argument);
    case 2:
        return matrix_checksum(argument);
    default:
        return argument;
    }
}

static int read_full_fd(int fd, void *buf, size_t len) {
    size_t total = 0;
    char *p = (char *)buf;

    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        total += (size_t)n;
    }

    return 0;
}

static int write_full_fd(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = (const char *)buf;

    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        total += (size_t)n;
    }

    return 0;
}

int executor_run_task(const NetworkPayload *task, uint32_t *result_out) {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        perror("[worker] pipe");
        return EXECUTOR_ERR_PIPE;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[worker] fork");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return EXECUTOR_ERR_FORK;
    }

    if (pid == 0) {
        uint32_t result;

        /*
         * Child owns only the write end of the pipe. Use _exit() so child
         * termination does not flush parent-owned stdio buffers twice.
         */
        close(pipe_fd[0]);
        result = execute_workload(task->command_code, task->argument);
        if (write_full_fd(pipe_fd[1], &result, sizeof(result)) != 0) {
            close(pipe_fd[1]);
            _exit(2);
        }
        close(pipe_fd[1]);
        _exit(0);
    }

    /*
     * Parent waits for both a complete result and normal child termination.
     * This separates computation failure from network/result-send failure.
     */
    close(pipe_fd[1]);

    uint32_t result = 0;
    int read_status = read_full_fd(pipe_fd[0], &result, sizeof(result));
    close(pipe_fd[0]);

    int child_status = 0;
    while (waitpid(pid, &child_status, 0) < 0) {
        if (errno == EINTR)
            continue;
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
