#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/shared/protocol.h"
#include "../src/shared/models.h"
#include "../src/worker/executor.h"

#define PASS(name) printf("[PASS] %s\n", name)

static NetworkPayload make_task(uint32_t id, uint32_t command_code, uint32_t argument) {
    NetworkPayload task;
    memset(&task, 0, sizeof(task));
    task.type         = MSG_TASK;
    task.worker_id    = 1;
    task.task_id      = id;
    task.command_code = command_code;
    task.argument     = argument;
    return task;
}

static void test_executor_prime_task(void) {
    NetworkPayload task = make_task(100, CMD_PRIME, 10);
    uint32_t result = 0;
    assert(executor_run_task(&task, &result) == EXECUTOR_OK);
    assert(result == 4); /* primes <= 10: 2,3,5,7 */
    PASS("executor_prime_task");
}

static void test_executor_matrix_task(void) {
    NetworkPayload task = make_task(101, CMD_MATRIX, 2);
    uint32_t result = 0;
    assert(executor_run_task(&task, &result) == EXECUTOR_OK);
    assert(result == 187);
    PASS("executor_matrix_task");
}

static void test_executor_matrix_parallel_task(void) {
    NetworkPayload task = make_task(108, CMD_MATRIX_PARALLEL, 0);
    uint32_t result = 0;
    task.result = 2;
    assert(executor_run_task(&task, &result) == EXECUTOR_OK);
    assert(result == 187);
    PASS("executor_matrix_parallel_task");
}

static void test_executor_prime_range(void) {
    /* Primes in [10, 20]: 11, 13, 17, 19 = 4 primes */
    NetworkPayload task = make_task(102, CMD_PRIME_RANGE, 10);
    task.result = 20; /* range_end repurposed in task packets */
    uint32_t result = 0;
    assert(executor_run_task(&task, &result) == EXECUTOR_OK);
    assert(result == 4);
    PASS("executor_prime_range");
}

static void test_executor_prime_range_full(void) {
    /* Primes <= 10 split as range [2,10]: 2,3,5,7 = 4 */
    NetworkPayload task = make_task(103, CMD_PRIME_RANGE, 2);
    task.result = 10;
    uint32_t result = 0;
    assert(executor_run_task(&task, &result) == EXECUTOR_OK);
    assert(result == 4);
    PASS("executor_prime_range_full");
}

static void test_executor_monte_carlo(void) {
    /* With 1M samples, Pi estimate should be roughly 3.14.
     * inside / samples ≈ 0.785, so result should be close to 785000. */
    NetworkPayload task = make_task(104, CMD_MONTE_CARLO, 1000000u);
    uint32_t result = 0;
    assert(executor_run_task(&task, &result) == EXECUTOR_OK);
    /* Allow ±2% tolerance */
    assert(result > 770000u && result < 800000u);
    PASS("executor_monte_carlo");
}

static void test_executor_mandelbrot(void) {
    /* Row 0 of the Mandelbrot set must produce a non-zero checksum */
    NetworkPayload task = make_task(105, CMD_MANDELBROT, 0);
    uint32_t result = 0;
    assert(executor_run_task(&task, &result) == EXECUTOR_OK);
    assert(result > 0);
    PASS("executor_mandelbrot");
}

static void test_executor_mandelbrot_deterministic(void) {
    /* Same row should always produce the same checksum */
    NetworkPayload t1 = make_task(106, CMD_MANDELBROT, 100);
    NetworkPayload t2 = make_task(107, CMD_MANDELBROT, 100);
    uint32_t r1 = 0, r2 = 0;
    assert(executor_run_task(&t1, &r1) == EXECUTOR_OK);
    assert(executor_run_task(&t2, &r2) == EXECUTOR_OK);
    assert(r1 == r2);
    PASS("executor_mandelbrot_deterministic");
}

static void test_protocol_socketpair_roundtrip(void) {
    int fds[2];
    NetworkPayload out, in;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    memset(&out, 0, sizeof(out));
    out.type         = MSG_RESULT;
    out.worker_id    = 7;
    out.task_id      = 42;
    out.command_code = CMD_PRIME;
    out.argument     = 100;
    out.result       = 25;

    payload_to_net(&out);
    assert(send_full(fds[0], &out, sizeof(out)) == 0);
    assert(recv_full(fds[1], &in, sizeof(in)) == 0);
    payload_to_host(&in);

    assert(in.type         == MSG_RESULT);
    assert(in.worker_id    == 7);
    assert(in.task_id      == 42);
    assert(in.command_code == CMD_PRIME);
    assert(in.argument     == 100);
    assert(in.result       == 25);

    close(fds[0]);
    close(fds[1]);

    PASS("protocol_socketpair_roundtrip");
}

static void test_protocol_prime_range_payload(void) {
    /* Verify range_end survives network byte-order roundtrip */
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    NetworkPayload out, in;
    memset(&out, 0, sizeof(out));
    out.type         = MSG_TASK;
    out.task_id      = 10;
    out.command_code = CMD_PRIME_RANGE;
    out.argument     = 1;
    out.result       = 10000000; /* range_end */

    payload_to_net(&out);
    assert(send_full(fds[0], &out, sizeof(out)) == 0);
    assert(recv_full(fds[1], &in, sizeof(in)) == 0);
    payload_to_host(&in);

    assert(in.argument == 1);
    assert(in.result   == 10000000u);

    close(fds[0]);
    close(fds[1]);

    PASS("protocol_prime_range_payload");
}

int main(void) {
    printf("=== Worker executor tests ===\n");

    test_executor_prime_task();
    test_executor_matrix_task();
    test_executor_matrix_parallel_task();
    test_executor_prime_range();
    test_executor_prime_range_full();
    test_executor_monte_carlo();
    test_executor_mandelbrot();
    test_executor_mandelbrot_deterministic();
    test_protocol_socketpair_roundtrip();
    test_protocol_prime_range_payload();

    printf("All worker tests passed.\n");
    return 0;
}
