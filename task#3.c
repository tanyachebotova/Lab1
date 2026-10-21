#define _POSIX_C_SOURCE 200809L

/* Задача 3 */

#include "pthread_utils.h"
#include <limits.h>

#ifdef USE_NATIVE_RWLOCK
typedef pthread_rwlock_t rwlock_t;
#define LOCK_NAME "native"

static void rwlock_init(rwlock_t *lock) {
    check_pthread(pthread_rwlock_init(lock, NULL), "pthread_rwlock_init");
}
static void rwlock_rdlock(rwlock_t *lock) {
    check_pthread(pthread_rwlock_rdlock(lock), "pthread_rwlock_rdlock");
}
static void rwlock_wrlock(rwlock_t *lock) {
    check_pthread(pthread_rwlock_wrlock(lock), "pthread_rwlock_wrlock");
}
static void rwlock_unlock(rwlock_t *lock) {
    check_pthread(pthread_rwlock_unlock(lock), "pthread_rwlock_unlock");
}
static void rwlock_destroy(rwlock_t *lock) {
    check_pthread(pthread_rwlock_destroy(lock), "pthread_rwlock_destroy");
}
#else
#define LOCK_NAME "custom"
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t readers, writers;
    size_t active_readers, waiting_readers, waiting_writers;
    bool active_writer;
} rwlock_t;

static void rwlock_init(rwlock_t *lock) {
    lock->active_readers = lock->waiting_readers = lock->waiting_writers = 0;
    lock->active_writer = false;
    check_pthread(pthread_mutex_init(&lock->mutex, NULL), "rwlock: mutex_init");
    check_pthread(pthread_cond_init(&lock->readers, NULL), "rwlock: readers_init");
    check_pthread(pthread_cond_init(&lock->writers, NULL), "rwlock: writers_init");
}

static void rwlock_rdlock(rwlock_t *lock) {
    check_pthread(pthread_mutex_lock(&lock->mutex), "rwlock: reader_lock");
    ++lock->waiting_readers;
    /* Новые читатели ждут, если писатель уже стоит в очереди */
    while (lock->active_writer || lock->waiting_writers != 0)
        check_pthread(pthread_cond_wait(&lock->readers, &lock->mutex), "rwlock: reader_wait");
    --lock->waiting_readers;
    ++lock->active_readers;
    check_pthread(pthread_mutex_unlock(&lock->mutex), "rwlock: reader_unlock");
}

static void rwlock_wrlock(rwlock_t *lock) {
    check_pthread(pthread_mutex_lock(&lock->mutex), "rwlock: writer_lock");
    ++lock->waiting_writers;
    while (lock->active_writer || lock->active_readers != 0)
        check_pthread(pthread_cond_wait(&lock->writers, &lock->mutex), "rwlock: writer_wait");
    --lock->waiting_writers;
    lock->active_writer = true;
    check_pthread(pthread_mutex_unlock(&lock->mutex), "rwlock: writer_unlock");
}

static void rwlock_unlock(rwlock_t *lock) {
    check_pthread(pthread_mutex_lock(&lock->mutex), "rwlock: unlock_lock");
    if (lock->active_writer) lock->active_writer = false;
    else if (lock->active_readers != 0) --lock->active_readers;
    else fail("Попытка освободить незахваченную блокировку");
    if (lock->active_readers == 0 && lock->waiting_writers != 0)
        check_pthread(pthread_cond_signal(&lock->writers), "rwlock: signal_writer");
    else if (!lock->active_writer && lock->waiting_writers == 0 && lock->waiting_readers != 0)
        check_pthread(pthread_cond_broadcast(&lock->readers), "rwlock: broadcast_readers");
    check_pthread(pthread_mutex_unlock(&lock->mutex), "rwlock: unlock_unlock");
}

static void rwlock_destroy(rwlock_t *lock) {
    /* Вызываем только после join всех потоков, когда новых захватов уже нет */
    if (lock->active_writer || lock->active_readers || lock->waiting_readers || lock->waiting_writers)
        fail("Нельзя уничтожать занятую блокировку");
    check_pthread(pthread_cond_destroy(&lock->readers), "rwlock: readers_destroy");
    check_pthread(pthread_cond_destroy(&lock->writers), "rwlock: writers_destroy");
    check_pthread(pthread_mutex_destroy(&lock->mutex), "rwlock: mutex_destroy");
}
#endif

typedef struct list_node {
    int data;
    struct list_node *next;
} list_node_t;

/* Алгоритм генератора из my_rand.c преподавателя */
static uint32_t my_rand(uint32_t *seed) {
    *seed = (uint32_t)((uint64_t)*seed * UINT64_C(279470273) % UINT64_C(4294967291));
    return *seed;
}

static double my_drand(uint32_t *seed) {
    return (double)my_rand(seed) / 4294967291.0;
}

static void initialize_list(list_node_t **head, size_t count, size_t key_space) {
    /* Сразу получаем отсортированный список без повторяющихся ключей */
    list_node_t **tail = head;
    for (size_t i = 0; i < count; ++i) {
        list_node_t *node = allocate_array(1, sizeof(*node));
        node->data = (int)((uint64_t)i * key_space / count);
        *tail = node;
        tail = &node->next;
    }
}

static bool member(const list_node_t *head, int value) {
    while (head != NULL && head->data < value) head = head->next;
    return head != NULL && head->data == value;
}

static bool insert(list_node_t **head, int value) {
    list_node_t **link = head;
    while (*link != NULL && (*link)->data < value) link = &(*link)->next;
    if (*link != NULL && (*link)->data == value) return false;
    list_node_t *node = allocate_array(1, sizeof(*node));
    node->data = value;
    node->next = *link;
    *link = node;
    return true;
}

