#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/tracepoint.h>
#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/limits.h>
#include <linux/compiler.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/interrupt.h>
#include <linux/atomic.h>
#include <asm/unistd.h>
#include <asm/cmpxchg.h>
#include "trace_ring_buffer.h"

#define PIDTAB_SIZE 65536
#define MAX_TRACED_PIDS 10
#define MAX_TRACE_LEN_PER_SLO 10

#define START_INVALID -1

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jerry Qi");
MODULE_DESCRIPTION("A Simple Kernel Module to track latency");
MODULE_VERSION("1.0");

static struct tracepoint *tp_sched_switch;
static struct tracepoint *tp_sched_wakeup;
static struct tracepoint *tp_sys_enter;
static struct tracepoint *tp_irq_threaded_handler_entry;
static struct tracepoint *tp_irq_threaded_handler_exit;
static struct tracepoint *tp_sched_process_exit;

// Global value for doing evaluation
static atomic64_t total_probe_hits;
static atomic64_t total_time_ns;
static atomic64_t max_time_ns;

enum stat_field { LATENCY, RESPONSE, RESPONSE_RELIEF, IRQ_HANDLING };

enum sleep_cause {
	SC_NONE = 0,
	SC_TIMER,
	SC_OTHER, // FUTURE: SC_LOCK, SC_IO, etc for other potential state tracking
};

struct task_stat_slo {
	// For Latency
	s64 latency_bound;
	atomic_t latency_violations;
	int max_l_start_index;
	int max_l_trace_len;
	struct slo_event *max_l_violation_trace;

	// For Response time voluntary sleep
	s64 response_bound;
	atomic_t response_violations;
	int max_r_start_index;
	int max_r_trace_len;
	struct slo_event *max_rt_violation_trace;

	// Response time relief sleep
	s64 response_relief_bound;
	atomic_t response_relief_violations;
	int max_rr_start_index;
	int max_rr_trace_len;
	struct slo_event *max_rtr_violation_trace;

	// IRQ tracepoints
	s64 irq_handling_bound;
	atomic_t irq_handling_violations;
	int max_i_start_index;
	int max_i_trace_len;
	struct slo_event *max_irqt_violation_trace;
};

struct task_latency_entry {
	u32 gen; // How many times this pid has been reused
	bool active; // Pid being tracked
	int pid; // Current pid
	char comm[TASK_COMM_LEN]; // Task name

	s64 latency_min_ns; // Minimum latency
	s64 latency_max_ns; // Maximum latency
	s64 latency_last_wakeup_ns; // Timestamp of the sched_wake event

	// Response time measurement - voluntary sleep
	// Def'n = latest sleep (because of any reason) - latest wake up
	s64 curr_wu_ns; // Wake used by this activation (copied at switch in)
	s64 rt_voluntary_min_ns;
	s64 rt_voluntary_max_ns;

	// Specific Response time tracking
	// Defn of response time = time_"relief-ed"_from_its_duty - time_awaken
	enum sleep_cause scause; // cause of next sleep
	enum sleep_cause last_sleep_cause; // last sleep cause needed because need to know if we came back from HW or lock, or we had a relief and this is a new cycle
	s64 cycle_start_ns; // When the control loop iteration started
	bool cycle_active;

	s64 rt_relief_max_ns; // Max response time
	s64 rt_relief_min_ns; // Min Response time

	// IRQ handler (kernel patch) statistics
	s64 last_handler_call_entry; 
	s64 irq_handle_max_ns; // max single interrupt handling iteration in the loop
	s64 irq_handle_min_ns;

	// SLO implementation
	struct task_stat_slo v;
	bool trace_enabled;

	// Trace locks to prevent traces from getting destroyed
	raw_spinlock_t lat_trace_lock;
	raw_spinlock_t resp_trace_lock;
	raw_spinlock_t resp_relief_trace_lock;
	raw_spinlock_t irq_trace_lock;
};

enum slo_trace_mode {
	TRACE_LATENCY,
	TRACE_RESP_ALL,
	TRACE_RESP_RELIEF,
	TRACE_IRQT,
};

static void update_violation(enum stat_field field, s64 value, struct task_latency_entry *e);
int collect_latency_violation_trace(struct task_latency_entry *e, int violation_event_index);
int collect_response_violation_trace(struct task_latency_entry *entry, int violation_event_index);
int collect_response_relief_violation_trace(struct task_latency_entry *entry, int violation_event_index);
int collect_irqt_violation_trace(struct task_latency_entry *entry, int violation_event_index);

static bool should_include_event(enum slo_trace_mode mode, struct slo_event *se, pid_t task_pid, int len_so_far, int start_index);
static int collect_slo_trace_common(struct task_latency_entry *e, int start_index, int violation_event_index, struct slo_event *dst,
				    int *dst_len, enum slo_trace_mode mode);

static void print_single_trace(struct task_latency_entry *e, struct task_stat_slo *v, struct slo_event *printed_arr, int len,
			       enum stat_field event_type);

static __always_inline bool cas_update_max_s64(s64 *p, s64 v);
static __always_inline bool cas_update_min_s64(s64 *p, s64 v);

#define DEFINE_SLO_SETTER(name, field)                                                                                             \
	static void set_##name(pid_t pid, s64 value)                                                                               \
	{                                                                                                                          \
		struct task_latency_entry *e = slot_for(pid);                                                                      \
		if (e) {                                                                                                           \
			struct task_stat_slo *s = &e->v;                                                                           \
			WRITE_ONCE(s->field, value);                                                                               \
		}                                                                                                                  \
	}

static struct task_latency_entry *pidtab;

static inline struct task_latency_entry *slot_for(pid_t pid)
{
	if (pid < 0 || pid >= PIDTAB_SIZE) {
		return NULL;
	} else {
		return &pidtab[pid];
	}
}

/*
Helper functions
*/

/**
 * Only return if the max was updated for a violation
 */
#define UPDATE_MINMAX_FIELD_CAS(e, v, min_field, max_field)                                                                        \
	({                                                                                                                         \
		bool __max_updated = cas_update_max_s64(&(e)->max_field, (v));                                                     \
		(void)cas_update_min_s64(&(e)->min_field, (v));                                                                    \
		__max_updated;                                                                                                     \
	})

