#ifndef _MUTEX_H_
#define _MUTEX_H_

#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#define MUTEX_UNLOCKED 0
#define MUTEX_LOCKED 1
#define MUTEX_LOCKED_WITH_WAITERS 2

typedef struct {
    volatile uint32_t state;
    pthread_t owner;
} mutex_t;

#define MUTEX_INIT {MUTEX_UNLOCKED}

void mutex_init(mutex_t *mutex);
void mutex_lock(mutex_t *mutex);
void mutex_unlock(mutex_t *mutex);
int mutex_trylock(mutex_t *mutex);

#endif