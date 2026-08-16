#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct run_state {
    pthread_mutex_t *mutex;
    clockid_t deadline_clock;
    uint64_t timeout_count;
    uint64_t wall_us;
    uint64_t cpu_us;
    int result;
};

static uint64_t timespec_us(const struct timespec *value)
{
    return (uint64_t)value->tv_sec * 1000000ULL +
           (uint64_t)value->tv_nsec / 1000ULL;
}

static void add_ms(struct timespec *value, long milliseconds)
{
    value->tv_nsec += milliseconds * 1000000L;
    value->tv_sec += value->tv_nsec / 1000000000L;
    value->tv_nsec %= 1000000000L;
}

static void *contended_lock(void *opaque)
{
    struct run_state *state = opaque;
    struct timespec wall_start, wall_end, cpu_start, cpu_end;

    clock_gettime(CLOCK_MONOTONIC, &wall_start);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start);

    for (;;) {
        struct timespec deadline;
        int result;

        clock_gettime(state->deadline_clock, &deadline);
        add_ms(&deadline, 33);
        result = pthread_mutex_timedlock(state->mutex, &deadline);
        if (result == ETIMEDOUT) {
            state->timeout_count++;
            continue;
        }
        state->result = result;
        break;
    }

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end);
    clock_gettime(CLOCK_MONOTONIC, &wall_end);
    state->wall_us = timespec_us(&wall_end) - timespec_us(&wall_start);
    state->cpu_us = timespec_us(&cpu_end) - timespec_us(&cpu_start);

    if (state->result == 0)
        pthread_mutex_unlock(state->mutex);
    return NULL;
}

static int run_case(clockid_t deadline_clock, struct run_state *state)
{
    pthread_mutexattr_t attributes;
    pthread_mutex_t mutex;
    pthread_t thread;
    struct timespec hold = { .tv_sec = 0, .tv_nsec = 50000000L };
    int result;

    result = pthread_mutexattr_init(&attributes);
    if (result != 0)
        return result;
    result = pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    if (result == 0)
        result = pthread_mutex_init(&mutex, &attributes);
    pthread_mutexattr_destroy(&attributes);
    if (result != 0)
        return result;

    pthread_mutex_lock(&mutex);
    *state = (struct run_state) {
        .mutex = &mutex,
        .deadline_clock = deadline_clock,
    };
    result = pthread_create(&thread, NULL, contended_lock, state);
    if (result == 0) {
        nanosleep(&hold, NULL);
        pthread_mutex_unlock(&mutex);
        result = pthread_join(thread, NULL);
    } else {
        pthread_mutex_unlock(&mutex);
    }
    pthread_mutex_destroy(&mutex);
    return result != 0 ? result : state->result;
}

int main(void)
{
    struct run_state monotonic = {0};
    struct run_state realtime = {0};
    int monotonic_result = run_case(CLOCK_MONOTONIC, &monotonic);
    int realtime_result = run_case(CLOCK_REALTIME, &realtime);
    int pass = monotonic_result == 0 && realtime_result == 0 &&
               monotonic.timeout_count > realtime.timeout_count &&
               monotonic.cpu_us > realtime.cpu_us;

    printf("{\"monotonic\":{\"result\":%d,\"timeouts\":%llu,"
           "\"wall_us\":%llu,\"cpu_us\":%llu},"
           "\"realtime\":{\"result\":%d,\"timeouts\":%llu,"
           "\"wall_us\":%llu,\"cpu_us\":%llu},\"pass\":%s}\n",
           monotonic_result,
           (unsigned long long)monotonic.timeout_count,
           (unsigned long long)monotonic.wall_us,
           (unsigned long long)monotonic.cpu_us,
           realtime_result,
           (unsigned long long)realtime.timeout_count,
           (unsigned long long)realtime.wall_us,
           (unsigned long long)realtime.cpu_us,
           pass ? "true" : "false");
    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
