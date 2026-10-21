#define _POSIX_C_SOURCE 200809L

/* Задача 2. Движение N тел */

#include "pthread_utils.h"
#include <float.h>
#include <stdatomic.h>

static const double G = 6.67430e-11;

typedef struct {
    double mass;
    double r[3], v[3];
} body_t;

typedef struct {
    size_t n, threads, steps;
    double end, dt;
    body_t *bodies;
    double *forces;
    barrier_t barrier;
    atomic_bool failed;
    FILE *output;
    bool xyz, stop;
} simulation_t;

typedef struct {
    simulation_t *simulation;
    size_t rank, pair_first, pair_end;
} simulation_args_t;

/* Число пар перед строкой i верхнего треугольника */
static size_t pair_offset(size_t n, size_t i) {
    return i * (2 * n - i - 1) / 2;
}

static size_t first_row(size_t n, size_t pair) {
    size_t low = 0, high = n - 1;
    while (low < high) {
        const size_t middle = low + (high - low + 1) / 2;
        if (pair_offset(n, middle) <= pair) low = middle;
        else high = middle - 1;
    }
    return low;
}

static void write_state(simulation_t *s, double time) {
    if (s->output == NULL) return;
    fprintf(s->output, "%.17g", time);
    for (size_t i = 0; i < s->n; ++i)
        for (size_t d = 0; d < (s->xyz ? 3u : 2u); ++d)
            fprintf(s->output, ",%.17g", s->bodies[i].r[d]);
    fputc('\n', s->output);
    if (ferror(s->output)) atomic_store(&s->failed, true);
}

