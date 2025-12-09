#include "spinlock.h"
#include <stdio.h>

// Встроенная функция CAS для x86 (GCC)
#define CAS(ptr, old, new) __sync_val_compare_and_swap(ptr, old, new)
#define ATOMIC_EXCHANGE(ptr, new) __sync_lock_test_and_set(ptr, new)
#define MEMORY_BARRIER() __sync_synchronize()

void spinlock_init(spinlock_t *lock) {
    lock->lock = 0;
}

void spinlock_lock(spinlock_t *lock) {
    // Активное ожидание - "спиннинг"
    while (1) {
        // Пытаемся установить 1, если там было 0
        if (CAS(&lock->lock, 0, 1) == 0) {
            // Успешно захватили блокировку
            MEMORY_BARRIER();  // Барьер памяти
            return;
        }
        
        // Оптимизация: уменьшение нагрузки на шину памяти
        // __asm__ volatile("pause");  // Для x86 - инструкция PAUSE
    }
}

int spinlock_trylock(spinlock_t *lock) {
    // Попытка захватить без ожидания
    if (CAS(&lock->lock, 0, 1) == 0) {
        MEMORY_BARRIER();
        return 1;  // Успех
    }
    return 0;  // Неудача
}

void spinlock_unlock(spinlock_t *lock) {
    MEMORY_BARRIER();  // Барьер памяти перед освобождением
    lock->lock = 0;    // Просто сбрасываем флаг
    // Для полной атомарности можно использовать:
    // __sync_lock_release(&lock->lock);
}