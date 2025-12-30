#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include <stdatomic.h>

#define MAX_STRING_LEN 100
#define SWAP_PROBABILITY 50

// Глобальные счетчики (атомарные для потокобезопасности)
atomic_long total_passes = 0;
atomic_long increase_passes = 0;
atomic_long decrease_passes = 0;
atomic_long equal_passes = 0;
atomic_long total_comparisons = 0;
atomic_long total_swaps = 0;
atomic_long increase_count = 0;
atomic_long decrease_count = 0;
atomic_long equal_count = 0;

volatile bool cancel = false;

typedef struct _Node {
    char value[MAX_STRING_LEN];
    struct _Node* next;
    pthread_mutex_t lock;
} Node;

typedef struct _List {
    Node *first;
    int size;
} List;

// Функции блокировки/разблокировки узлов
static inline void lock_node(Node* node) {
    pthread_mutex_lock(&node->lock);
}

static inline void unlock_node(Node* node) {
    pthread_mutex_unlock(&node->lock);
}

// Создание и инициализация списка
List* create_list(int size) {
    List* list = (List*)malloc(sizeof(List));
    list->first = NULL;
    list->size = size;
    
    if (size <= 0) return list;
    
    // Создаем первый узел
    list->first = (Node*)malloc(sizeof(Node));
    int len = 1 + rand() % (MAX_STRING_LEN - 1);
    for (int i = 0; i < len; i++) {
        list->first->value[i] = 'a' + rand() % 26;
    }
    list->first->value[len] = '\0';
    list->first->next = NULL;
    pthread_mutex_init(&list->first->lock, NULL);
    
    // Создаем остальные узлы
    Node* current = list->first;
    for (int i = 1; i < size; i++) {
        Node* new_node = (Node*)malloc(sizeof(Node));
        
        len = 1 + rand() % (MAX_STRING_LEN - 1);
        for (int j = 0; j < len; j++) {
            new_node->value[j] = 'a' + rand() % 26;
        }
        new_node->value[len] = '\0';
        new_node->next = NULL;
        pthread_mutex_init(&new_node->lock, NULL);
        
        current->next = new_node;
        current = new_node;
    }
    
    return list;
}

// Освобождение списка
void free_list(List* list) {
    Node* current = list->first;
    while (current != NULL) {
        Node* next = current->next;
        pthread_mutex_destroy(&current->lock);
        free(current);
        current = next;
    }
    free(list);
}

// Функция сравнения длин строк
static inline int compare_lengths(const char* str1, const char* str2) {
    size_t len1 = strlen(str1);
    size_t len2 = strlen(str2);
    
    if (len1 < len2) return -1;   // увеличивается
    if (len1 > len2) return 1;    // уменьшается
    return 0;                     // равны
}

// Функция обмена двух узлов
void swap_nodes(Node* prev, Node* node1, Node* node2) {
    // node1 и node2 - соседние узлы, node1 предшествует node2
    Node* next = node2->next;
    
    if (prev != NULL) {
        prev->next = node2;
    }
    
    node1->next = next;
    node2->next = node1;
}

// Поток для подсчета возрастающих последовательностей
void* increases_thread(void* arg) {
    List* list = (List*)arg;
    
    while (!cancel) {
        // Захватываем первый узел
        Node* first = list->first;
        if (first == NULL) continue;
        
        lock_node(first);
        
        Node* second = first->next;
        if (second == NULL) {
            unlock_node(first);
            atomic_fetch_add(&increase_passes, 1);
            continue;
        }
        
        lock_node(second);
        
        // Линейный проход по списку
        Node* current = first;
        Node* next_node = second;
        Node* prev = NULL;
        
        while (next_node != NULL) {
            // Сравниваем длины
            int cmp = compare_lengths(current->value, next_node->value);
            atomic_fetch_add(&total_comparisons, 1);
            
            if (cmp < 0) {
                atomic_fetch_add(&increase_count, 1);
            }
            
            // Переходим к следующей паре
            if (prev != NULL) {
                unlock_node(prev);
            }
            
            prev = current;
            current = next_node;
            next_node = next_node->next;
            
            if (next_node != NULL) {
                lock_node(next_node);
            }
        }
        
        // Разблокируем последние узлы
        if (prev != NULL) {
            unlock_node(prev);
        }
        unlock_node(current);
        
        atomic_fetch_add(&increase_passes, 1);
        atomic_fetch_add(&total_passes, 1);
    }
    
    return NULL;
}