static __always_inline void maybe_update_trace_locked(struct task_latency_entry *e, enum stat_field choice, s64 v,
						      int violation_event_index)
{
	/* Re-check: only the *current* max should own the trace */
	switch (choice) {
	case LATENCY:
		if (READ_ONCE(e->latency_max_ns) == v)
			collect_latency_violation_trace(e, violation_event_index);
		break;
	case RESPONSE:
		if (READ_ONCE(e->rt_voluntary_max_ns) == v)
			collect_response_violation_trace(e, violation_event_index);
		break;
	case RESPONSE_RELIEF:
		if (READ_ONCE(e->rt_relief_max_ns) == v)
			collect_response_relief_violation_trace(e, violation_event_index);
		break;
	case IRQ_HANDLING:
		if (READ_ONCE(e->irq_handle_max_ns) == v)
			collect_irqt_violation_trace(e, violation_event_index);
		break;
	default:
		return;
	}
}

static __always_inline void update_minmax(enum stat_field choice, struct task_latency_entry *e, s64 v, int violation_event_index)
{
	unsigned long flags;

	switch (choice) {
	case LATENCY:
		if (UPDATE_MINMAX_FIELD_CAS(e, v, latency_min_ns, latency_max_ns)) {
			raw_spin_lock_irqsave(&e->lat_trace_lock, flags);
                        if (READ_ONCE(e->trace_enabled) && READ_ONCE(e->latency_max_ns) == v) {
                                collect_latency_violation_trace(e, violation_event_index);
                        }
			raw_spin_unlock_irqrestore(&e->lat_trace_lock, flags);
		}
		break;
	case RESPONSE:
		if (UPDATE_MINMAX_FIELD_CAS(e, v, rt_voluntary_min_ns, rt_voluntary_max_ns)) {
			raw_spin_lock_irqsave(&e->resp_trace_lock, flags);
                        if (READ_ONCE(e->trace_enabled) && READ_ONCE(e->rt_voluntary_max_ns) == v) {
                                collect_response_violation_trace(e, violation_event_index);
                        }
			raw_spin_unlock_irqrestore(&e->resp_trace_lock, flags);
		}
		break;
	case RESPONSE_RELIEF:
		if (UPDATE_MINMAX_FIELD_CAS(e, v, rt_relief_min_ns, rt_relief_max_ns)) {
			raw_spin_lock_irqsave(&e->resp_relief_trace_lock, flags);
                        // check if trace corresponds to current max/min.
                        if (READ_ONCE(e->trace_enabled) && READ_ONCE(e->rt_relief_max_ns) == v) {
                                collect_response_relief_violation_trace(e, violation_event_index);
                        }
			raw_spin_unlock_irqrestore(&e->resp_relief_trace_lock, flags);
		}
		break;
	case IRQ_HANDLING:
                // If the max was updated
		if (UPDATE_MINMAX_FIELD_CAS(e, v, irq_handle_min_ns, irq_handle_max_ns)) {
                        // Then take the lock
			raw_spin_lock_irqsave(&e->irq_trace_lock, flags);
                        // if it is trace enabled and also if the irq handling max is what we are saying right here, then collect this
                        if (READ_ONCE(e->trace_enabled) && READ_ONCE(e->irq_handle_max_ns) == v) {
                                collect_irqt_violation_trace(e, violation_event_index);
                        }
			raw_spin_unlock_irqrestore(&e->irq_trace_lock, flags);
		}
		break;
	default:
		/* nothing */
		break;
	}
}

/**
 * Update value at p with v if v is a greater value
 * 
 */
static __always_inline bool cas_update_max_s64(s64 *p, s64 v)
{
	s64 old = READ_ONCE(*p);

	while (v > old) {
		s64 prev = cmpxchg64(p, old, v);
		if (prev == old) {
			return true;
		}
		old = prev;
	}
	return false;
}

/**
 * Update value at p with v if v is a lesser value
 * 
 */
static __always_inline bool cas_update_min_s64(s64 *p, s64 v)
{
	s64 old = READ_ONCE(*p);

	while (v < old) {
		s64 prev = cmpxchg64(p, old, v);
		if (prev == old) {
			return true;
		}
		old = prev;
	}
	return false;
}

/*
Adding a pid to be recorded, and removing a pid to be recorded
*/
static int start_recording_pid(pid_t pid, bool traced)
{
	struct task_latency_entry *e = slot_for(pid);
	if (!e) {
		return -EINVAL;
	}

	WRITE_ONCE(e->active, false);

	// Reinit stats
	WRITE_ONCE(e->pid, pid);
	WRITE_ONCE(e->latency_min_ns, LLONG_MAX);
	WRITE_ONCE(e->latency_max_ns, 0);
	WRITE_ONCE(e->latency_last_wakeup_ns, 0);
	WRITE_ONCE(e->curr_wu_ns, 0);
	WRITE_ONCE(e->rt_voluntary_min_ns, LLONG_MAX);
	WRITE_ONCE(e->rt_voluntary_max_ns, 0);
	WRITE_ONCE(e->scause, SC_NONE);
	WRITE_ONCE(e->last_sleep_cause, SC_NONE);
	WRITE_ONCE(e->cycle_start_ns, 0);
	WRITE_ONCE(e->cycle_active, false);
	WRITE_ONCE(e->rt_relief_max_ns, 0);
	WRITE_ONCE(e->rt_relief_min_ns, LLONG_MAX);
	WRITE_ONCE(e->last_handler_call_entry, 0);
	WRITE_ONCE(e->irq_handle_max_ns, 0);
	WRITE_ONCE(e->irq_handle_min_ns, LLONG_MAX);
	WRITE_ONCE(e->trace_enabled, traced);

        // Initialize spinlocks
        // and also use them when printing out stats
        raw_spin_lock_init(&e->lat_trace_lock);
        raw_spin_lock_init(&e->resp_trace_lock);
        raw_spin_lock_init(&e->resp_relief_trace_lock);
        raw_spin_lock_init(&e->irq_trace_lock);

	// SLO violation
	WRITE_ONCE(e->v.latency_bound, LLONG_MAX);
        atomic_set(&(e->v.latency_violations), 0);
	WRITE_ONCE(e->v.response_bound, LLONG_MAX);
        atomic_set(&(e->v.response_violations), 0);
	WRITE_ONCE(e->v.response_relief_bound, LLONG_MAX);
        atomic_set(&(e->v.response_relief_violations), 0);
	WRITE_ONCE(e->v.irq_handling_bound, LLONG_MAX);
        atomic_set(&(e->v.irq_handling_violations), 0);
	/* Latency */
	WRITE_ONCE(e->v.max_l_start_index, 0);
	WRITE_ONCE(e->v.max_l_trace_len, 0);
	/* Response (voluntary sleep) */
	WRITE_ONCE(e->v.max_r_start_index, 0);
	WRITE_ONCE(e->v.max_r_trace_len, 0);
	/* Response relief */
	WRITE_ONCE(e->v.max_rr_start_index, 0);
	WRITE_ONCE(e->v.max_rr_trace_len, 0);
	/* IRQ handling */
	WRITE_ONCE(e->v.max_i_start_index, 0);
	WRITE_ONCE(e->v.max_i_trace_len, 0);
	// TODO: make sure other variables in the recording and default structus are initialized correctly

	// Allocate if trace is enabled
	if (traced) {
		struct slo_event *latencyslo = kzalloc(MAX_TRACE_LEN_PER_SLO * sizeof(struct slo_event), GFP_KERNEL);
		struct slo_event *rtslo = kzalloc(MAX_TRACE_LEN_PER_SLO * sizeof(struct slo_event), GFP_KERNEL);
		struct slo_event *rt_reliefslo = kzalloc(MAX_TRACE_LEN_PER_SLO * sizeof(struct slo_event), GFP_KERNEL);
		struct slo_event *irqslo = kzalloc(MAX_TRACE_LEN_PER_SLO * sizeof(struct slo_event), GFP_KERNEL);
		WRITE_ONCE(e->v.max_l_violation_trace, latencyslo);
		WRITE_ONCE(e->v.max_rt_violation_trace, rtslo);
		WRITE_ONCE(e->v.max_rtr_violation_trace, rt_reliefslo);
		WRITE_ONCE(e->v.max_irqt_violation_trace, irqslo);
	} else {
                WRITE_ONCE(e->v.max_l_violation_trace, NULL);
		WRITE_ONCE(e->v.max_rt_violation_trace, NULL);
		WRITE_ONCE(e->v.max_rtr_violation_trace, NULL);
		WRITE_ONCE(e->v.max_irqt_violation_trace, NULL);
        }

	// Set PID's name
	struct task_struct *p = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (p) {
		get_task_comm(e->comm, p);
	} else {
		strncpy(e->comm, "<unknown>", TASK_COMM_LEN);
	}

	// Increment to show that this is not the original PID anymore.
	WRITE_ONCE(e->gen, READ_ONCE(e->gen) + 1);

	smp_store_release(&e->active, true);
	return 0;
}

