#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/master/queue_mgr.h"
#include "../src/master/scheduler.h"
#include "../src/shared/protocol.h"

#define PASS(name) printf("[PASS] %s\n", name)
#define FAIL(name) do { printf("[FAIL] %s\n", name); exit(1); } while(0)

/* ── Queue tests ─────────────────────────────────────────────────────────── */

static NetworkPayload make_task(uint32_t id, uint32_t cmd, uint32_t arg) {
    NetworkPayload p;
    memset(&p, 0, sizeof(p));
    p.type         = MSG_TASK;
    p.task_id      = id;
    p.command_code = cmd;
    p.argument     = arg;
    return p;
}

static void test_queue_fifo(void) {
    TaskQueue q;
    queue_init(&q);

    NetworkPayload t1 = make_task(1, 1, 10);
    NetworkPayload t2 = make_task(2, 1, 20);
    NetworkPayload t3 = make_task(3, 1, 30);

    assert(queue_enqueue(&q, &t1) == 0);
    assert(queue_enqueue(&q, &t2) == 0);
    assert(queue_enqueue(&q, &t3) == 0);
    assert(queue_size(&q) == 3);

    NetworkPayload out;
    assert(queue_dequeue_nowait(&q, &out) == 0);
    assert(out.task_id == 1);
    assert(queue_dequeue_nowait(&q, &out) == 0);
    assert(out.task_id == 2);
    assert(queue_dequeue_nowait(&q, &out) == 0);
    assert(out.task_id == 3);
    assert(queue_dequeue_nowait(&q, &out) == -1);  /* empty */

    queue_destroy(&q);
    PASS("queue_fifo");
}

static void test_queue_empty(void) {
    TaskQueue q;
    queue_init(&q);

    assert(queue_is_empty(&q) == 1);
    assert(queue_size(&q) == 0);

    NetworkPayload t = make_task(1, 1, 0);
    queue_enqueue(&q, &t);
    assert(queue_is_empty(&q) == 0);
    assert(queue_size(&q) == 1);

    NetworkPayload out;
    queue_dequeue_nowait(&q, &out);
    assert(queue_is_empty(&q) == 1);

    queue_destroy(&q);
    PASS("queue_empty");
}

static void test_queue_requeue(void) {
    TaskQueue q;
    queue_init(&q);

    NetworkPayload t1 = make_task(10, 2, 99);
    NetworkPayload t2 = make_task(11, 2, 100);
    queue_enqueue(&q, &t1);
    queue_enqueue(&q, &t2);

    /* Dequeue first, simulate failure, requeue at tail */
    NetworkPayload out;
    queue_dequeue_nowait(&q, &out);
    assert(out.task_id == 10);
    queue_requeue(&q, &out);

    /* Now order should be 11, then 10 */
    queue_dequeue_nowait(&q, &out);
    assert(out.task_id == 11);
    queue_dequeue_nowait(&q, &out);
    assert(out.task_id == 10);
    assert(queue_is_empty(&q) == 1);

    queue_destroy(&q);
    PASS("queue_requeue");
}

/* ── Registry tests ──────────────────────────────────────────────────────── */

static void test_registry_add_and_idle(void) {
    WorkerRegistry r;
    registry_init(&r);

    assert(r.count == 0);

    int idx0 = registry_add(&r, 1, 10);
    int idx1 = registry_add(&r, 2, 11);
    int idx2 = registry_add(&r, 3, 12);
    assert(idx0 >= 0);
    assert(idx1 >= 0);
    assert(idx2 >= 0);
    assert(r.count == 3);

    /* All should be IDLE after add */
    pthread_mutex_lock(&r.lock);
    assert(r.workers[idx0].status == WORKER_IDLE);
    assert(r.workers[idx1].status == WORKER_IDLE);
    assert(r.workers[idx2].status == WORKER_IDLE);
    pthread_mutex_unlock(&r.lock);

    pthread_mutex_destroy(&r.lock);
    PASS("registry_add_and_idle");
}

static void test_registry_round_robin(void) {
    WorkerRegistry r;
    registry_init(&r);

    /* Add 3 workers */
    registry_add(&r, 1, 10);
    registry_add(&r, 2, 11);
    registry_add(&r, 3, 12);

    /* Find idle 4 times — should cycle 0→1→2→0 (by slot index) */
    pthread_mutex_lock(&r.lock);
    int a = registry_find_idle(&r);
    int b = registry_find_idle(&r);
    int c = registry_find_idle(&r);
    int d = registry_find_idle(&r);
    pthread_mutex_unlock(&r.lock);

    assert(a >= 0 && b >= 0 && c >= 0 && d >= 0);
    assert(a != b && b != c); /* three distinct slots the first three times */
    assert(d == a);            /* fourth wraps back to first slot */

    pthread_mutex_destroy(&r.lock);
    PASS("registry_round_robin");
}

static void test_registry_set_offline(void) {
    WorkerRegistry r;
    registry_init(&r);

    registry_add(&r, 7, 20);
    registry_add(&r, 8, 21);
    assert(r.count == 2);

    registry_set_offline(&r, 7);
    assert(r.count == 1);

    /* Worker 7 should not appear as idle */
    pthread_mutex_lock(&r.lock);
    WorkerInfo *w = registry_get(&r, 7);
    assert(w == NULL);
    pthread_mutex_unlock(&r.lock);

    /* Worker 8 still findable */
    pthread_mutex_lock(&r.lock);
    int idx = registry_find_idle(&r);
    assert(idx >= 0);
    assert(r.workers[idx].worker_id == 8);
    pthread_mutex_unlock(&r.lock);

    pthread_mutex_destroy(&r.lock);
    PASS("registry_set_offline");
}

static void test_registry_busy_not_selected(void) {
    WorkerRegistry r;
    registry_init(&r);

    registry_add(&r, 1, 10);
    registry_add(&r, 2, 11);
    registry_set_status(&r, 1, WORKER_BUSY);

    /* Only worker 2 should be returned */
    pthread_mutex_lock(&r.lock);
    int idx = registry_find_idle(&r);
    assert(idx >= 0);
    assert(r.workers[idx].worker_id == 2);

    /* Mark worker 2 busy too — no idle worker */
    r.workers[idx].status = WORKER_BUSY;
    int idx2 = registry_find_idle(&r);
    assert(idx2 == -1);
    pthread_mutex_unlock(&r.lock);

    pthread_mutex_destroy(&r.lock);
    PASS("registry_busy_not_selected");
}

/* ── Protocol byte-order tests ───────────────────────────────────────────── */

static void test_payload_byte_order(void) {
    NetworkPayload p;
    p.type         = 1;
    p.worker_id    = 2;
    p.task_id      = 3;
    p.command_code = 4;
    p.argument     = 5;
    p.result       = 6;

    NetworkPayload original = p;
    payload_to_net(&p);
    payload_to_host(&p);

    assert(p.type         == original.type);
    assert(p.worker_id    == original.worker_id);
    assert(p.task_id      == original.task_id);
    assert(p.command_code == original.command_code);
    assert(p.argument     == original.argument);
    assert(p.result       == original.result);

    PASS("payload_byte_order_roundtrip");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== Master unit tests ===\n");

    test_queue_fifo();
    test_queue_empty();
    test_queue_requeue();
    test_registry_add_and_idle();
    test_registry_round_robin();
    test_registry_set_offline();
    test_registry_busy_not_selected();
    test_payload_byte_order();

    printf("All tests passed.\n");
    return 0;
}
