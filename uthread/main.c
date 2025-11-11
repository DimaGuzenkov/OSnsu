#include "uthread.h"
#include <stdio.h>
#include <unistd.h>

void* worker1(void *arg) {
    int id = *(int*)arg;
    for (int i = 0; i < 5; i++) {
        printf("Поток %d: итерация %d\n", id, i);
        uthread_yield();
        usleep(100000);
    }
    printf("Поток %d завершен\n", id);
    return (void*)(long)(id * 100);
}

void* worker2(void *arg) {
    char *message = (char*)arg;
    for (int i = 0; i < 3; i++) {
        printf("Поток '%s': работа %d\n", message, i);
        uthread_yield();
        usleep(150000);
    }
    printf("Поток '%s' завершен\n", message);
    return (void*)message;
}

int main() {
    uthread_t thread1, thread2, thread3;
    int id1 = 1, id2 = 2;
    char *msg = "test";
    
    printf("Создание потоков...\n");
    
    if (uthread_create(&thread1, worker1, &id1) != 0) {
        printf("Ошибка создания потока 1\n");
        return 1;
    }
    
    if (uthread_create(&thread2, worker1, &id2) != 0) {
        printf("Ошибка создания потока 2\n");
        return 1;
    }
    
    if (uthread_create(&thread3, worker2, msg) != 0) {
        printf("Ошибка создания потока 3\n");
        return 1;
    }
    
    printf("Все потоки созданы. Запуск...\n");
    
    uthread_schedule();
    
    printf("Все потоки завершены.\n");
    
    void *result1, *result2, *result3;
    uthread_join(&thread1, &result1);
    uthread_join(&thread2, &result2);
    uthread_join(&thread3, &result3);
    
    printf("Результат потока 1: %ld\n", (long)result1);
    printf("Результат потока 2: %ld\n", (long)result2);
    printf("Результат потока 3: %s\n", (char*)result3);
    
    return 0;
}