/*
This should be called for every program on exit to clean up.
*/
static void stop_recording_pid(pid_t pid)
{
	struct task_latency_entry *e = slot_for(pid);
	if (!e) {
		return;
	}

	// to prevent double stop (e.g. rmmod and program exit happens at the same time)
	if (xchg(&e->active, false)) {
                unsigned int flags;

                // Take its pre trace lock to ensure that it finishes its previous trace updates
                raw_spin_lock_irqsave(&e->lat_trace_lock, flags);
                raw_spin_lock_irqsave(&e->resp_trace_lock, flags);
                raw_spin_lock_irqsave(&e->resp_relief_trace_lock, flags);
                raw_spin_lock_irqsave(&e->irq_trace_lock, flags);

		// print some base statistics
		// special case for IRQs
		bool is_irq = false;
		if (strncmp(e->comm, "irq/", 4) == 0) {
			is_irq = true;
			pr_info("IRQ %s (PID=%d):\n", e->comm, pid);
			pr_info("  service_time_min/max (wake->first sleep): %lld / %lld ns\n", e->rt_voluntary_min_ns,
				e->rt_voluntary_max_ns);
			pr_info("  latency_min/max (wake->first run): %lld / %lld ns\n", e->latency_min_ns, e->latency_max_ns);
			pr_info("  per interrupt bottom half handling time (single iteration): %lld / %lld ns\n",
				e->irq_handle_min_ns, e->irq_handle_max_ns);
		} else {
			pr_info("jerry_rt_module: PID=%d, Name=%s, minLat=%lli, maxLat=%lli,\nminResponseTimeVoluntarySleepAllTypes=%lli, maxResponseTimeVoluntarySleepAllTypes=%lli, \nminResponseTimeVoluntarySleepReliefBased=%lli, maxResponseTimeVoluntarySleepReliefBased=%lli",
				pid, e->comm, READ_ONCE(e->latency_min_ns), READ_ONCE(e->latency_max_ns),
				READ_ONCE(e->rt_voluntary_min_ns), READ_ONCE(e->rt_voluntary_max_ns),
				READ_ONCE(e->rt_relief_min_ns), READ_ONCE(e->rt_relief_max_ns));
		}

		struct task_stat_slo *v = &e->v;
		// then print out the violation traces
		if (v->latency_bound != LLONG_MAX) {
			pr_info("  LatencyBound=%lld, Violations=%lld\n", v->latency_bound, atomic_read(&(v->latency_violations)));
			print_single_trace(e, &e->v, e->v.max_l_violation_trace, e->v.max_l_trace_len, LATENCY);
		}
		if (v->response_bound != LLONG_MAX) {
			pr_info("  ResponseBound=%lld, Violations=%lld\n", v->response_bound, atomic_read(&(v->response_violations)));
			print_single_trace(e, &e->v, e->v.max_rt_violation_trace, e->v.max_r_trace_len, RESPONSE);
		}
		if (v->response_relief_bound != LLONG_MAX) {
			pr_info("  ResponseReliefBound=%lld, Violations=%lld\n", v->response_relief_bound, atomic_read(&(v->response_relief_violations)));
			print_single_trace(e, &e->v, e->v.max_rtr_violation_trace, e->v.max_rr_trace_len, RESPONSE_RELIEF);
		}
		if (v->irq_handling_bound != LLONG_MAX && is_irq) {
			pr_info("  IRQHandlingBound=%lld, Violations=%lld\n", v->irq_handling_bound, atomic_read(&(v->irq_handling_violations)));
			print_single_trace(e, &e->v, e->v.max_irqt_violation_trace, e->v.max_i_trace_len, IRQ_HANDLING);
		}

                raw_spin_unlock_irqrestore(&e->lat_trace_lock, flags);
                raw_spin_unlock_irqrestore(&e->resp_trace_lock, flags);
                raw_spin_unlock_irqrestore(&e->resp_relief_trace_lock, flags);
                raw_spin_unlock_irqrestore(&e->irq_trace_lock, flags);

                // Don't free here.
		// if (xchg(&e->trace_enabled, false)) {
		// 	void *latencyslo = READ_ONCE(e->v.max_l_violation_trace);
		// 	void *rtslo = READ_ONCE(e->v.max_rt_violation_trace);
		// 	void *rt_reliefslo = READ_ONCE(e->v.max_rtr_violation_trace);
		// 	void *irqtslo = READ_ONCE(e->v.max_irqt_violation_trace);

		// 	kfree(latencyslo);
		// 	kfree(rtslo);
		// 	kfree(rt_reliefslo);
		// 	kfree(irqtslo);
		// }
	}
}

