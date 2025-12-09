#ifndef _QUEUE_H_
#define _QUEUE_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include "spinlock.h"

typedef struct _qnode {
    int val;
    struct _qnode *next;
} qnode_t;

typedef struct queue {
    qnode_t *first;
    qnode_t *last;
    int count;
    int max_count;
    long add_attempts;
    long get_attempts;
    long add_count;
    long get_count;
    pthread_t qmonitor_tid;
    spinlock_t lock;
} queue_t;

queue_t* queue_init(int max_count);
void queue_destroy(queue_t *q);
int queue_add(queue_t *q, int val);
int queue_get(queue_t *q, int *val);
void queue_print_stats(queue_t *q);

#endif