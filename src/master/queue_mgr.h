#ifndef QUEUE_MGR_H
#define QUEUE_MGR_H

#include <pthread.h>
#include "../shared/models.h"

typedef struct QueueNode {
    NetworkPayload   task;
    struct QueueNode *next;
} QueueNode;

typedef struct {
    QueueNode       *head;
    QueueNode       *tail;
    int              size;
    pthread_mutex_t  lock;
    pthread_cond_t   not_empty;
} TaskQueue;

void queue_init(TaskQueue *q);
void queue_destroy(TaskQueue *q);

/* Enqueue a copy of task at the tail. Returns 0 on success, -1 on alloc failure. */
int queue_enqueue(TaskQueue *q, const NetworkPayload *task);

/* Blocking dequeue from head. Always succeeds (waits if empty). */
int queue_dequeue(TaskQueue *q, NetworkPayload *out);

/* Non-blocking dequeue. Returns 0 and fills out, or -1 if empty. */
int queue_dequeue_nowait(TaskQueue *q, NetworkPayload *out);

/* Re-enqueue an existing task (e.g. after worker disconnect). Same as enqueue. */
int queue_requeue(TaskQueue *q, const NetworkPayload *task);

int queue_is_empty(TaskQueue *q);
int queue_size(TaskQueue *q);

#endif /* QUEUE_MGR_H */
