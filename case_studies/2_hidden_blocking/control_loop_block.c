#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>     // clock_gettime(), struct timespec
#include <stdint.h>   // int64_t / uint64_t if you use them
#include <semaphore.h>
#include <pthread.h>
#include <sys/mman.h>
#include <errno.h>

#define PERIOD_NS 5000000  // 5ms
#define SEMAPHORE_UNLOCK_FREQ_NS 7000000 // 7ms
#define WORK_PERIOD_NS 1000000 // 1ms

void *notifier_thread(void *arg);
static void busy_loop_ns(uint64_t ns, uint64_t cntfrq);
static inline void ts_add_ns(struct timespec *ts, long ns);

sem_t sem;

void *notifier_thread(void *arg) {
        // interval
        struct timespec interval = {0, SEMAPHORE_UNLOCK_FREQ_NS};
        for (;;) {
                nanosleep(&interval, NULL);
                sem_post(&sem);
        }
        return NULL;
}

static inline uint64_t read_cntfrq(void) {
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntvct(void) {
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}


static volatile unsigned long counter;
static volatile uint64_t sink;

int main(void) {
        mlockall(MCL_CURRENT | MCL_FUTURE);

        // Setup the busy loop
        uint64_t frq = read_cntfrq();
        printf("cntfrq=%llu Hz\n", (unsigned long long)frq);

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
        // CPU affinity CPU 2
        CPU_ZERO(&cpus);
        CPU_SET(2, &cpus);
        pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
        
        if (pthread_create(&tid, &attr, notifier_thread, NULL) != 0) {
                perror("pthread create failed");
                return 1;
        }
        
        struct timespec next;
        clock_gettime(CLOCK_MONOTONIC, &next);

        // Real control loop
        for (;;) {
                // Next workload
                ts_add_ns(&next, PERIOD_NS);

                // do some computation here (1ms)
                busy_loop_ns(WORK_PERIOD_NS, frq);
                counter = counter % 1000000000 + 1;
                
                // semaphore to block
                sem_wait(&sem);
                
                // sleep until next period
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