// Поток для подсчета убывающих последовательностей
void* decreases_thread(void* arg) {
    List* list = (List*)arg;
    
    while (!cancel) {
        Node* first = list->first;
        if (first == NULL) continue;
        
        lock_node(first);
        
        Node* second = first->next;
        if (second == NULL) {
            unlock_node(first);
            atomic_fetch_add(&decrease_passes, 1);
            continue;
        }
        
        lock_node(second);
        
        Node* current = first;
        Node* next_node = second;
        Node* prev = NULL;
        
        while (next_node != NULL) {
            int cmp = compare_lengths(current->value, next_node->value);
            atomic_fetch_add(&total_comparisons, 1);
            
            if (cmp > 0) {
                atomic_fetch_add(&decrease_count, 1);
            }
            
            if (prev != NULL) {
                unlock_node(prev);
            }
            
            prev = current;
            current = next_node;
            next_node = next_node->next;
            
            if (next_node != NULL) {
                lock_node(next_node);
            }
        }
        
        if (prev != NULL) {
            unlock_node(prev);
        }
        unlock_node(current);
        
        atomic_fetch_add(&decrease_passes, 1);
        atomic_fetch_add(&total_passes, 1);
    }
    
    return NULL;
}

// Поток для подсчета равных последовательностей
void* equal_thread(void* arg) {
    List* list = (List*)arg;
    
    while (!cancel) {
        Node* first = list->first;
        if (first == NULL) continue;
        
        lock_node(first);
        
        Node* second = first->next;
        if (second == NULL) {
            unlock_node(first);
            atomic_fetch_add(&equal_passes, 1);
            continue;
        }
        
        lock_node(second);
        
        Node* current = first;
        Node* next_node = second;
        Node* prev = NULL;
        
        while (next_node != NULL) {
            int cmp = compare_lengths(current->value, next_node->value);
            atomic_fetch_add(&total_comparisons, 1);
            
            if (cmp == 0) {
                atomic_fetch_add(&equal_count, 1);
            }
            
            if (prev != NULL) {
                unlock_node(prev);
            }
            
            prev = current;
            current = next_node;
            next_node = next_node->next;
            
            if (next_node != NULL) {
                lock_node(next_node);
            }
        }
        
        if (prev != NULL) {
            unlock_node(prev);
        }
        unlock_node(current);
        
        atomic_fetch_add(&equal_passes, 1);
        atomic_fetch_add(&total_passes, 1);
    }
    
    return NULL;
}

// Поток для перестановок
void* swap_thread(void* arg) {
    List* list = (List*)arg;
    unsigned int seed = time(NULL) ^ pthread_self();
    
    while (!cancel) {
        // Начинаем с начала списка
        Node* first = list->first;
        if (first == NULL || first->next == NULL) {
            usleep(1000);  // Краткая пауза если список слишком мал
            continue;
        }
        
        // Захватываем первый узел
        lock_node(first);
        
        // Определяем, менять ли первый и второй узлы
        if (rand_r(&seed) % 100 < SWAP_PROBABILITY) {
            Node* second = first->next;
            lock_node(second);
            
            // Меняем первый и второй узлы
            list->first = second;
            first->next = second->next;
            second->next = first;
            
            atomic_fetch_add(&total_swaps, 1);
            
            // Продолжаем с нового первого узла
            unlock_node(second);
            unlock_node(first);
            continue;
        }
        
        // Ищем пару для обмена дальше в списке
        Node* prev = first;
        Node* current = first->next;
        
        if (current == NULL) {
            unlock_node(first);
            continue;
        }
        
        lock_node(current);
        
        Node* next = current->next;
        
        while (next != NULL) {
            lock_node(next);
            
            // Случайно решаем, менять ли current и next
            if (rand_r(&seed) % 100 < SWAP_PROBABILITY) {
                // Меняем current и next
                prev->next = next;
                current->next = next->next;
                next->next = current;
                atomic_fetch_add(&total_swaps, 1);
                
                // После обмена current и next поменялись местами
                Node* temp = current;
                current = next;
                next = temp;
            }
            
            // Переходим к следующей тройке
            Node* new_next = next->next;
            unlock_node(prev);
            
            prev = current;
            current = next;
            next = new_next;
        }
        
        // Разблокируем последние узлы
        unlock_node(prev);
        if (current != NULL) {
            unlock_node(current);
        }
        
        // Краткая пауза чтобы дать другим потокам поработать
        usleep(10);
    }
    
    return NULL;
}

