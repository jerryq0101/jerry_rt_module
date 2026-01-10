#define _GNU_SOURCE
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sched.h>

static volatile uint64_t sink;

static inline uint64_t read_cntvct(void) {
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntfrq(void) {
    uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

static inline void ts_add_ns(struct timespec *ts, long ns) {
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

static void busy_loop_ns(uint64_t ns, uint64_t cntfrq) {
    // ticks = ns * freq / 1e9
    // Get the target number of ticks to run for (2ms worth of ticks)
    uint64_t target = (ns * cntfrq) / 1000000000ULL;
    // cntvct current hw counter value
    uint64_t start = read_cntvct();

    // Go until hardware increments target amount of ticks
    while ((read_cntvct() - start) < target) {
        sink = sink * 1664525u + 1013904223u;
        sink ^= sink >> 13;
        sink *= 0xff51afd7ed558ccdULL;
    }
}

int main(void) {
    mlockall(MCL_CURRENT | MCL_FUTURE);

    // cntfrq is the hardware counter frequency.
    uint64_t frq = read_cntfrq();
    printf("cntfrq=%llu Hz\n", (unsigned long long)frq);

    const long PERIOD_NS = 10 * 1000 * 1000;   // 10ms
    const uint64_t WORK_NS = 2 * 1000 * 1000;  // 2ms

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    for (;;) {
        ts_add_ns(&next, PERIOD_NS);

        busy_loop_ns(WORK_NS, frq);

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
}
