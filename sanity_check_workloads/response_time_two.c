#define _GNU_SOURCE

#include <stdio.h>
#include <time.h>     // clock_gettime(), struct timespec
#include <stdint.h>   // int64_t / uint64_t if you use them
#include <semaphore.h>
#include <pthread.h>

#define PERIOD_NS 5000000  // 5ms
#define WORK_PERIOD_NS 1000000 // 1ms

void *notifier_thread(void *arg);
void busy_loop_ns(long long duration_ns);
static inline void ts_add_ns(struct timespec *ts, long ns);


sem_t sem;

void *notifier_thread(void *arg) {
        // interval 
        struct timespec interval = {0, 7* 1000 * 1000};
        for (;;) {
                nanosleep(&interval, NULL);
                sem_post(&sem);
        }
        return NULL;
}

/**
 * The repsonse time all types could be
 * It does some computation, and semaphore is supplied before.
 * So it doesn't block and captures the entire to relief timer loop.
 * Look at min for this stat (max maybe skewed).
 * 
 * Look at max for the other stat - for sure capturing to relief timer.
 */

volatile unsigned long counter;
volatile unsigned long sink;


int main(void) {
        sink = 0;
        struct timespec next;
        clock_gettime(CLOCK_MONOTONIC, &next);
        sem_init(&sem, 0, 0);

        // initialize this thread with some priority on another core
        pthread_t tid;
        pthread_attr_t attr;
        struct sched_param sp;
        cpu_set_t cpus;

        pthread_attr_init(&attr);

        // RT scheduling policy + priority before creating the thread.
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        sp.sched_priority = 50;
        pthread_attr_setschedparam(&attr, &sp);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

        // CPU affinity
        CPU_ZERO(&cpus);
        CPU_SET(2, &cpus);
        pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);

        if (pthread_create(&tid, &attr, notifier_thread, NULL) != 0) {
                perror("pthread create failed");
                return 1;
        }

        for (;;) {
                // do some computation here (1ms)
                busy_loop_ns(WORK_PERIOD_NS);
                counter = counter % 1000000000 + 1;
                
                // semaphore to block (expected about )
                sem_wait(&sem);
                
                // do some computation here (2ms)
                busy_loop_ns(WORK_PERIOD_NS);
                busy_loop_ns(WORK_PERIOD_NS);

                ts_add_ns(&next, PERIOD_NS);
                // sleep for 2ms - (semaphore blocking time). 
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }
}

static inline void ts_add_ns(struct timespec *ts, long ns) {
    ts->tv_nsec += ns;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec++;
    }
}


void busy_loop_ns(long long duration_ns) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    long long start = ts.tv_sec * 1000000000LL + ts.tv_nsec;

    while (1) {
        // prevent compiler from removing the loop
        sink++;

        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        long long now = ts.tv_sec * 1000000000LL + ts.tv_nsec;
        if (now - start >= duration_ns)
            break;
    }
}
