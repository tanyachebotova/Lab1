#ifndef LAB1_PTHREAD_UTILS_H
#define LAB1_PTHREAD_UTILS_H

#include "timer.h"
#include <pthread.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline _Noreturn void fail(const char *message) {
    fprintf(stderr, "Ошибка: %s\n", message);
    exit(EXIT_FAILURE);
}

static inline void check_pthread(int code, const char *operation) {
    /* pthreads возвращает код ошибки напрямую, не через errno */
    if (code != 0) {
        fprintf(stderr, "Ошибка: %s: %s\n", operation, strerror(code));
        exit(EXIT_FAILURE);
    }
}

static inline void *allocate_array(size_t count, size_t element_size) {
    if (element_size == 0 || count > SIZE_MAX / element_size)
        fail("Слишком большой размер массива");
    /* Даже для пустого массива возвращаем указатель, пригодный для free */
    void *memory = calloc(count == 0 ? 1 : count, element_size);
    if (memory == NULL) fail("Не удалось выделить память");
    return memory;
}

static inline size_t parse_size(const char *text, const char *name, bool allow_zero) {
    if (*text == '\0') fail("Не задан числовой параметр");
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            fprintf(stderr, "Ошибка: %s должен быть целым неотрицательным числом\n", name);
            exit(EXIT_FAILURE);
        }
    }
    errno = 0;
    char *end = NULL;
    const uintmax_t value = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || value > SIZE_MAX || (!allow_zero && value == 0)) {
        fprintf(stderr, "Ошибка: недопустимое значение параметра %s\n", name);
        exit(EXIT_FAILURE);
    }
    return (size_t)value;
}

static inline double parse_real(const char *text, const char *name) {
    errno = 0;
    char *end = NULL;
    const double value = strtod(text, &end);
    if (*text == '\0' || *end != '\0' || errno != 0 || !isfinite(value)) {
        fprintf(stderr, "Ошибка: %s должен быть конечным вещественным числом\n", name);
        exit(EXIT_FAILURE);
    }
    return value;
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    size_t participants, arrived, generation;
} barrier_t;

static inline void barrier_init(barrier_t *barrier, size_t participants) {
    if (participants == 0) fail("В барьере нет участников");
    barrier->participants = participants;
    barrier->arrived = barrier->generation = 0;
    check_pthread(pthread_mutex_init(&barrier->mutex, NULL), "barrier: mutex_init");
    check_pthread(pthread_cond_init(&barrier->changed, NULL), "barrier: cond_init");
}

static inline void barrier_wait(barrier_t *barrier) {
    check_pthread(pthread_mutex_lock(&barrier->mutex), "barrier: lock");
    const size_t generation = barrier->generation;
    if (++barrier->arrived == barrier->participants) {
        barrier->arrived = 0;
        ++barrier->generation;
        check_pthread(pthread_cond_broadcast(&barrier->changed), "barrier: broadcast");
    } else {
        /* Проверяем поколение после каждого (и ложного) пробуждения */
        while (generation == barrier->generation)
            check_pthread(pthread_cond_wait(&barrier->changed, &barrier->mutex), "barrier: wait");
    }
    check_pthread(pthread_mutex_unlock(&barrier->mutex), "barrier: unlock");
}

static inline void barrier_destroy(barrier_t *barrier) {
    check_pthread(pthread_cond_destroy(&barrier->changed), "barrier: cond_destroy");
    check_pthread(pthread_mutex_destroy(&barrier->mutex), "barrier: mutex_destroy");
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready_cond, start_cond;
    size_t ready;
    bool started, cancelled;
} start_gate_t;

typedef struct {
    start_gate_t *gate;
    void *(*work)(void *);
    void *argument;
} worker_call_t;

static inline void *start_worker(void *pointer) {
    worker_call_t *call = pointer;
    start_gate_t *gate = call->gate;
    check_pthread(pthread_mutex_lock(&gate->mutex), "start: lock");
    ++gate->ready;
    check_pthread(pthread_cond_signal(&gate->ready_cond), "start: ready_signal");
    while (!gate->started)
        check_pthread(pthread_cond_wait(&gate->start_cond, &gate->mutex), "start: wait");
    const bool cancelled = gate->cancelled;
    check_pthread(pthread_mutex_unlock(&gate->mutex), "start: unlock");
    return cancelled ? NULL : call->work(call->argument);
}

/* Массив аргументов существует до завершения всех потоков.
   Замер включает общий старт, работу и join, создание потоков исключено */
static inline double run_parallel(size_t count, void *arguments, size_t stride,
                                  void *(*work)(void *)) {
    if (count == 0) fail("Не заданы рабочие потоки");
    start_gate_t gate;
    gate.ready = 0;
    gate.started = gate.cancelled = false;
    check_pthread(pthread_mutex_init(&gate.mutex, NULL), "start: mutex_init");
    check_pthread(pthread_cond_init(&gate.ready_cond, NULL), "start: ready_init");
    check_pthread(pthread_cond_init(&gate.start_cond, NULL), "start: cond_init");
    pthread_t *threads = allocate_array(count, sizeof(*threads));
    worker_call_t *calls = allocate_array(count, sizeof(*calls));
    for (size_t i = 0; i < count; ++i) {
        calls[i] = (worker_call_t){&gate, work, (char *)arguments + i * stride};
        const int code = pthread_create(&threads[i], NULL, start_worker, &calls[i]);
        if (code != 0) {
            /* При нехватке ресурсов завершаем уже созданные потоки */
            check_pthread(pthread_mutex_lock(&gate.mutex), "start: cancel_lock");
            gate.cancelled = gate.started = true;
            check_pthread(pthread_cond_broadcast(&gate.start_cond), "start: cancel");
            check_pthread(pthread_mutex_unlock(&gate.mutex), "start: cancel_unlock");
            for (size_t j = 0; j < i; ++j)
                check_pthread(pthread_join(threads[j], NULL), "pthread_join");
            check_pthread(code, "pthread_create");
        }
    }
    check_pthread(pthread_mutex_lock(&gate.mutex), "start: main_lock");
    while (gate.ready != count)
        check_pthread(pthread_cond_wait(&gate.ready_cond, &gate.mutex), "start: ready_wait");
    const double start = timer_seconds();
    gate.started = true;
    check_pthread(pthread_cond_broadcast(&gate.start_cond), "start: broadcast");
    check_pthread(pthread_mutex_unlock(&gate.mutex), "start: main_unlock");
    for (size_t i = 0; i < count; ++i)
        check_pthread(pthread_join(threads[i], NULL), "pthread_join");
    const double elapsed = timer_seconds() - start;
    check_pthread(pthread_cond_destroy(&gate.ready_cond), "start: ready_destroy");
    check_pthread(pthread_cond_destroy(&gate.start_cond), "start: cond_destroy");
    check_pthread(pthread_mutex_destroy(&gate.mutex), "start: mutex_destroy");
    free(calls);
    free(threads);
    return elapsed;
}

#endif
