/**
 * Purpose is to demonstrate that for a background process that does not adhere to a
 * control loop like fashion, this can still be useful.
 * 
 * A event driven telemetry burst on event. (mimicing a irq pull, and then the task would wake up)
 * 
 * Do a burst of CPU work and then do a write. and accidentally include an extra voluntary sleep.
 * This showcases a bad case for the tool.
 */
#define _GNU_SOURCE

#include <sys/eventfd.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sched.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/mman.h>

#define WORK_PERIOD_NS 1000000 // 1ms
#define SEMAPHORE_UNLOCK_FREQ_NS 7000000 // 7ms

// semaphore for blocking, efd for event triggered wakeups
sem_t sem;
static int efd;

static volatile uint64_t sink;
static uint32_t lcg = 1234567;

static void busy_loop_ns(uint64_t ns, uint64_t cntfrq);

static void nsleep_ns(long ns) {
        struct timespec ts = { .tv_sec = ns/1000000000L, .tv_nsec = ns%1000000000L };
        nanosleep(&ts, NULL);
}

static inline uint32_t fast_rand(void)
{
        lcg = lcg * 1103515245 + 12345;
        return lcg;
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


void *event_thread(void *arg) {
        for (;;) {
                long jitter = 500000 + (fast_rand() % 5000000);
                nsleep_ns(jitter);
                uint64_t one = 1;
                write(efd, &one, sizeof(one));
        }
        return NULL;
}

void *blocking_thread(void *arg) {
        // interval
        struct timespec interval = {0, SEMAPHORE_UNLOCK_FREQ_NS};
        for (;;) {
                nanosleep(&interval, NULL);
                sem_post(&sem);
        }
        return NULL;
}


int main(void) {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
                perror("mlockall");
        }

        // Find the freq for the busy loop
        uint64_t frq = read_cntfrq();
        printf("cntfrq=%llu Hz\n", (unsigned long long)frq);

        // Setup blocking semaphore
        if (sem_init(&sem, 0, 0) != 0) {
                perror("sem_init");
                return 1;
        }
        
        // Setup the eventfd obj
        efd = eventfd(0, 0);
        if (efd < 0) {
                perror("eventfd");
                return 1;
        }

        // Notifier thread and blocking thread
        pthread_t btid, ntid;
        pthread_attr_t battr, nattr;
        struct sched_param bsp = {0}, nsp = {0};
        cpu_set_t bcpu, ncpu;

        pthread_attr_init(&battr);
        pthread_attr_init(&nattr);

        // Make sure attrs actually take effect (don’t inherit parent thread policy)
        pthread_attr_setinheritsched(&battr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setinheritsched(&nattr, PTHREAD_EXPLICIT_SCHED);

        // Scheduling policy
        pthread_attr_setschedpolicy(&battr, SCHED_FIFO);
        pthread_attr_setschedpolicy(&nattr, SCHED_FIFO);

        // Priority (pick what you want)
        bsp.sched_priority = 50;
        nsp.sched_priority = 50;
        pthread_attr_setschedparam(&battr, &bsp);
        pthread_attr_setschedparam(&nattr, &nsp);

        // CPU affinity: blocking thread on CPU2, notifier thread on CPU3
        CPU_ZERO(&bcpu);
        CPU_SET(2, &bcpu);
        pthread_attr_setaffinity_np(&battr, sizeof(bcpu), &bcpu);

        CPU_ZERO(&ncpu);
        CPU_SET(3, &ncpu);
        pthread_attr_setaffinity_np(&nattr, sizeof(ncpu), &ncpu);

        // Create threads with their own attrs
        if (pthread_create(&btid, &battr, blocking_thread, NULL) != 0) {
                perror("pthread_create blocking_thread");
        }
        if (pthread_create(&ntid, &nattr, event_thread, NULL) != 0) {
                perror("pthread_create event_thread");
        }

        for (;;) {
                uint64_t v;
                read(efd, &v, sizeof(v));

                busy_loop_ns(WORK_PERIOD_NS, frq);

                // accidental voluntary sleep
                sem_wait(&sem);
        }
}

static void busy_loop_ns(uint64_t ns, uint64_t cntfrq)
{
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

