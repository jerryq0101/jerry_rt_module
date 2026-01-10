#ifndef SLO_EVENTS_H
#define SLO_EVENTS_H

#include <linux/kfifo.h>
#include <linux/sched.h>
#include <linux/types.h>

// Global ring buffer
#define QUEUE_LEN 16384

// Understand what prev_state is
struct sched_switch_info {
        pid_t prev_pid;
        pid_t next_pid;
        bool preempt;
        unsigned int prev_state;
        int event_cpu;

        // Priorities
        int prev_prio;
        int next_prio;

        // Time
        s64 time;
};

struct sched_wakeup_info {
        pid_t pid;
        int wake_cpu;
        int recent_used_cpu;

        // General knowledge about the Task
        int prio;

        // Time
        s64 time;
};

struct sys_sleep_info {
        // TODO: this name could be replaced with a pid
        pid_t pid;
        
        // Time
        s64 time;
};

struct irqt_entry_info {
        pid_t pid;
        
        // Time
        s64 time;
};

struct irqt_exit_info {
        pid_t pid;

        // Time
        s64 time
};

enum event_type {
        SCHED_SWITCH,
        SCHED_WAKEUP,
        SYS_SLEEP,
        IRQT_ENTRY,
        IRQT_EXIT,
};

struct slo_event {
        enum event_type type;
        union {
                struct sched_switch_info switch_info;
                struct sched_wakeup_info wakeup_info;
                struct sys_sleep_info sleep_info;
                struct irqt_entry_info irqt_entry_info;
                struct irqt_exit_info irqt_exit_info;
        } event;
};

struct event_ring {
        struct slo_event buf[QUEUE_LEN];
        u64 head_seq;
        u64 tail_seq;
        // ring wraps, tail will indicate the oldest event before the head.
        spinlock_t lock;
};

/*
Functions that are needed 
*/
int slo_queue_init(void);
void slo_queue_exit(void);

int slo_queue_push(struct slo_event ev);

extern struct event_ring event_queue;

#endif
