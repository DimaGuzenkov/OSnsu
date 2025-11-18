#include "uthread.h"
#include <stdio.h>
#include <unistd.h>

void set_cpu(int n) {
	int err;
	cpu_set_t cpuset;
	pthread_t tid = pthread_self();

	CPU_ZERO(&cpuset);
	CPU_SET(n, &cpuset);

	err = pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cpuset);
	if (err) {
		printf("set_cpu: pthread_setaffinity failed for cpu %d\n", n);
		return;
	}

	printf("set_cpu: set cpu %d\n", n);
}

void* worker1(void *arg) {
    int id = *(int*)arg;
    set_cpu(1);
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
    set_cpu(2);
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