static void print_single_trace(struct task_latency_entry *e, struct task_stat_slo *v, struct slo_event *printed_arr, int len,
			       enum stat_field event_type)
{
	if (e->trace_enabled) {
		pr_info("MAX TRACE LEN: %d\n", len);
		// If violation trace hasn't been allocated it could be bad accessing invalid memory
		// by definition if trace enabled then, violation trace is allocated
		u64 base = 0;
		if (event_type == LATENCY || event_type == RESPONSE || event_type == RESPONSE_RELIEF) {
			base = printed_arr[0].event.wakeup_info.time;
		} else {
			base = printed_arr[0].event.irqt_entry_info.time;
		}
		switch (event_type) {
		case LATENCY:
			pr_info("WORST LATENCY TRACE: \n");
			break;
		case RESPONSE:
			pr_info("WORST RESPONSE TIME TRACE: \n");
			break;
		case RESPONSE_RELIEF:
			pr_info("WORST RESPONSE RELIEF TIME TRACE: \n");
			break;
		case IRQ_HANDLING:
			pr_info("WORST IRQ HANDLING TIME TRACE: \n");
			break;
		default:
			break;
		}

		for (int i = 0; i < len; i++) {
			struct slo_event event = printed_arr[i];
			if (event.type == SCHED_SWITCH) {
				u64 rel_us = div_u64(event.event.switch_info.time - base, 1000);
				pr_info("[%6llu µs] Event: sched_switch, preemption: %d, voluntary: %d, prev_pid: %d (priority: %d), next_pid: %d (priority: %d), on_cpu: %d, \n",
					rel_us, event.event.switch_info.preempt,
					// TODO: Do better decoding for this prev state thing, (there are some random number that shows up e.g. 1026 don't know what this shit means)
					event.event.switch_info.prev_state &
						(TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE | TASK_PARKED | TASK_IDLE | TASK_DEAD),
					event.event.switch_info.prev_pid, event.event.switch_info.prev_prio,
					event.event.switch_info.next_pid, event.event.switch_info.next_prio,
					event.event.switch_info.event_cpu);
			} else if (event.type == SCHED_WAKEUP) {
				u64 rel_us = div_u64(event.event.wakeup_info.time - base, 1000);
				pr_info("[%6llu µs] Event: sched_wakeup, pid: %d, wake_cpu: %d\n", rel_us,
					event.event.wakeup_info.pid, event.event.wakeup_info.wake_cpu);
			} else if (event.type == SYS_SLEEP) {
				u64 rel_us = div_u64(event.event.sleep_info.time - base, 1000);
				pr_info("[%6llu µs] Event: sys_sleep, pid: %d\n", rel_us, event.event.sleep_info.pid);
			} else if (event.type == IRQT_ENTRY) {
				u64 rel_us = div_u64(event.event.irqt_entry_info.time - base, 1000);
				pr_info("[%6llu µs] Event: irqt_entry, pid: %d\n", rel_us, event.event.irqt_entry_info.pid);
			} else if (event.type == IRQT_EXIT) {
				u64 rel_us = div_u64(event.event.irqt_exit_info.time - base, 1000);
				pr_info("[%6llu µs] Event: irqt_exit, pid: %d\n", rel_us, event.event.irqt_exit_info.pid);
			}
		}
	}
}

/*
Function that would be called whn sched_switched happens

Task is running 
*/
static void probe_sched_switch(void *data, bool preempt, struct task_struct *prev, struct task_struct *next,
			       unsigned int prev_state)
{
        u64 t0 = ktime_get_ns();

        atomic64_inc(&total_probe_hits);

	// Record sched_switch event on ring
	struct slo_event switch_event = { 0 };
	switch_event.type = SCHED_SWITCH;
	switch_event.event.switch_info.prev_pid = prev->pid;
	switch_event.event.switch_info.next_pid = next->pid;
	switch_event.event.switch_info.preempt = preempt;
	switch_event.event.switch_info.prev_state = prev_state;
	switch_event.event.switch_info.event_cpu = smp_processor_id();
	switch_event.event.switch_info.prev_prio = prev->prio;
	switch_event.event.switch_info.next_prio = next->prio;
	switch_event.event.switch_info.time = ktime_get_ns();
	int switch_index = slo_queue_push(switch_event);

	// Track latency statistics for the next task switched into
	struct task_latency_entry *e = slot_for(next->pid);
	s64 wu, now;

	if (e && smp_load_acquire(&e->active)) {
		wu = xchg(&e->latency_last_wakeup_ns, 0);
		if (wu != 0) {
			now = ktime_get_ns();
			s64 delta = now - wu;
			update_minmax((enum stat_field)LATENCY, e, delta, switch_index);
			update_violation((enum stat_field)LATENCY, delta, e);

			// Write down the last wake up time used (this is for response time)
			WRITE_ONCE(e->curr_wu_ns, wu);
		}
	}

	struct task_latency_entry *p = slot_for(prev->pid);
	if (p && smp_load_acquire(&p->active)) {
		// Indicates a voluntary exit
		bool voluntary = prev_state & (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE | TASK_PARKED | TASK_IDLE | TASK_DEAD);

		if (voluntary) {
			s64 now = ktime_get_ns();
			enum sleep_cause vol_sleep_cause = READ_ONCE(p->scause);
			if (vol_sleep_cause == SC_NONE) {
				vol_sleep_cause = SC_OTHER;
			}
			WRITE_ONCE(p->last_sleep_cause, vol_sleep_cause);
			WRITE_ONCE(p->scause, SC_NONE);

			s64 wu = xchg(&p->curr_wu_ns, 0);

			// Record the the voluntary sleep (any type)
			if (wu) {
				s64 resp = now - wu;
				update_minmax((enum stat_field)RESPONSE, p, resp, switch_index);
				update_violation((enum stat_field)RESPONSE, resp, p);
			}

			if (vol_sleep_cause == SC_TIMER && READ_ONCE(p->cycle_active)) {
				s64 cs = xchg(&p->cycle_start_ns, 0);
				if (cs) {
					s64 cycle = now - cs;
					update_minmax((enum stat_field)RESPONSE_RELIEF, p, cycle, switch_index);
					update_violation((enum stat_field)RESPONSE_RELIEF, cycle, p);
				}
				WRITE_ONCE(p->cycle_active, false);
			}
		}
	}
        u64 dt = ktime_get_ns() - t0;
        atomic64_add(dt, &total_time_ns);

        u64 old = atomic64_read(&max_time_ns);
        while (dt > old) {
                u64 prev = atomic64_cmpxchg(&max_time_ns, old, dt);
                if (prev == old) break;
                old = prev;
        }
}

