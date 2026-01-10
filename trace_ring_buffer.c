#include <linux/spinlock.h>
#include <linux/slab.h>
#include "trace_ring_buffer.h"

struct event_ring event_queue;                          // static memory

int slo_queue_init(void)
{
        spin_lock_init(&event_queue.lock);
        event_queue.head_seq = 1;
        event_queue.tail_seq = 0;
        // buf is initialized to 0s, as event_queue is in static memory
        return 0;
}

/**
 * In case I do some heap allocation
 */
void slo_queue_exit(void)
{

}

/**
 * Pushes a slo event into the queue, and returns the index of push.
 */
// pass in slo_event by value (scoping will free this, and it will free the caller's as well, so only the event queue one remains)
int slo_queue_push(struct slo_event ev)
{
        unsigned long flags;
        unsigned int copied;

        spin_lock_irqsave(&event_queue.lock, flags);
        // Storing
        u64 head = event_queue.head_seq;
        // store a copy
        event_queue.buf[head % QUEUE_LEN] = ev;
        event_queue.head_seq = head + 1;

        // Updating tail (if head caught up to tail)
        u64 tail = event_queue.tail_seq;
        if (head == tail) {
                event_queue.tail_seq = tail + 1;
        }
        spin_unlock_irqrestore(&event_queue.lock, flags);

        return head;
}

