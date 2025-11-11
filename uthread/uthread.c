#include "uthread.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static uthread_t threads[MAX_THREADS];
static int thread_count = 0;
static int current_thread = -1;
static ucontext_t main_context;

static void uthread_wrapper(uthread_t *thread) {
    thread->result = thread->start_routine(thread->arg);
    thread->completed = 1;
    
    uthread_yield();
}

int uthread_create(uthread_t *thread, void *(*start_routine)(void *), void *arg) {
    if (thread_count >= MAX_THREADS) {
        return -1;
    }
    
    if (getcontext(&thread->context) == -1) {
        return -1;
    }
    
    thread->context.uc_stack.ss_sp = thread->stack;
    thread->context.uc_stack.ss_size = STACK_SIZE;
    thread->context.uc_link = NULL;
    
    thread->start_routine = start_routine;
    thread->arg = arg;
    thread->completed = 0;
    thread->result = NULL;
    
    makecontext(&thread->context, (void (*)())uthread_wrapper, 1, thread);
    
    threads[thread_count] = *thread;
    thread_count++;
    
    return 0;
}

void uthread_yield(void) {
    if (thread_count == 0) {
        return;
    }
    
    int next_thread = -1;
    for (int i = 1; i <= thread_count; i++) {
        int candidate = (current_thread + i) % thread_count;
        if (!threads[candidate].completed) {
            next_thread = candidate;
            break;
        }
    }
    
    if (next_thread == -1) {
        if (current_thread != -1) {
            setcontext(&main_context);
        }
        return;
    }
    
    ucontext_t *current_context = (current_thread == -1) ? &main_context : &threads[current_thread].context;
    
    int prev_thread = current_thread;
    current_thread = next_thread;
    
    swapcontext(current_context, &threads[next_thread].context);
}

void uthread_schedule(void) {
    if (getcontext(&main_context) == -1) {
        return;
    }
    
    while (1) {
        int active_threads = 0;
        for (int i = 0; i < thread_count; i++) {
            if (!threads[i].completed) {
                active_threads++;
            }
        }
        
        if (active_threads == 0) {
            break;
        }
        uthread_yield();
    }
}

int uthread_join(uthread_t *thread, void **result) {
    if (!thread->completed) {
        ucontext_t caller_context;
        getcontext(&caller_context);
        
        if (current_thread == -1) {
            main_context = caller_context;
        }
        
        setcontext(&thread->context);
    }
    
    if (result != NULL) {
        *result = thread->result;
    }
    
    return 0;
}

int uthread_self(void) {
    return current_thread;
}