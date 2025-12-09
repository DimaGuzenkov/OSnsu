#include "queue.h"

#define LOCK_INIT(lock) spinlock_init(lock)
#define LOCK(lock) spinlock_lock(lock)
#define UNLOCK(lock) spinlock_unlock(lock)
#define TRY_LOCK(lock) spinlock_trylock(lock)

void *qmonitor(void *arg) {
    queue_t *q = (queue_t *)arg;
    printf("qmonitor: [%d %d %d]\n", getpid(), getppid(), gettid());

    while (1) {
        queue_print_stats(q);
        sleep(1);
    }
    return NULL;
}

queue_t* queue_init(int max_count) {
    int err;
    queue_t *q = malloc(sizeof(queue_t));
    
    if (!q) {
        printf("Cannot allocate memory for a queue\n");
        abort();
    }

    q->first = NULL;
    q->last = NULL;
    q->max_count = max_count;
    q->count = 0;

    q->add_attempts = q->get_attempts = 0;
    q->add_count = q->get_count = 0;

    // Инициализируем нашу блокировку
    LOCK_INIT(&q->lock);

    err = pthread_create(&q->qmonitor_tid, NULL, qmonitor, q);
    if (err) {
        printf("queue_init: pthread_create() failed: %s\n", strerror(err));
        free(q);
        abort();
    }

    return q;
}

void queue_destroy(queue_t *q) {
    if (q == NULL) return;
    
    pthread_cancel(q->qmonitor_tid);
    pthread_join(q->qmonitor_tid, NULL);
    
    // Блокировка не нужна при уничтожении
    
    qnode_t *current = q->first;
    while (current != NULL) {
        qnode_t *temp = current;
        current = current->next;
        free(temp);
    }
    
    free(q);
}

int queue_add(queue_t *q, int val) {
    LOCK(&q->lock);
    
    q->add_attempts++;
    assert(q->count <= q->max_count);

    if (q->count == q->max_count) {
        UNLOCK(&q->lock);
        return 0;
    }

    qnode_t *new = malloc(sizeof(qnode_t));
    if (!new) {
        printf("Cannot allocate memory for new node\n");
        UNLOCK(&q->lock);
        abort();
    }

    new->val = val;
    new->next = NULL;

    if (!q->first)
        q->first = q->last = new;
    else {
        q->last->next = new;
        q->last = q->last->next;
    }

    q->count++;
    q->add_count++;

    UNLOCK(&q->lock);
    return 1;
}

int queue_get(queue_t *q, int *val) {
    LOCK(&q->lock);
    
    q->get_attempts++;
    assert(q->count >= 0);

    if (q->count == 0) {
        UNLOCK(&q->lock);
        return 0;
    }

    qnode_t *tmp = q->first;
    *val = tmp->val;
    q->first = q->first->next;

    if (q->first == NULL) {
        q->last = NULL;
    }

    free(tmp);
    q->count--;
    q->get_count++;

    UNLOCK(&q->lock);
    return 1;
}

void queue_print_stats(queue_t *q) {
    LOCK(&q->lock);
    
    printf("queue stats: current size %d; attempts: (%ld %ld %ld); counts (%ld %ld %ld)\n",
        q->count,
        q->add_attempts, q->get_attempts, q->add_attempts - q->get_attempts,
        q->add_count, q->get_count, q->add_count - q->get_count);
    
    UNLOCK(&q->lock);
}