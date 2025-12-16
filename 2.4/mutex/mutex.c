#include "mutex.h"
#include "futex.h"
#include <stdio.h>

#define CAS(ptr, old, new) __sync_val_compare_and_swap(ptr, old, new)
#define ATOMIC_EXCHANGE(ptr, new) __sync_lock_test_and_set(ptr, new)
#define ATOMIC_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
#define ATOMIC_SUB(ptr, val) __sync_fetch_and_sub(ptr, val)
#define MEMORY_BARRIER() __sync_synchronize()

void mutex_init(mutex_t *mutex) {
    mutex->state = MUTEX_UNLOCKED;
}

void mutex_lock(mutex_t *mutex) {
    if (CAS(&mutex->state, MUTEX_UNLOCKED, MUTEX_LOCKED) == MUTEX_UNLOCKED) {
        MEMORY_BARRIER();
        mutex->owner = pthread_self();
        return;
    }
    
    while (1) {
        uint32_t old_state = mutex->state;
        
        if (old_state == MUTEX_UNLOCKED) {
            if (CAS(&mutex->state, MUTEX_UNLOCKED, MUTEX_LOCKED_WITH_WAITERS) == MUTEX_UNLOCKED) {
                MEMORY_BARRIER();
                return;
            }
            continue;
        }
        
        if (old_state != MUTEX_LOCKED_WITH_WAITERS) {
            CAS(&mutex->state, MUTEX_LOCKED, MUTEX_LOCKED_WITH_WAITERS);
        }
        
        futex_wait(&mutex->state, MUTEX_LOCKED_WITH_WAITERS);
    }
}

int mutex_trylock(mutex_t *mutex) {
    if (CAS(&mutex->state, MUTEX_UNLOCKED, MUTEX_LOCKED) == MUTEX_UNLOCKED) {
        MEMORY_BARRIER();
        return 1;
    }
    return 0;
}

void mutex_unlock(mutex_t *mutex) {
    MEMORY_BARRIER();
    if (mutex->state == MUTEX_UNLOCKED || mutex->owner != pthread_self()) {
        perror("Mutex error");
    }
    
    uint32_t old_state = ATOMIC_EXCHANGE(&mutex->state, MUTEX_UNLOCKED);
    
    if (old_state == MUTEX_LOCKED_WITH_WAITERS) {
        futex_wake(&mutex->state, 1);
    }
}