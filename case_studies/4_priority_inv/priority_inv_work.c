#define _GNU_SOURCE

#include <pthread.h> // pthread_*, pthread_mutex_*, pthread_attr_*
#include <time.h> // struct timespec, nanosleep, clock_gettime, clock_nanosleep
#include <stdint.h> // uint64_t, etc
#include <stdio.h> // printf, perror
#include <errno.h> // errno (optional but useful)
#include <string.h> // strerror (optional)
#include <sys/mman.h> // mlockall, MCL_CURRENT, MCL_FUTURE
#include <sched.h> // SCHED_FIFO, CPU_SET/CPU_ZERO, cpu_set_t
#include <unistd.h> // (optional) for misc POSIX things

/**
 * Priorities (SCHED_FIFO):
 * High = 90
 * Med = 50
 * Low = 10
 */
#define LOW_PRIO_WORK_PERIOD 2000000   // 2ms
#define LOW_PRIO_SLEEP_PERIOD 100000    // 0.1ms

#define MED_PRIO_WORK_PERIOD 10000000   // 10ms
#define MED_PRIO_SLEEP_PERIOD 1000000    // 1ms

#define HIGH_PRIO_SLEEP_PERIOD 5000000 // 5ms
#define HIGH_PRIO_WORK_PERIOD 1000000 // 1ms

pthread_mutex_t lock;
uint64_t frq;

static volatile unsigned long counter;
static volatile uint64_t sink;

static void busy_loop_ns(uint64_t ns, uint64_t cntfrq);

static inline struct timespec ns_to_ts(uint64_t ns)
{
	struct timespec ts;
	ts.tv_sec = ns / 1000000000ULL;
	ts.tv_nsec = ns % 1000000000ULL;
	return ts;
}

static inline void ts_add_ns(struct timespec *t, uint64_t ns)
{
	t->tv_nsec += ns;
	while (t->tv_nsec >= 1000000000ULL) {
		t->tv_nsec -= 1000000000ULL;
		t->tv_sec++;
	}
}

// Setup PriorityInversion mutex
static void init_pi_mutex(void)
{
	pthread_mutexattr_t ma;
	int rc;

	rc = pthread_mutexattr_init(&ma);
	if (rc) {
		fprintf(stderr, "attr_init: %s\n", strerror(rc));
		return;
	}
	rc = pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
	if (rc) {
		fprintf(stderr, "setprotocol: %s\n", strerror(rc));
		return;
	}

	// Optional but fine:
	rc = pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_NORMAL);
	if (rc) {
		fprintf(stderr, "settype: %s\n", strerror(rc));
		return;
	}

	rc = pthread_mutex_init(&lock, &ma);
	if (rc) {
		fprintf(stderr, "mutex_init: %s\n", strerror(rc));
		return;
	}

	pthread_mutexattr_destroy(&ma);
}

/**
 * Grabs lock that the high prio thread needs.
 * And "hogs that".
 * High prio thread will block there, med thread will run, and prevent
 * this thread from unlocking. 
 * Therefore high prio thread will remain blocked while waiting for the lock
 */
void *low_prio_thread(void *arg)
{
	for (;;) {
		pthread_mutex_lock(&lock);

		busy_loop_ns(LOW_PRIO_WORK_PERIOD, frq);

		pthread_mutex_unlock(&lock);

		// Sleep X from now
		// TODO: making sure that the low prio thread actually runs to hold it (maybe doing a multicore situation is better)
		// What is going on right now:
		// low gets spawned on CPU2 and then grabs lock
		// med gets in preempts low
		// so med just continuously runs while low holds the lock
		// The High prio program runs on another CPU and just can't unlock.

		// TODO: Implement a quick initialize so that it captures the initial behaviour
		struct timespec ts = ns_to_ts(LOW_PRIO_SLEEP_PERIOD);
		nanosleep(&ts, NULL);
	}
}

/**
 * CPU Hog: busy loop almost continuously (wtih small sleeps)
 * Inversion amplifier -> preempts L and preventing L from 
 * running to release the mutex.
 */
void *med_prio_thread(void *arg)
{
	for (;;) {
		busy_loop_ns(MED_PRIO_WORK_PERIOD, frq);

		// Sleep X from now
		struct timespec ts = ns_to_ts(MED_PRIO_SLEEP_PERIOD);
		nanosleep(&ts, NULL);
	}
}

static inline uint64_t read_cntfrq(void)
{
	uint64_t v;
	asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static inline uint64_t read_cntvct(void)
{
	uint64_t v;
	asm volatile("mrs %0, cntvct_el0" : "=r"(v));
	return v;
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

int main(void)
{
	// Initialize Busy loop stuff
	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
		perror("mlockall");
	}

        // Setup the lock
        init_pi_mutex();

	// Find the freq for the busy loop
	frq = read_cntfrq();
	printf("cntfrq=%llu Hz\n", (unsigned long long)frq);

	// Initialize 3 threads
	// L - low prio
	// M - medium prio (CPU hog)
	// H - high prio (make this the main process)

	// need a mutex
	// and just set these up individually

	// Convert these two to low and medium threads
	// Notifier thread and blocking thread
	pthread_t ltid, mtid;
	pthread_attr_t lattr, mattr;
	struct sched_param lsp = { .sched_priority = 10 }, msp = { .sched_priority = 50 };
	cpu_set_t lcpu, mcpu;

	pthread_attr_init(&lattr);
	pthread_attr_init(&mattr);

	// Make sure attrs actually take effect (don’t inherit parent thread policy)
	pthread_attr_setinheritsched(&lattr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setinheritsched(&mattr, PTHREAD_EXPLICIT_SCHED);

	// Scheduling policy
	pthread_attr_setschedpolicy(&lattr, SCHED_FIFO);
	pthread_attr_setschedpolicy(&mattr, SCHED_FIFO);

	// Priority (pick what you want)
	pthread_attr_setschedparam(&lattr, &lsp);
	pthread_attr_setschedparam(&mattr, &msp);

	// CPU affinity: blocking thread on CPU2, notifier thread on CPU3
	CPU_ZERO(&lcpu);
	CPU_SET(2, &lcpu);
	pthread_attr_setaffinity_np(&lattr, sizeof(lcpu), &lcpu);

	CPU_ZERO(&mcpu);
	CPU_SET(2, &mcpu);
	pthread_attr_setaffinity_np(&mattr, sizeof(mcpu), &mcpu);

	// Create threads with their own attrs
	if (pthread_create(&ltid, &lattr, low_prio_thread, NULL) != 0) {
		perror("pthread_create blocking_thread");
	}
	if (pthread_create(&mtid, &mattr, med_prio_thread, NULL) != 0) {
		perror("pthread_create event_thread");
	}

	// Main high priority loop
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);

	// Pretend we have absolute time
	for (;;) {
		// Next workload
		ts_add_ns(&next, HIGH_PRIO_SLEEP_PERIOD);

		// do some computation here (1ms)
		busy_loop_ns(HIGH_PRIO_WORK_PERIOD, frq);
		counter = counter % 1000000000 + 1;

		// mutex acquisition
		pthread_mutex_lock(&lock);
		pthread_mutex_unlock(&lock);

		// sleep until next period
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
}
