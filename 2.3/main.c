#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>

#define MAX_STRING_LEN 100
#define SWAP_PROBABILITY 50

// Глобальные счетчики
volatile long long iterations[3] = {0};
volatile long long swap_attempts = 0;
volatile long long swap_success = 0;

// Узел списка
typedef struct _Node {
    char value[MAX_STRING_LEN];
    struct _Node* next;
    pthread_mutex_t sync;
} Node;

// Хранилище
typedef struct _Storage {
    Node *first;
    pthread_rwlock_t rwlock;
    int size;
} Storage;

// Инициализация
Storage* storage_create() {
    Storage* s = (Storage*)malloc(sizeof(Storage));
    s->first = NULL;
    s->size = 0;
    pthread_rwlock_init(&s->rwlock, NULL);
    return s;
}

Node* node_create(const char* value) {
    Node* n = (Node*)malloc(sizeof(Node));
    strncpy(n->value, value, MAX_STRING_LEN - 1);
    n->value[MAX_STRING_LEN - 1] = '\0';
    n->next = NULL;
    pthread_mutex_init(&n->sync, NULL);
    return n;
}

void storage_add(Storage* s, const char* value) {
    Node* new_node = node_create(value);
    
    pthread_rwlock_wrlock(&s->rwlock);
    if (s->first == NULL) {
        s->first = new_node;
    } else {
        Node* current = s->first;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_node;
    }
    s->size++;
    pthread_rwlock_unlock(&s->rwlock);
}

void generate_random_string(char* buffer, int min_len, int max_len) {
    int len = min_len + rand() % (max_len - min_len + 1);
    for (int i = 0; i < len; i++) {
        buffer[i] = 'a' + rand() % 26;
    }
    buffer[len] = '\0';
}

// ПОТОКИ ПОДСЧЕТА (корректные - одна пара за итерацию)
void* count_increasing(void* arg) {
    Storage* s = (Storage*)arg;
    static __thread int current_index = 0;  // Текущая позиция в списке
    
    while (1) {
        pthread_rwlock_rdlock(&s->rwlock);
        
        // Если список слишком мал
        if (s->size < 2) {
            pthread_rwlock_unlock(&s->rwlock);
            usleep(10);
            continue;
        }
        
        // Если дошли до конца списка
        if (current_index >= s->size - 1) {
            pthread_rwlock_unlock(&s->rwlock);
            iterations[0]++;  // Инкрементируем счетчик проходов
            current_index = 0;  // Начинаем с начала
            usleep(10);
            continue;
        }
        
        // Находим текущую пару по индексу
        Node* prev = s->first;
        for (int i = 0; i < current_index && prev != NULL; i++) {
            prev = prev->next;
        }
        
        if (prev == NULL || prev->next == NULL) {
            pthread_rwlock_unlock(&s->rwlock);
            current_index = 0;
            usleep(10);
            continue;
        }
        
        Node* curr = prev->next;
        
        // Захватываем мьютексы узлов
        pthread_mutex_lock(&prev->sync);
        pthread_mutex_lock(&curr->sync);
        
        // Можем отпустить rwlock
        pthread_rwlock_unlock(&s->rwlock);
        
        // Проверяем условие (возрастание)
        if (strlen(prev->value) < strlen(curr->value)) {
            // Можно что-то сделать, но в задании просто считаем итерации
        }
        
        // Освобождаем мьютексы
        pthread_mutex_unlock(&curr->sync);
        pthread_mutex_unlock(&prev->sync);
        
        // Переходим к следующей паре
        current_index++;
        
        usleep(10);
    }
    
    return NULL;
}