// Поток для вывода статистики
void* stats_thread(void* arg) {
    long long last_total_passes = 0;
    long long last_total_comparisons = 0;
    long long last_total_swaps = 0;
    
    while (!cancel) {
        sleep(1);
        
        long long current_passes = atomic_load(&total_passes);
        long long current_comparisons = atomic_load(&total_comparisons);
        long long current_swaps = atomic_load(&total_swaps);
        
        long long delta_passes = current_passes - last_total_passes;
        long long delta_comparisons = current_comparisons - last_total_comparisons;
        long long delta_swaps = current_swaps - last_total_swaps;
        
        printf("\n=== Статистика за 1 секунду ===\n");
        printf("Всего проходов:          %lld\n", delta_passes);
        printf("Проходов (возрастание):  %lld\n", atomic_load(&increase_passes));
        printf("Проходов (убывание):     %lld\n", atomic_load(&decrease_passes));
        printf("Проходов (равные):       %lld\n", atomic_load(&equal_passes));
        printf("Сравнений:               %lld\n", delta_comparisons);
        printf("Перестановок:            %lld\n", delta_swaps);
        printf("Возрастаний:             %lld\n", atomic_load(&increase_count));
        printf("Убываний:                %lld\n", atomic_load(&decrease_count));
        printf("Равных:                  %lld\n", atomic_load(&equal_count));
        
        last_total_passes = current_passes;
        last_total_comparisons = current_comparisons;
        last_total_swaps = current_swaps;
    }
    
    return NULL;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Использование: %s <размер_списка>\n", argv[0]);
        return 1;
    }
    
    int list_size = atoi(argv[1]);
    if (list_size <= 0) {
        printf("Неверный размер списка\n");
        return 1;
    }
    
    srand(time(NULL));
    
    printf("Создание списка из %d элементов...\n", list_size);
    List* list = create_list(list_size);
    printf("Список создан успешно\n");
    
    pthread_t inc_thread, dec_thread, eq_thread;
    pthread_t swap_threads[3];
    pthread_t stats_thread_id;
    
    printf("Запуск потоков...\n");
    
    // Запуск потоков подсчета
    pthread_create(&inc_thread, NULL, increases_thread, list);
    pthread_create(&dec_thread, NULL, decreases_thread, list);
    pthread_create(&eq_thread, NULL, equal_thread, list);
    
    // Запуск потоков перестановок
    for (int i = 0; i < 3; i++) {
        pthread_create(&swap_threads[i], NULL, swap_thread, list);
    }
    
    // Запуск потока статистики
    pthread_create(&stats_thread_id, NULL, stats_thread, NULL);
    
    // Работаем 30 секунд
    printf("Работаем 30 секунд...\n");
    for (int i = 0; i < 30; i++) {
        sleep(1);
        printf(".");
        fflush(stdout);
    }
    printf("\n");
    
    // Остановка потоков
    cancel = true;
    
    // Ожидание завершения потоков
    pthread_join(inc_thread, NULL);
    pthread_join(dec_thread, NULL);
    pthread_join(eq_thread, NULL);
    
    for (int i = 0; i < 3; i++) {
        pthread_join(swap_threads[i], NULL);
    }
    
    pthread_join(stats_thread_id, NULL);
    
    printf("\n=== ФИНАЛЬНАЯ СТАТИСТИКА ===\n");
    printf("Всего проходов:          %lld\n", atomic_load(&total_passes));
    printf("Проходов (возрастание):  %lld\n", atomic_load(&increase_passes));
    printf("Проходов (убывание):     %lld\n", atomic_load(&decrease_passes));
    printf("Проходов (равные):       %lld\n", atomic_load(&equal_passes));
    printf("Всего сравнений:         %lld\n", atomic_load(&total_comparisons));
    printf("Всего перестановок:      %lld\n", atomic_load(&total_swaps));
    printf("Всего возрастаний:       %lld\n", atomic_load(&increase_count));
    printf("Всего убываний:          %lld\n", atomic_load(&decrease_count));
    printf("Всего равных:            %lld\n", atomic_load(&equal_count));
    
    printf("\nОчистка памяти...\n");
    free_list(list);
    
    return 0;
}