/*
Function that would be called when sched_wakeup happens.
*/
static void probe_sched_wakeup(void *data, struct task_struct *p)
{
        u64 t0 = ktime_get_ns();
        atomic64_inc(&total_probe_hits);

	// Record event into ring buffer
	struct slo_event wakeup_event = { 0 };
	wakeup_event.type = SCHED_WAKEUP;
	wakeup_event.event.wakeup_info.prio = p->prio;
	wakeup_event.event.wakeup_info.recent_used_cpu = p->recent_used_cpu;
	wakeup_event.event.wakeup_info.wake_cpu = smp_processor_id();
	wakeup_event.event.wakeup_info.pid = p->pid;
	wakeup_event.event.wakeup_info.time = ktime_get_ns();
	int store_index = slo_queue_push(wakeup_event);

	struct task_latency_entry *e = slot_for(p->pid);
	if (!e) {
		return;
	}

	if (smp_load_acquire(&e->active) == false) {
		return;
	}
	if (READ_ONCE(e->pid) != p->pid) {
		return;
	}

	s64 now = ktime_get_ns();
	WRITE_ONCE(e->latency_last_wakeup_ns, now);
	// index that indicates wake up to switch in.
	if (READ_ONCE(e->trace_enabled) == true) {
		// pr_info("Are we putting down a new start index??\n");
		WRITE_ONCE(e->v.max_l_start_index, store_index);
		WRITE_ONCE(e->v.max_r_start_index, store_index);
	}

	if (READ_ONCE(e->last_sleep_cause) == SC_TIMER) {
		WRITE_ONCE(e->cycle_active, true);
		WRITE_ONCE(e->cycle_start_ns, now);

		// index that indicates from wakeup (until relief sleep)
		if (READ_ONCE(e->trace_enabled)) {
			WRITE_ONCE(e->v.max_rr_start_index, store_index);
		}
	}
        
        u64 dt = ktime_get_ns() - t0;
        atomic64_add(dt, &total_time_ns);

        u64 old = atomic64_read(&max_time_ns);
        while (dt > old) {
                u64 prev = atomic64_cmpxchg(&max_time_ns, old, dt);
                if (prev == old) break;
                old = prev;
        }
}

static __always_inline bool is_timer_sleep_syscall(long id)
{
#ifdef __NR_nanosleep
	if (id == __NR_nanosleep)
		return true;
#endif
#ifdef __NR_nanosleep_time64
	if (id == __NR_nanosleep_time64)
		return true;
#endif
#ifdef __NR_clock_nanosleep
	if (id == __NR_clock_nanosleep)
		return true;
#endif
#ifdef __NR_clock_nanosleep_time64
	if (id == __NR_clock_nanosleep_time64)
		return true;
#endif
	return false;
}

/*
Handler for all syscalls. However only checking for if its a sleep syscall here.
The "sys enter" handler will only handle sleeps
*/
static void probe_sys_enter(void *data, struct pt_regs *regs, long id)
{
        u64 t0 = ktime_get_ns();
        atomic64_inc(&total_probe_hits);

	if (!is_timer_sleep_syscall(id)) {
		return;
	}
	struct task_latency_entry *e = slot_for(current->pid);
	if (!e || smp_load_acquire(&e->active) == false) {
		return;
	}

	// Record this as an event onto ring buffer
	struct slo_event sleep_event = { 0 };
	sleep_event.type = SYS_SLEEP;
	sleep_event.event.sleep_info.pid = e->pid;
	sleep_event.event.sleep_info.time = ktime_get_ns();
	slo_queue_push(sleep_event);

	WRITE_ONCE(e->scause, SC_TIMER);

        u64 dt = ktime_get_ns() - t0;
        atomic64_add(dt, &total_time_ns);

        u64 old = atomic64_read(&max_time_ns);
        while (dt > old) {
                u64 prev = atomic64_cmpxchg(&max_time_ns, old, dt);
                if (prev == old) break;
                old = prev;
        }
}

/*
Handler for IRQ tracepoints.
*/
static void probe_irq_threaded_handler_entry(void *data, int irq, struct irqaction *action)
{
	if (action != NULL) {
		struct slo_event entry_event = { 0 };
		entry_event.type = IRQT_ENTRY;
		entry_event.event.irqt_entry_info.pid = action->thread->pid;
		entry_event.event.irqt_entry_info.time = ktime_get_ns();
		int entry_index = slo_queue_push(entry_event);

		struct task_struct *irq_thread = action->thread;
		pid_t pid = irq_thread->pid;
		struct task_latency_entry *e = slot_for(pid);
		if (READ_ONCE(e->trace_enabled)) {
			// TODO might need a lock here
			WRITE_ONCE(e->v.max_i_start_index, entry_index);
		}

		if (e && smp_load_acquire(&e->active)) {
			WRITE_ONCE(e->last_handler_call_entry, ktime_get_ns());
		}
	}
}

static void probe_irq_threaded_handler_exit(void *data, int irq, struct irqaction *action)
{
	if (action != NULL) {
		struct slo_event exit_event = { 0 };
		exit_event.type = IRQT_EXIT;
		exit_event.event.irqt_exit_info.pid = action->thread->pid;
		exit_event.event.irqt_exit_info.time = ktime_get_ns();
		int switch_index = slo_queue_push(exit_event);

		struct task_struct *irq_thread = action->thread;
		if (irq_thread == 0) {
			return;
		}
		pid_t pid = irq_thread->pid;
		struct task_latency_entry *e = slot_for(pid);

		if (e && smp_load_acquire(&e->active)) {
			s64 le = xchg(&e->last_handler_call_entry, 0);
			if (le != 0) {
				s64 delta = ktime_get_ns() - le;
				update_minmax((enum stat_field)IRQ_HANDLING, e, delta, switch_index);
				update_violation((enum stat_field)IRQ_HANDLING, delta, e);
			}
		}
	}
}

/*
Handler for process natural exit
*/
static void probe_sched_process_exit(void *data, struct task_struct *p)
{
        u64 t0 = ktime_get_ns();
        atomic64_inc(&total_probe_hits);
	pid_t pid = p->pid;
	struct task_latency_entry *e = slot_for(pid);

	if (!e || !smp_load_acquire(&e->active)) {
		return;
	}
	stop_recording_pid(pid);

        u64 dt = ktime_get_ns() - t0;
        atomic64_add(dt, &total_time_ns);

        u64 old = atomic64_read(&max_time_ns);
        while (dt > old) {
                u64 prev = atomic64_cmpxchg(&max_time_ns, old, dt);
                if (prev == old) break;
                old = prev;
        }
}

