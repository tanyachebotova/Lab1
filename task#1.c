#define _POSIX_C_SOURCE 200809L

/* Задача 1. Получение заданного количества точек множества Мандельброта */
#include "pthread_utils.h"

typedef struct { double x, y; } point_t;

typedef struct {
    size_t rank, threads, first, count, iterations;
    unsigned char *accepted;
} search_args_t;

/* Обращаем цифры номера в заданном основании для последовательности Халтона */
static double radical_inverse(size_t index, size_t base) {
    double value = 0.0, factor = 1.0 / (double)base;
    while (index != 0) {
        value += (double)(index % base) * factor;
        index /= base;
        factor /= (double)base;
    }
    return value;
}

static point_t candidate(size_t index) {
    return (point_t){-2.0 + 3.0 * radical_inverse(index, 2),
                    -1.5 + 3.0 * radical_inverse(index, 3)};
}

static bool belongs(point_t c, size_t iterations) {
    double x = 0.0, y = 0.0;
    for (size_t k = 0; k < iterations; ++k) {
        const double next_x = x * x - y * y + c.x;
        y = 2.0 * x * y + c.y;
        x = next_x;
        if (x * x + y * y > 4.0) return false;
    }
    /* Конечная проверка даёт численное приближение множества */
    return true;
}

static void *search(void *pointer) {
    const search_args_t *args = pointer;
    const size_t block = 64;
    /* Раздаём блоки циклически, чтобы смешать лёгкие и трудные кандидаты */
    for (size_t first = args->rank * block; first < args->count;
         first += args->threads * block) {
        const size_t end = args->count - first < block ? args->count : first + block;
        for (size_t i = first; i < end; ++i)
            args->accepted[i] = (unsigned char)belongs(candidate(args->first + i),
                                                     args->iterations);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3)
        fail("Использование: mandelbrot nthreads npoints [--max-iter N] "
             "[--output FILE] [--no-output]");
    const size_t threads = parse_size(argv[1], "nthreads", false);
    const size_t wanted = parse_size(argv[2], "npoints", true);
    if (threads > 4096) fail("Число потоков не должно превышать 4096");
    size_t iterations = 2000;
    const char *output = "mandelbrot.csv";
    bool no_output = false;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--max-iter") == 0 && i + 1 < argc)
            iterations = parse_size(argv[++i], "max-iter", false);
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (strcmp(argv[i], "--no-output") == 0)
            no_output = true;
        else fail("Неизвестный параметр или пропущено его значение");
    }
    point_t *points = allocate_array(wanted, sizeof(*points));
    search_args_t *args = allocate_array(threads, sizeof(*args));
    size_t found = 0, first = 1, tested = 0;
    double parallel_seconds = 0.0;
    const double start = timer_seconds();
    while (found < wanted) {
        const size_t remaining = wanted - found;
        if (remaining > (SIZE_MAX - 8192) / 7) fail("Слишком большое число точек");
        const size_t count = remaining * 7 > 8192 ? remaining * 7 : 8192;
        if (first > SIZE_MAX - count) fail("Переполнение индекса кандидата");
        /* Каждый поток записывает свои байты, нет общей записи в один элемент */
        unsigned char *accepted = allocate_array(count, sizeof(*accepted));
        for (size_t t = 0; t < threads; ++t)
            args[t] = (search_args_t){t, threads, first, count, iterations, accepted};
        parallel_seconds += run_parallel(threads, args, sizeof(*args), search);
        /* Порядок точек не зависит от порядка завершения потоков */
        for (size_t i = 0; i < count && found < wanted; ++i)
            if (accepted[i]) points[found++] = candidate(first + i);
        free(accepted);
        first += count;
        tested += count;
    }
    const double compute_seconds = timer_seconds() - start;
    if (!no_output) {
        FILE *file = fopen(output, "w");
        if (file == NULL) fail("Не удалось открыть CSV для записи");
        fprintf(file, "x,y\n");
        for (size_t i = 0; i < wanted; ++i)
            fprintf(file, "%.17g,%.17g\n", points[i].x, points[i].y);
        const bool write_error = ferror(file) != 0;
        const int close_error = fclose(file);
        if (write_error || close_error != 0) fail("Не удалось записать CSV");
    }
    printf("threads=%zu points=%zu tested=%zu max_iter=%zu "
           "parallel_seconds=%.9f compute_seconds=%.9f\n",
           threads, found, tested, iterations, parallel_seconds, compute_seconds);
    free(args);
    free(points);
    return EXIT_SUCCESS;
}
