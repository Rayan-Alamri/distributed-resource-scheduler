#include "queue_mgr.h"
#include <stdlib.h>
#include <string.h>

void queue_init(TaskQueue *q) {
    q->head = NULL;
    q->tail = NULL;
    q->size = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

void queue_destroy(TaskQueue *q) {
    pthread_mutex_lock(&q->lock);
    /* Drain every allocated node so repeated test runs do not leak queue data. */
    QueueNode *cur = q->head;
    while (cur) {
        QueueNode *tmp = cur->next;
        free(cur);
        cur = tmp;
    }
    q->head = q->tail = NULL;
    q->size = 0;
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
}

/* Append node to tail — caller must hold q->lock. */
static void append_locked(TaskQueue *q, QueueNode *node) {
    node->next = NULL;
    if (q->tail)
        q->tail->next = node;
    else
        q->head = node;
    q->tail = node;
    q->size++;
    pthread_cond_signal(&q->not_empty);
}

int queue_enqueue(TaskQueue *q, const NetworkPayload *task) {
    QueueNode *node = malloc(sizeof(QueueNode));
    if (!node)
        return -1;
    memcpy(&node->task, task, sizeof(NetworkPayload));
    node->next = NULL;
    pthread_mutex_lock(&q->lock);
    append_locked(q, node);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int queue_dequeue(TaskQueue *q, NetworkPayload *out) {
    pthread_mutex_lock(&q->lock);
    /*
     * pthread_cond_wait atomically releases the mutex while sleeping and
     * reacquires it before returning, so producers can enqueue safely.
     */
    while (!q->head)
        pthread_cond_wait(&q->not_empty, &q->lock);
    QueueNode *node = q->head;
    q->head = node->next;
    if (!q->head)
        q->tail = NULL;
    q->size--;
    memcpy(out, &node->task, sizeof(NetworkPayload));
    free(node);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int queue_dequeue_nowait(TaskQueue *q, NetworkPayload *out) {
    pthread_mutex_lock(&q->lock);
    if (!q->head) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    /* Same FIFO removal as queue_dequeue(), but returns immediately if empty. */
    QueueNode *node = q->head;
    q->head = node->next;
    if (!q->head)
        q->tail = NULL;
    q->size--;
    memcpy(out, &node->task, sizeof(NetworkPayload));
    free(node);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int queue_requeue(TaskQueue *q, const NetworkPayload *task) {
    return queue_enqueue(q, task);
}

int queue_is_empty(TaskQueue *q) {
    pthread_mutex_lock(&q->lock);
    int empty = (q->head == NULL);
    pthread_mutex_unlock(&q->lock);
    return empty;
}

int queue_size(TaskQueue *q) {
    pthread_mutex_lock(&q->lock);
    int sz = q->size;
    pthread_mutex_unlock(&q->lock);
    return sz;
}