/**
 * Function to update violation status for a particular field
 * @field is the specific metric that this value is applicable for
 * 
 * violation_event_index points to the event that completed this SLO. 
 * 
 */
static void update_violation(enum stat_field field, s64 value, struct task_latency_entry *e)
{
	s64 bound;
	switch (field) {
	case LATENCY:
		/**
                 * This value update is intended for latency
                 */
		bound = READ_ONCE(e->v.latency_bound);
		if (value > bound) {
			// WRITE_ONCE(e->v.latency_violations, READ_ONCE(e->v.latency_violations) + 1);
                        atomic_inc(&(e->v.latency_violations));
		}
		break;
	case RESPONSE:
		bound = READ_ONCE(e->v.response_bound);
		if (value > bound) {
			// WRITE_ONCE(e->v.response_violations, READ_ONCE(e->v.response_violations) + 1);
                        atomic_inc(&(e->v.response_violations));
		}
		break;
	case RESPONSE_RELIEF:
		bound = READ_ONCE(e->v.response_relief_bound);
		if (value > bound) {
			// WRITE_ONCE(e->v.response_relief_violations, READ_ONCE(e->v.response_relief_violations) + 1);
                        atomic_inc(&(e->v.response_relief_violations));
		}
		break;
	case IRQ_HANDLING:
		bound = READ_ONCE(e->v.irq_handling_bound);
		if (value > bound) {
			// WRITE_ONCE(e->v.irq_handling_violations, READ_ONCE(e->v.irq_handling_violations) + 1);
                        atomic_inc(&(e->v.irq_handling_violations));
		}
		break;
	default:
		break;
	}
}

/**
 * Checks if the ring still contains full trace
 * If not: then return
 * If yes: then collect and put it into the buffer
 * 
 * violation_event_index is the index which completed this SLO.
 * 
 * PRECONDITION: pid is an active element 
 */
int collect_latency_violation_trace(struct task_latency_entry *entry, int violation_event_index)
{
	// Restrict max_l_start_index to only be consumed once
	// TODO: implement this only consume once on other trace functions
	int start_index = xchg(&entry->v.max_l_start_index, START_INVALID);
	if (start_index == START_INVALID) {
		return -1;
	}

	return collect_slo_trace_common(entry, start_index, violation_event_index, entry->v.max_l_violation_trace,
					&entry->v.max_l_trace_len, (enum slo_trace_mode)TRACE_LATENCY);
}

int collect_response_violation_trace(struct task_latency_entry *entry, int violation_event_index)
{
        int start_index = xchg(&entry->v.max_r_start_index, START_INVALID);
        if (start_index == START_INVALID) {
                return -1;
        }

	return collect_slo_trace_common(entry, start_index, violation_event_index,
					entry->v.max_rt_violation_trace, &entry->v.max_r_trace_len,
					(enum slo_trace_mode)TRACE_RESP_ALL);
}

int collect_response_relief_violation_trace(struct task_latency_entry *entry, int violation_event_index)
{
        int start_index = xchg(&entry->v.max_rr_start_index, START_INVALID);
        if (start_index == START_INVALID) {
                return -1;
        }

	return collect_slo_trace_common(entry, start_index, violation_event_index,
					entry->v.max_rtr_violation_trace, &entry->v.max_rr_trace_len, TRACE_RESP_RELIEF);
}

int collect_irqt_violation_trace(struct task_latency_entry *entry, int violation_event_index)
{
        int start_index = xchg(&entry->v.max_i_start_index, START_INVALID);
        if (start_index == START_INVALID) {
                return -1;
        }

	return collect_slo_trace_common(entry, start_index, violation_event_index,
					entry->v.max_irqt_violation_trace, &entry->v.max_i_trace_len, TRACE_IRQT);
}

static int collect_slo_trace_common(struct task_latency_entry *e, int start_index, int violation_event_index, struct slo_event *dst,
				    int *dst_len, enum slo_trace_mode mode)
{
	unsigned int flags;
	int len = 0;
	if (!READ_ONCE(e->trace_enabled)) {
		return 0;
	}
	if (!dst || !dst_len) {
		return -EINVAL;
	}
	spin_lock_irqsave(&event_queue.lock, flags);

	u64 tail = event_queue.tail_seq;
	if (tail > start_index) {
		spin_unlock_irqrestore(&event_queue.lock, flags);
		return -1;
	}

	for (int i = start_index; i <= violation_event_index && len < MAX_TRACE_LEN_PER_SLO; i++) {
		struct slo_event *se = &event_queue.buf[i % QUEUE_LEN];

		if (should_include_event(mode, se, e->pid, i, start_index)) {
			dst[len] = *se;
			len = len + 1;
		}
	}
	spin_unlock_irqrestore(&event_queue.lock, flags);
	*dst_len = len;

	return 0;
}

static bool should_include_event(enum slo_trace_mode mode, struct slo_event *se, pid_t task_pid, int len_so_far, int start_index)
{
	switch (mode) {
	case TRACE_LATENCY:
		return true;
	case TRACE_RESP_ALL:
	case TRACE_RESP_RELIEF:
		if (len_so_far == start_index) {
			return se->type == SCHED_WAKEUP;
		}
		if (len_so_far > start_index) {
			if (se->type == SCHED_SWITCH &&
			    (se->event.switch_info.prev_pid == task_pid || se->event.switch_info.next_pid == task_pid)) {
				return true;
			}
			if (se->type == SYS_SLEEP && se->event.sleep_info.pid == task_pid) {
				return true;
			}
		}
		return false;
	case TRACE_IRQT:
		if (len_so_far == start_index) {
			return se->type == IRQT_ENTRY;
		}
		if (len_so_far > start_index) {
			if (se->type == SCHED_SWITCH &&
			    (se->event.switch_info.prev_pid == task_pid || se->event.switch_info.next_pid == task_pid)) {
				return true;
			}
			if (se->type == SYS_SLEEP && se->event.sleep_info.pid == task_pid) {
				return true;
			}
			if (se->type == IRQT_EXIT && se->event.irqt_exit_info.pid == task_pid) {
				return true;
			}
		}
		return false;
	default:
		break;
	}
	return false;
}

