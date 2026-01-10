#include <stdio.h>
#include <time.h>     // clock_gettime(), struct timespec
#include <stdint.h>   // int64_t / uint64_t if you use them

#define PERIOD_NS 1000000  // 5ms, previously. Now, 1ms to use for cyclictest background workload

static inline void ts_add_ns(struct timespec *ts, long ns) {
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}


volatile unsigned long sum = 0;

/**
 * For this loop, we should have relief response time == response time
 */
int main(void) {
        struct timespec next;
        clock_gettime(CLOCK_MONOTONIC, &next);

        for (;;) {
                // Do some some simple thing to keep compiler happy
                sum = sum % 100000000 + 1;
                
                // Schedule next period
                ts_add_ns(&next, PERIOD_NS);
                // Sleep for 5ms
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }
        
        return 0;
}