static void *integrate(void *pointer) {
    const simulation_args_t *args = pointer;
    simulation_t *s = args->simulation;
    /* У каждого потока свой массив сил для всех тел */
    double *force = s->forces + args->rank * s->n * 3;
    double previous_time = 0.0;
    for (size_t step = 0; step < s->steps; ++step) {
        const double time = step + 1 == s->steps ? s->end :
                            fmin(s->end, (double)(step + 1) * s->dt);
        const double dt = time - previous_time;
        previous_time = time;
        memset(force, 0, s->n * 3 * sizeof(*force));
        size_t i = first_row(s->n, args->pair_first);
        size_t j = i + 1 + args->pair_first - pair_offset(s->n, i);
        for (size_t pair = args->pair_first; pair < args->pair_end; ++pair) {
            double delta[3], squared = 0.0;
            for (size_t d = 0; d < 3; ++d) {
                delta[d] = s->bodies[j].r[d] - s->bodies[i].r[d];
                squared += delta[d] * delta[d];
            }
            if (!(squared > 0.0) || !isfinite(squared)) {
                atomic_store(&s->failed, true);
            } else {
                const double factor = G * s->bodies[i].mass * s->bodies[j].mass /
                                      (squared * sqrt(squared));
                for (size_t d = 0; d < 3; ++d) {
                    const double f = factor * delta[d];
                    if (!isfinite(f)) atomic_store(&s->failed, true);
                    /* Третий закон Ньютона (пару считаем только один раз) */
                    force[3 * i + d] += f;
                    force[3 * j + d] -= f;
                }
            }
            if (++j == s->n) { ++i; j = i + 1; }
        }
        /* Старые координаты нужны всем потокам до завершения расчёта сил */
        barrier_wait(&s->barrier);
        const size_t remainder = s->n % s->threads;
        const size_t begin = (s->n / s->threads) * args->rank +
                             (args->rank < remainder ? args->rank : remainder);
        const size_t end = begin + s->n / s->threads + (args->rank < remainder);
        for (size_t q = begin; q < end; ++q) {
            for (size_t d = 0; d < 3; ++d) {
                double total = 0.0;
                for (size_t t = 0; t < s->threads; ++t)
                    total += s->forces[(t * s->n + q) * 3 + d];
                /* Положение считаем по старой скорости, затем меняем скорость */
                s->bodies[q].r[d] += dt * s->bodies[q].v[d];
                s->bodies[q].v[d] += dt * total / s->bodies[q].mass;
                if (!isfinite(s->bodies[q].r[d]) || !isfinite(s->bodies[q].v[d]))
                    atomic_store(&s->failed, true);
            }
        }
        barrier_wait(&s->barrier);
        if (args->rank == 0) {
            if (!atomic_load(&s->failed)) write_state(s, time);
            s->stop = atomic_load(&s->failed);
        }
        /* Все потоки получают решение об остановке до следующего шага */
        barrier_wait(&s->barrier);
        if (s->stop) break;
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3)
        fail("Использование: nbody tend input [--threads P] [--dt H] "
             "[--output FILE] [--xyz] [--no-output]");
    simulation_t s = {0};
    atomic_init(&s.failed, false);
    s.threads = 4;
    s.end = parse_real(argv[1], "tend");
    if (s.end < 0.0) fail("Конечное время должно быть неотрицательным");
    s.dt = s.end > 0.0 ? s.end / 1000.0 : 1.0;
    const char *output = "trajectories.csv";
    bool no_output = false;
    for (int k = 3; k < argc; ++k) {
        if (strcmp(argv[k], "--threads") == 0 && k + 1 < argc)
            s.threads = parse_size(argv[++k], "threads", false);
        else if (strcmp(argv[k], "--dt") == 0 && k + 1 < argc)
            s.dt = parse_real(argv[++k], "dt");
        else if (strcmp(argv[k], "--output") == 0 && k + 1 < argc)
            output = argv[++k];
        else if (strcmp(argv[k], "--xyz") == 0) s.xyz = true;
        else if (strcmp(argv[k], "--no-output") == 0) no_output = true;
        else fail("Неизвестный параметр или пропущено его значение");
    }
    if (!(s.dt > 0.0) || s.threads > 4096)
        fail("Шаг должен быть положительным; число потоков не более 4096");
    const double ratio = s.end / s.dt, nearest = round(ratio);
    double steps = fabs(ratio - nearest) <= 16 * DBL_EPSILON * fmax(1.0, ratio)
                   ? nearest : ceil(ratio);
    if (s.end > 0.0 && steps < 1.0) steps = 1.0;
    if (!isfinite(steps) || steps >= (double)SIZE_MAX)
        fail("Слишком большое число шагов по времени");
    s.steps = (size_t)steps;

    FILE *input = fopen(argv[2], "r");
    if (input == NULL) fail("Не удалось открыть входной файл");
    char token[256];
    if (fscanf(input, "%255s", token) != 1) fail("Не задано количество тел");
    s.n = parse_size(token, "число тел", false);
    if (s.n > SIZE_MAX / 2 || s.n > SIZE_MAX / s.n)
        fail("Слишком большое число тел");
    s.bodies = allocate_array(s.n, sizeof(*s.bodies));
    for (size_t i = 0; i < s.n; ++i) {
        body_t *body = &s.bodies[i];
        if (fscanf(input, "%lf %lf %lf %lf %lf %lf %lf", &body->mass,
                   &body->r[0], &body->r[1], &body->r[2],
                   &body->v[0], &body->v[1], &body->v[2]) != 7)
            fail("Для каждого тела нужны mass, x, y, z, vx, vy, vz");
        if (!(body->mass > 0.0) || !isfinite(body->mass))
            fail("Масса должна быть положительной и конечной");
        for (size_t d = 0; d < 3; ++d)
            if (!isfinite(body->r[d]) || !isfinite(body->v[d]))
                fail("Координаты и скорости должны быть конечными");
    }
    char extra;
    if (fscanf(input, " %c", &extra) == 1) fail("Лишние данные во входном файле");
    const bool input_error = ferror(input) != 0;
    const int input_close_error = fclose(input);
    if (input_error || input_close_error != 0) fail("Ошибка чтения входного файла");
    if (s.n > SIZE_MAX / 3 / s.threads) fail("Слишком большой массив сил");
    s.forces = allocate_array(s.n * 3 * s.threads, sizeof(*s.forces));
    simulation_args_t *args = allocate_array(s.threads, sizeof(*args));
    const size_t pairs = s.n * (s.n - 1) / 2;
    size_t offset = 0;
    for (size_t t = 0; t < s.threads; ++t) {
        const size_t count = pairs / s.threads + (t < pairs % s.threads);
        args[t] = (simulation_args_t){&s, t, offset, offset + count};
        offset += count;
    }
    if (!no_output) {
        s.output = fopen(output, "w");
        if (s.output == NULL) fail("Не удалось открыть выходной файл");
        fputc('t', s.output);
        for (size_t q = 1; q <= s.n; ++q) {
            fprintf(s.output, ",x%zu,y%zu", q, q);
            if (s.xyz) fprintf(s.output, ",z%zu", q);
        }
        fputc('\n', s.output);
        write_state(&s, 0.0);
    }
    
barrier_init(&s.barrier, s.threads);
    const double seconds = run_parallel(s.threads, args, sizeof(*args), integrate);
    barrier_destroy(&s.barrier);
    if (s.output != NULL && fclose(s.output) != 0) atomic_store(&s.failed, true);
    const bool failed = atomic_load(&s.failed);
    free(args);
    free(s.forces);
    free(s.bodies);
    if (failed)
        fail("Совпадение координат тел, переполнение вычислений или ошибка записи CSV; "
             "результат неполный");
    printf("threads=%zu bodies=%zu steps=%zu dt=%.17g "
           "pair_evaluations_per_step=%zu parallel_seconds=%.9f csv=%s\n",
           s.threads, s.n, s.steps, s.dt, pairs, seconds, no_output ? "off" : "on");
    return EXIT_SUCCESS;
}
