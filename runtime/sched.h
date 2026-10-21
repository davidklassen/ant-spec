/* The scheduler's private interface for the sync objects: every condition
 * variable in the runtime waits on the GIL's own mutex (design section 3),
 * so a goroutine that tested its predicate under the GIL cannot miss the
 * wake-up that follows a change to it. Included by sched.c and sync.c. */
#ifndef ANT_SCHED_H
#define ANT_SCHED_H

#include <pthread.h>

/* Gives the GIL away and parks the calling goroutine on cv; returns once
 * it has been woken and holds the GIL again. Callers loop on their
 * predicate: a wake-up may be spurious or another goroutine may have
 * consumed the state change first. */
void ant_block_on(pthread_cond_t *cv);
void ant_cond_init(pthread_cond_t *cv);
void ant_cond_signal(pthread_cond_t *cv);
void ant_cond_broadcast(pthread_cond_t *cv);

#endif
