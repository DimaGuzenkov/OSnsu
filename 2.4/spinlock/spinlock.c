#include "spinlock.h"
#include <stdio.h>

#define CAS(ptr, old, new) __sync_val_compare_and_swap(ptr, old, new)
#define ATOMIC_EXCHANGE(ptr, new) __sync_lock_test_and_set(ptr, new)
#define MEMORY_BARRIER() __sync_synchronize()

void spinlock_init(spinlock_t *lock) {
    lock->lock = 0;
}

void spinlock_lock(spinlock_t *lock) {
    while (1) {
        if (CAS(&lock->lock, 0, 1) == 0) {
            MEMORY_BARRIER();
            return;
        }
    }
}

int spinlock_trylock(spinlock_t *lock) {
    if (CAS(&lock->lock, 0, 1) == 0) {
        MEMORY_BARRIER();
        return 1;
    }
    return 0;
}

void spinlock_unlock(spinlock_t *lock) {
    MEMORY_BARRIER();
    lock->lock = 0;
}