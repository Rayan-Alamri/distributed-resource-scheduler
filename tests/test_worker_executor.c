#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/shared/protocol.h"
#include "../src/worker/executor.h"

#define PASS(name) printf("[PASS] %s\n", name)

static NetworkPayload make_task(uint32_t id, uint32_t command_code, uint32_t argument) {
    NetworkPayload task;

    memset(&task, 0, sizeof(task));
    task.type = MSG_TASK;
    task.worker_id = 1;
    task.task_id = id;
    task.command_code = command_code;
    task.argument = argument;

    return task;
}

static void test_executor_prime_task(void) {
    NetworkPayload task = make_task(100, 1, 10);
    uint32_t result = 0;

    assert(executor_run_task(&task, &result) == EXECUTOR_OK);
    assert(result == 4);

    PASS("executor_prime_task");
}

static void test_executor_matrix_task(void) {
    NetworkPayload task = make_task(101, 2, 2);
    uint32_t result = 0;

    assert(executor_run_task(&task, &result) == EXECUTOR_OK);
    assert(result == 187);

    PASS("executor_matrix_task");
}

static void test_protocol_socketpair_roundtrip(void) {
    int fds[2];
    NetworkPayload out;
    NetworkPayload in;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    memset(&out, 0, sizeof(out));
    out.type = MSG_RESULT;
    out.worker_id = 7;
    out.task_id = 42;
    out.command_code = 1;
    out.argument = 100;
    out.result = 25;

    payload_to_net(&out);
    if (send_full(fds[0], &out, sizeof(out)) != 0) {
        perror("send_full");
        assert(0);
    }
    assert(recv_full(fds[1], &in, sizeof(in)) == 0);
    payload_to_host(&in);

    assert(in.type == MSG_RESULT);
    assert(in.worker_id == 7);
    assert(in.task_id == 42);
    assert(in.command_code == 1);
    assert(in.argument == 100);
    assert(in.result == 25);

    close(fds[0]);
    close(fds[1]);

    PASS("protocol_socketpair_roundtrip");
}

int main(void) {
    printf("=== Worker executor tests ===\n");

    test_executor_prime_task();
    test_executor_matrix_task();
    test_protocol_socketpair_roundtrip();

    printf("All worker tests passed.\n");
    return 0;
}
