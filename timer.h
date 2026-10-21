#ifndef LAB1_TIMER_H
#define LAB1_TIMER_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* монотонные часы не зависят от изменения системного времени */
static inline double timer_seconds(void) {
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)timestamp.tv_sec + (double)timestamp.tv_nsec / 1e9;
}

#define GET_TIME(now) do { (now) = timer_seconds(); } while (0)

#endif