static bool delete_key(list_node_t **head, int value) {
    list_node_t **link = head;
    while (*link != NULL && (*link)->data < value) link = &(*link)->next;
    if (*link == NULL || (*link)->data != value) return false;
    list_node_t *removed = *link;
    *link = removed->next;
    free(removed);
    return true;
}

static size_t validate_list(const list_node_t *head, size_t key_space) {
    size_t count = 0;
    int previous = -1;
    for (const list_node_t *node = head; node != NULL; node = node->next) {
        if (node->data <= previous || node->data < 0 || (size_t)node->data >= key_space)
            fail("Нарушена сортировка списка или диапазон ключей");
        previous = node->data;
        ++count;
    }
    return count;
}

static void free_list(list_node_t *head) {
    while (head != NULL) {
        list_node_t *next = head->next;
        free(head);
        head = next;
    }
}

typedef struct { int key; unsigned char kind; } operation_t;
typedef struct {
    list_node_t **head;
    rwlock_t *lock;
    const operation_t *operations;
    size_t first, end;
    size_t counts[3], inserted, deleted, found;
} list_args_t;

static void *thread_work(void *pointer) {
    list_args_t *args = pointer;
    for (size_t i = args->first; i < args->end; ++i) {
        const operation_t op = args->operations[i];
        if (op.kind == 0) {
            rwlock_rdlock(args->lock);
            args->found += member(*args->head, op.key);
        } else {
            rwlock_wrlock(args->lock);
            if (op.kind == 1) args->inserted += insert(args->head, op.key);
            else args->deleted += delete_key(args->head, op.key);
        }
        rwlock_unlock(args->lock);
        ++args->counts[op.kind];
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 8)
        fail("Использование: pth_ll_rwl threads "
             "[initial ops search_fraction insert_fraction key_space seed]\n"
             "Без дополнительных аргументов четыре параметра вводятся через стандартный ввод.");
    const size_t threads = parse_size(argv[1], "threads", false);
    if (threads > 4096) fail("Число потоков не должно превышать 4096");
    size_t initial, total, key_space = 100000000;
    double search_fraction, insert_fraction;
    uint32_t seed = 1;
    if (argc == 8) {
        initial = parse_size(argv[2], "initial", true);
        total = parse_size(argv[3], "ops", true);
        search_fraction = parse_real(argv[4], "search_fraction");
        insert_fraction = parse_real(argv[5], "insert_fraction");
        key_space = parse_size(argv[6], "key_space", false);
        const size_t value = parse_size(argv[7], "seed", false);
        if (value >= UINT64_C(4294967291))
            fail("Начальное значение генератора должно быть от 1 до 4294967290");
        seed = (uint32_t)value;
    } else {
        char tokens[4][128];
        fprintf(stderr, "Введите начальное число ключей, число операций, "
                        "долю поиска и долю вставки:\n");
        for (size_t i = 0; i < 4; ++i)
            if (scanf("%127s", tokens[i]) != 1) fail("Не удалось прочитать входные данные");
        initial = parse_size(tokens[0], "initial", true);
        total = parse_size(tokens[1], "ops", true);
        search_fraction = parse_real(tokens[2], "search_fraction");
        insert_fraction = parse_real(tokens[3], "insert_fraction");
    }
    if (key_space > INT_MAX || initial > key_space ||
        search_fraction < 0.0 || insert_fraction < 0.0 ||
        search_fraction > 1.0 || insert_fraction > 1.0 ||
        search_fraction + insert_fraction > 1.0)
        fail("Неверное число ключей, диапазон ключей или доли операций");
    list_node_t *head = NULL;
    initialize_list(&head, initial, key_space);
    /* Одинаковая последовательность операций готовится до начала замера */
    operation_t *operations = allocate_array(total, sizeof(*operations));
    for (size_t i = 0; i < total; ++i) {
        const double which = my_drand(&seed);
        operations[i].kind = which < search_fraction ? 0 :
                             (which < search_fraction + insert_fraction ? 1 : 2);
        operations[i].key = (int)(my_rand(&seed) % key_space);
    }
    rwlock_t lock;
    rwlock_init(&lock);
    list_args_t *args = allocate_array(threads, sizeof(*args));
    size_t offset = 0;
    for (size_t t = 0; t < threads; ++t) {
        const size_t count = total / threads + (t < total % threads);
        args[t].head = &head;
        args[t].lock = &lock;
        args[t].operations = operations;
        args[t].first = offset;
        args[t].end = offset + count;
        offset += count;
    }
    const double seconds = run_parallel(threads, args, sizeof(*args), thread_work);
    size_t counts[3] = {0}, added = 0, removed = 0, found = 0;
    for (size_t t = 0; t < threads; ++t) {
        for (size_t k = 0; k < 3; ++k) counts[k] += args[t].counts[k];
        added += args[t].inserted;
        removed += args[t].deleted;
        found += args[t].found;
    }
    const size_t size = validate_list(head, key_space);
    if (counts[0] + counts[1] + counts[2] != total || initial + added < removed ||
        size != initial + added - removed)
        fail("Не совпадает число операций или итоговый размер списка");
    printf("lock=%s threads=%zu ops=%zu member=%zu insert=%zu delete=%zu "
           "successful_insert=%zu successful_delete=%zu member_hits=%zu "
           "final_size=%zu parallel_seconds=%.9f valid=1\n",
           LOCK_NAME, threads, total, counts[0], counts[1], counts[2],
           added, removed, found, size, seconds);
    rwlock_destroy(&lock);
    free_list(head);
    free(args);
    free(operations);
    return EXIT_SUCCESS;
}
