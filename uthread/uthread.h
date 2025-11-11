#ifndef UTHREAD_H
#define UTHREAD_H

#include <ucontext.h>

#define STACK_SIZE 8192
#define MAX_THREADS 64

typedef struct {
    ucontext_t context;
    char stack[STACK_SIZE];
    void *(*start_routine)(void *);
    void *arg;
    void *result;
    int completed;
} uthread_t;

int uthread_create(uthread_t *thread, void *(*start_routine)(void *), void *arg);

void uthread_schedule(void);

int uthread_join(uthread_t *thread, void **result);

void uthread_yield(void);

int uthread_self(void);

#endif