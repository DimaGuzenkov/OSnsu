#ifndef _SPINLOCK_H_
#define _SPINLOCK_H_

#include <stdint.h>

// Структура спинлока - просто целое число
typedef struct {
    volatile int lock;
} spinlock_t;

// Инициализация спинлока
#define SPINLOCK_INIT {0}

// Функции работы со спинлоком
void spinlock_init(spinlock_t *lock);
void spinlock_lock(spinlock_t *lock);
void spinlock_unlock(spinlock_t *lock);
int spinlock_trylock(spinlock_t *lock);

#endif