void* count_decreasing(void* arg) {
    Storage* s = (Storage*)arg;
    static __thread int current_index = 0;
    
    while (1) {
        pthread_rwlock_rdlock(&s->rwlock);
        
        if (s->size < 2) {
            pthread_rwlock_unlock(&s->rwlock);
            usleep(10);
            continue;
        }
        
        if (current_index >= s->size - 1) {
            pthread_rwlock_unlock(&s->rwlock);
            iterations[1]++;
            current_index = 0;
            usleep(10);
            continue;
        }
        
        Node* prev = s->first;
        for (int i = 0; i < current_index && prev != NULL; i++) {
            prev = prev->next;
        }
        
        if (prev == NULL || prev->next == NULL) {
            pthread_rwlock_unlock(&s->rwlock);
            current_index = 0;
            usleep(10);
            continue;
        }
        
        Node* curr = prev->next;
        
        pthread_mutex_lock(&prev->sync);
        pthread_mutex_lock(&curr->sync);
        
        pthread_rwlock_unlock(&s->rwlock);
        
        if (strlen(prev->value) > strlen(curr->value)) {
            // Проверка условия
        }
        
        pthread_mutex_unlock(&curr->sync);
        pthread_mutex_unlock(&prev->sync);
        
        current_index++;
        
        usleep(10);
    }
    
    return NULL;
}

void* count_equal(void* arg) {
    Storage* s = (Storage*)arg;
    static __thread int current_index = 0;
    
    while (1) {
        pthread_rwlock_rdlock(&s->rwlock);
        
        if (s->size < 2) {
            pthread_rwlock_unlock(&s->rwlock);
            usleep(10);
            continue;
        }
        
        if (current_index >= s->size - 1) {
            pthread_rwlock_unlock(&s->rwlock);
            iterations[2]++;
            current_index = 0;
            usleep(10);
            continue;
        }
        
        Node* prev = s->first;
        for (int i = 0; i < current_index && prev != NULL; i++) {
            prev = prev->next;
        }
        
        if (prev == NULL || prev->next == NULL) {
            pthread_rwlock_unlock(&s->rwlock);
            current_index = 0;
            usleep(10);
            continue;
        }
        
        Node* curr = prev->next;
        
        pthread_mutex_lock(&prev->sync);
        pthread_mutex_lock(&curr->sync);
        
        pthread_rwlock_unlock(&s->rwlock);
        
        if (strlen(prev->value) == strlen(curr->value)) {
            // Проверка условия
        }
        
        pthread_mutex_unlock(&curr->sync);
        pthread_mutex_unlock(&prev->sync);
        
        current_index++;
        
        usleep(10);
    }
    
    return NULL;
}

// ПОТОКИ ПЕРЕСТАНОВКИ (одна пара за итерацию)
void* swap_thread(void* arg) {
    Storage* s = (Storage*)arg;
    static __thread unsigned int seed = 0;
    static __thread int current_index = 0;
    
    if (seed == 0) seed = time(NULL) ^ pthread_self();
    
    while (1) {
        __sync_fetch_and_add(&swap_attempts, 1);
        
        // Решаем случайно, будем ли менять
        bool should_swap = (rand_r(&seed) % 100) < SWAP_PROBABILITY;
        
        if (!should_swap) {
            current_index = (current_index + 1) % (s->size > 1 ? s->size - 1 : 1);
            usleep(10);
            continue;
        }
        
        pthread_rwlock_wrlock(&s->rwlock);
        
        if (s->size < 2) {
            pthread_rwlock_unlock(&s->rwlock);
            usleep(10);
            continue;
        }
        
        // Определяем текущую пару
        int pair_index = current_index % (s->size - 1);
        current_index = (current_index + 1) % (s->size - 1);
        
        // Находим узлы
        Node* prev = NULL;
        Node* curr = s->first;
        
        for (int i = 0; i < pair_index && curr != NULL; i++) {
            prev = curr;
            curr = curr->next;
        }
        
        if (curr == NULL || curr->next == NULL) {
            pthread_rwlock_unlock(&s->rwlock);
            continue;
        }
        
        Node* next = curr->next;
        
        // Выполняем перестановку
        if (prev == NULL) {
            s->first = next;
        } else {
            prev->next = next;
        }
        
        curr->next = next->next;
        next->next = curr;
        
        __sync_fetch_and_add(&swap_success, 1);
        
        pthread_rwlock_unlock(&s->rwlock);
        
        usleep(10);
    }
    
    return NULL;
}