/*
Used to find the correct tracepoint that we want to hook onto
*/
static void find_tp(struct tracepoint *tp, void *priv)
{
	if (strcmp(tp->name, "sched_switch") == 0) {
		tp_sched_switch = tp;
	} else if (strcmp(tp->name, "sched_wakeup") == 0) {
		tp_sched_wakeup = tp;
	} else if (strcmp(tp->name, "sys_enter") == 0) {
		tp_sys_enter = tp;
	} else if (strcmp(tp->name, "irq_threaded_handler_entry") == 0) {
		tp_irq_threaded_handler_entry = tp;
	} else if (strcmp(tp->name, "irq_threaded_handler_exit") == 0) {
		tp_irq_threaded_handler_exit = tp;
	} else if (strcmp(tp->name, "sched_process_exit") == 0) {
		tp_sched_process_exit = tp;
	}
}

/*
Kernel sysfs input handling

Check here if we need to trace for this pid.
echo "3452 trace" > /sys/kernel/jerry_rt_module/add_pid

Remove pid is still the same, but now we need to free the arr in remove, and also rmmod.
*/
static ssize_t add_pid_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int pid;
	char cmd[16] = { 0 };

	int n = sscanf(buf, "%d %15s", &pid, cmd);

	if (n == 1) // No trace
	{
		start_recording_pid(pid, false);
	} else if (n == 2) // Yes trace
	{
		if (strcmp(cmd, "trace") == 0) {
			start_recording_pid(pid, true);
		} else {
			pr_warn("Unknown command: %s\n", cmd);
		}
	} else {
		pr_warn("Bad format: expected 'pid' or 'pid trace'\n");
	}

	return count;
}

static ssize_t del_pid_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	pid_t pid;
	if (kstrtoint(buf, 10, &pid) == 0) {
		stop_recording_pid(pid);
	}
	return count;
}

DEFINE_SLO_SETTER(latency_bound, latency_bound);
DEFINE_SLO_SETTER(response_bound, response_bound);
DEFINE_SLO_SETTER(response_relief_bound, response_relief_bound);
DEFINE_SLO_SETTER(irq_handling_bound, irq_handling_bound);

/**
 * Helper function parse for two integers
 */
static int parse_two_ints(const char *buf, size_t count, int *first, int *second)
{
	char tmp[64];
	char *space;
	int ret;
	size_t len;

	/* Clamp and copy, add '\0' */
	len = min(count, sizeof(tmp) - 1);
	memcpy(tmp, buf, len);
	tmp[len] = '\0';

	/* Find the space separator */
	space = strchr(tmp, ' ');
	if (!space) {
		return -EINVAL;
	}

	*space = '\0'; /* Split into two C strings */

	ret = kstrtoint(tmp, 10, first);
	if (ret)
		return ret;

	ret = kstrtoint(space + 1, 10, second);
	if (ret) {
		return ret;
	}

	return 0;
}

/**
 * kobj is just the sysfs object the attr is under
 * attr is the struct that contains the file name, permissions and pointers to store show functions
 * buf is the raw buffer of characters userspace wrote into the sysfs file
 * count is number of bytes written.
 * 
 * We expect echo "A B", where pid = A, bound = B 
 * This function would tell us the particular bound it is for.
 */
static ssize_t set_slo_latency_bound_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	pid_t pid;
	int bound;

	parse_two_ints(buf, count, &pid, &bound);
	set_latency_bound(pid, bound);

	return count;
}

static ssize_t set_slo_response_bound_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	pid_t pid;
	int bound;

	parse_two_ints(buf, count, &pid, &bound);
	set_response_bound(pid, bound);

	return count;
}

static ssize_t set_slo_response_relief_bound_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	pid_t pid;
	int bound;

	parse_two_ints(buf, count, &pid, &bound);
	set_response_relief_bound(pid, bound);

	return count;
}

static ssize_t set_slo_irq_handling_bound_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	pid_t pid;
	int bound;

	parse_two_ints(buf, count, &pid, &bound);
	set_irq_handling_bound(pid, bound);

	return count;
}

/**
 * _ATTR_WO macro expands into something like .attr = something, .store=add_pid_store, .show=add_pid_show. Since we register the attr, when that attr is written to, we'd call the function registered with store
 */
static struct kobj_attribute add_pid_attr = __ATTR_WO(add_pid);
static struct kobj_attribute del_pid_attr = __ATTR_WO(del_pid);
static struct kobj_attribute set_slo_latency_bound_attr = __ATTR_WO(set_slo_latency_bound);
static struct kobj_attribute set_slo_response_bound_attr = __ATTR_WO(set_slo_response_bound);
static struct kobj_attribute set_slo_response_relief_bound_attr = __ATTR_WO(set_slo_response_relief_bound);
static struct kobj_attribute set_slo_irq_handling_bound_attr = __ATTR_WO(set_slo_irq_handling_bound);

static struct attribute *rt_attrs[] = {
	&add_pid_attr.attr,
	&del_pid_attr.attr,
	&set_slo_latency_bound_attr.attr,
	&set_slo_response_bound_attr.attr,
	&set_slo_response_relief_bound_attr.attr,
	&set_slo_irq_handling_bound_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rt);

static struct kobject *rt_kobj;

