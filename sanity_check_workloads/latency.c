#include <stdio.h>
#include <time.h>     // clock_gettime(), struct timespec
#include <stdint.h>   // int64_t / uint64_t if you use them

#define PERIOD_NS 5000000  // 5ms
#define WORK_PERIOD_NS 4000000 // 4ms

volatile unsigned long sink;
void busy_loop_ns(long long duration_ns);

static inline void ts_add_ns(struct timespec *ts, long ns) {
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}

int main(void) {
        sink = 0;
        struct timespec next;
        clock_gettime(CLOCK_MONOTONIC, &next);

        for (;;) {
                // Do some random work for 4ms
                busy_loop_ns(WORK_PERIOD_NS);
                
                // schedule next period
                ts_add_ns(&next, PERIOD_NS);
                // sleep for 5ms
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }
}


void busy_loop_ns(long long duration_ns) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    long long start = ts.tv_sec * 1000000000LL + ts.tv_nsec;

    while (1) {
        sink++;  // prevent compiler from removing the loop

        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        long long now = ts.tv_sec * 1000000000LL + ts.tv_nsec;
        if (now - start >= duration_ns)
            break;
    }
}