// Статистика
void* stats_thread(void* arg) {
    long long last_iterations[3] = {0};
    long long last_swap_attempts = 0;
    long long last_swap_success = 0;
    
    while (1) {
        sleep(1);
        
        long long delta_iter0 = iterations[0] - last_iterations[0];
        long long delta_iter1 = iterations[1] - last_iterations[1];
        long long delta_iter2 = iterations[2] - last_iterations[2];
        long long delta_attempts = swap_attempts - last_swap_attempts;
        long long delta_success = swap_success - last_swap_success;
        
        printf("\n=== Статистика за 1 секунду ===\n");
        printf("Проходов подсчета (возрастание): %lld\n", delta_iter0);
        printf("Проходов подсчета (убывание):    %lld\n", delta_iter1);
        printf("Проходов подсчета (равные):      %lld\n", delta_iter2);
        printf("Попыток перестановки:           %lld\n", delta_attempts);
        printf("Успешных перестановок:          %lld\n", delta_success);
        printf("Эффективность перестановок:     %.2f%%\n", 
               delta_attempts > 0 ? (100.0 * delta_success / delta_attempts) : 0.0);
        
        last_iterations[0] = iterations[0];
        last_iterations[1] = iterations[1];
        last_iterations[2] = iterations[2];
        last_swap_attempts = swap_attempts;
        last_swap_success = swap_success;
    }
    
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Использование: %s <размер_списка>\n", argv[0]);
        printf("Доступные размеры: 100, 1000, 10000, 100000\n");
        return 1;
    }
    
    int list_size = atoi(argv[1]);
    if (list_size <= 0) {
        printf("Неверный размер списка\n");
        return 1;
    }
    
    srand(time(NULL));
    
    printf("Создание списка из %d элементов...\n", list_size);
    Storage* storage = storage_create();
    
    for (int i = 0; i < list_size; i++) {
        char buffer[MAX_STRING_LEN];
        generate_random_string(buffer, 1, 99);
        storage_add(storage, buffer);
        
        if (list_size >= 10000 && (i + 1) % 10000 == 0) {
            printf("  Создано %d элементов...\n", i + 1);
        }
    }
    printf("Список создан. Размер: %d\n", storage->size);
    
    // Создаем потоки
    pthread_t counter_threads[3];
    pthread_t swap_threads[3];
    pthread_t stats_thread_id;
    
    printf("Запуск потоков...\n");
    
    // Запускаем потоки подсчета
    pthread_create(&counter_threads[0], NULL, count_increasing, storage);
    pthread_create(&counter_threads[1], NULL, count_decreasing, storage);
    pthread_create(&counter_threads[2], NULL, count_equal, storage);
    
    // Запускаем потоки перестановки
    for (int i = 0; i < 3; i++) {
        pthread_create(&swap_threads[i], NULL, swap_thread, storage);
    }
    
    pthread_create(&stats_thread_id, NULL, stats_thread, NULL);
    
    printf("Работаем 30 секунд...\n");
    for (int i = 0; i < 30; i++) {
        sleep(1);
        printf(".");
        fflush(stdout);
    }
    printf("\n");
    
    printf("\n=== ФИНАЛЬНАЯ СТАТИСТИКА ===\n");
    printf("Всего проходов подсчета (возрастание): %lld\n", iterations[0]);
    printf("Всего проходов подсчета (убывание):    %lld\n", iterations[1]);
    printf("Всего проходов подсчета (равные):      %lld\n", iterations[2]);
    printf("Всего попыток перестановки:           %lld\n", swap_attempts);
    printf("Всего успешных перестановок:          %lld\n", swap_success);
    printf("Общая эффективность перестановок:     %.2f%%\n", 
           swap_attempts > 0 ? (100.0 * swap_success / swap_attempts) : 0.0);
    
    printf("\nСредняя скорость (за 30 секунд):\n");
    printf("Подсчет возрастания: %.1f проходов/сек\n", iterations[0] / 30.0);
    printf("Подсчет убывания:    %.1f проходов/сек\n", iterations[1] / 30.0);
    printf("Подсчет равенства:   %.1f проходов/сек\n", iterations[2] / 30.0);
    printf("Перестановки:        %.1f попыток/сек\n", swap_attempts / 30.0);
    
    printf("\nЗавершение работы...\n");
    
    return 0;
}