/*
init: initializes tracepoints and links handler functions to those tracepoints
*/
static int __init rt_module_init(void)
{
	// Allocate GFP_KERNEL in initialization
	pidtab = vzalloc(sizeof(struct task_latency_entry) * PIDTAB_SIZE);
	if (!pidtab) {
		pr_err("jerry_rt_module: failed to allocate pidtab\n");
		return -ENOMEM;
	}

	pr_info("jerry_rt_module: looking for tracepoints...\n");

	// kernel macro that just calls find_tp with each tracepoint it knows
	for_each_kernel_tracepoint(find_tp, NULL);

	if (!tp_sched_switch) {
		pr_err("jerry_rt_module: We don't have sched_switch. ftrace not loaded?");
		vfree(pidtab);
		return -ENOENT;
	}
	if (!tp_sched_wakeup) {
		pr_err("jerry_rt_module: We don't have sched_wakeup. ftrace not loaded?");
		vfree(pidtab);
		return -ENOENT;
	}
	if (!tp_sys_enter) {
		pr_err("jerry_rt_module: We don't have sys_enter tracepoint\n");
		vfree(pidtab);
		return -ENOENT;
	}
	if (!tp_irq_threaded_handler_entry) {
		pr_err("jerry_rt_module: We don't have irq_threaded_handler_entry tracepoint\n");
		vfree(pidtab);
		return -ENOENT;
	}
	if (!tp_irq_threaded_handler_exit) {
		pr_err("jerry_rt_module: We don't have irq_threaded_handler_exit tracepoint\n");
		vfree(pidtab);
		return -ENOENT;
	}
	if (!tp_sched_process_exit) {
		pr_err("jerry_rt_module: We don't have sched_process_exit tracepoint\n");
		vfree(pidtab);
		return -ENOENT;
	}

	pr_info("jerry_rt_module: Found sched_switch and sched_wakeup trace point");
	pr_info("jerry_rt_module: Registering the probe function for each event");

	if (tracepoint_probe_register(tp_sched_switch, probe_sched_switch, NULL)) {
		pr_err("jerry_rt_module: Failed to register probe\n");
		return -EINVAL;
	}

	if (tracepoint_probe_register(tp_sched_wakeup, probe_sched_wakeup, NULL)) {
		pr_err("jerry_rt_module: Failed to register probe for sched_wakeup\n");
		tracepoint_probe_unregister(tp_sched_switch, probe_sched_switch, NULL);
		vfree(pidtab);
		return -EINVAL;
	}

	if (tracepoint_probe_register(tp_sys_enter, probe_sys_enter, NULL)) {
		pr_err("jerry_rt_module: Failed to register sys_enter probe\n");
		tracepoint_probe_unregister(tp_sched_switch, probe_sched_switch, NULL);
		tracepoint_probe_unregister(tp_sched_wakeup, probe_sched_wakeup, NULL);
		vfree(pidtab);
		return -EINVAL;
	}

	if (tracepoint_probe_register(tp_irq_threaded_handler_entry, probe_irq_threaded_handler_entry, NULL)) {
		pr_err("jerry_rt_module: Failed to register irq_threaded_handler_entry probe\n");
		tracepoint_probe_unregister(tp_sched_switch, probe_sched_switch, NULL);
		tracepoint_probe_unregister(tp_sched_wakeup, probe_sched_wakeup, NULL);
		tracepoint_probe_unregister(tp_sys_enter, probe_sys_enter, NULL);
		vfree(pidtab);
		return -EINVAL;
	}

	if (tracepoint_probe_register(tp_irq_threaded_handler_exit, probe_irq_threaded_handler_exit, NULL)) {
		pr_err("jerry_rt_module: Failed to register irq_threaded_handler_entry probe\n");
		tracepoint_probe_unregister(tp_sched_switch, probe_sched_switch, NULL);
		tracepoint_probe_unregister(tp_sched_wakeup, probe_sched_wakeup, NULL);
		tracepoint_probe_unregister(tp_sys_enter, probe_sys_enter, NULL);
		tracepoint_probe_unregister(tp_irq_threaded_handler_entry, probe_irq_threaded_handler_entry, NULL);
		vfree(pidtab);
		return -EINVAL;
	}

	if (tracepoint_probe_register(tp_sched_process_exit, probe_sched_process_exit, NULL)) {
		pr_err("jerry_rt_module: Failed to register sched_process_exit probe\n");
		tracepoint_probe_unregister(tp_sched_switch, probe_sched_switch, NULL);
		tracepoint_probe_unregister(tp_sched_wakeup, probe_sched_wakeup, NULL);
		tracepoint_probe_unregister(tp_sys_enter, probe_sys_enter, NULL);
		tracepoint_probe_unregister(tp_irq_threaded_handler_entry, probe_irq_threaded_handler_entry, NULL);
		tracepoint_probe_unregister(tp_irq_threaded_handler_exit, probe_irq_threaded_handler_exit, NULL);
		vfree(pidtab);
		return -EINVAL;
	}

	// Creating the SLO queue
	int ret = slo_queue_init();
	if (ret) {
		pr_info("Failed to initialize slo tracing queue");
		return -1;
	}
	pr_info("Slo tracing queue loaded");

	// Registering the attribute group
	rt_kobj = kobject_create_and_add("jerry_rt_module", kernel_kobj);
	if (!rt_kobj) {
		return -ENOMEM;
	}
	sysfs_create_groups(rt_kobj, rt_groups);

	pr_info("jerry_rt_module: Success! sched_switch and sched_wakeup are both registered");
	return 0;
}

static void __exit rt_module_exit(void)
{
	// Unregister the schedule
	if (tp_sched_switch) {
		// tell the thing to stop calling our function
		tracepoint_probe_unregister(tp_sched_switch, probe_sched_switch, NULL);
	}
	if (tp_sched_wakeup) {
		tracepoint_probe_unregister(tp_sched_wakeup, probe_sched_wakeup, NULL);
	}
	if (tp_sys_enter) {
		tracepoint_probe_unregister(tp_sys_enter, probe_sys_enter, NULL);
	}
	if (tp_irq_threaded_handler_entry) {
		tracepoint_probe_unregister(tp_irq_threaded_handler_entry, probe_irq_threaded_handler_entry, NULL);
	}
	if (tp_irq_threaded_handler_exit) {
		tracepoint_probe_unregister(tp_irq_threaded_handler_exit, probe_irq_threaded_handler_exit, NULL);
	}
	if (tp_sched_process_exit) {
		tracepoint_probe_unregister(tp_sched_process_exit, probe_sched_process_exit, NULL);
	}

	// Wait so that probe function is not still running on other cores
	tracepoint_synchronize_unregister();

        // Print Evaluation statistics
        pr_info("probe hits=%lld total_time=%lld ns avg=%lld ns max=%lld ns\n",
                atomic64_read(&total_probe_hits),
                atomic64_read(&total_time_ns),
                div64_u64(atomic64_read(&total_time_ns),
                        atomic64_read(&total_probe_hits)),
                atomic64_read(&max_time_ns)
        );

        // Ensures that last potential trace update has happened.

	if (pidtab) {
		for (int i = 0; i < PIDTAB_SIZE; i++) {
			struct task_latency_entry *curr = slot_for(i);
			if (xchg(&curr->active, false)) {
				stop_recording_pid(i);

                                void *latencyslo = READ_ONCE(curr->v.max_l_violation_trace);
                                void *rtslo = READ_ONCE(curr->v.max_rt_violation_trace);
                                void *rt_reliefslo = READ_ONCE(curr->v.max_rtr_violation_trace);
                                void *irqtslo = READ_ONCE(curr->v.max_irqt_violation_trace);
                                // If they are allocated then they are not null. 
                                if (latencyslo) {
                                        kfree(latencyslo);
                                }
                                if (rtslo) {
                                        kfree(rtslo);
                                }
                                if (rt_reliefslo) {
                                        kfree(rt_reliefslo);
                                }
                                if (irqtslo) {
                                        kfree(irqtslo);
                                }
			}
		}
		vfree(pidtab);
	}

	slo_queue_exit();

	sysfs_remove_groups(rt_kobj, rt_groups);
	kobject_put(rt_kobj);
}

module_init(rt_module_init);
module_exit(rt_module_exit);
