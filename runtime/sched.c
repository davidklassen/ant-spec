/* Goroutines (docs/runtime-design.md section 3): one OS thread each,
 * serialized by a global interpreter lock, the GIL, that a thread must
 * hold to run Ant code or touch an AntValue. The holder gives the GIL up
 * at a safepoint when asked to, when it blocks on a sync object, when it
 * sleeps, and when it ends. The hand-off is direct and FIFO: the releaser
 * dequeues the next holder and signals its condition variable, so a
 * running goroutine never re-acquires ahead of one that waits and a
 * spinner cannot starve `main`. There is no ticker thread: the waiter at
 * the head of the queue, after a slice of GIL_INTERVAL_NS without becoming
 * the holder, asks the holder to switch through ant_switch_requested,
 * which ANT_SAFEPOINT reads.
 *
 * gil.m protects the scheduler's own fields and the condition variables
 * of the sync objects. It is never held while running Ant code or
 * allocating, so it never nests with the collector's lock. */
#include <errno.h>
#include <string.h>
#include <time.h>
#include "ant.h"
#include "sched.h"

typedef struct GoThread GoThread;
struct GoThread {
  AntGoroutine g;            /* first: ant_g points into the holder's GoThread */
  pthread_cond_t cv;         /* signalled when this thread becomes the holder */
  GoThread *qnext;           /* the GIL queue */
  AntValue closure;          /* the `go` block, kept reachable for the collector */
};

enum { GIL_INTERVAL_NS = 5 * 1000 * 1000, GO_STACK = 64 << 20 };

static GoThread main_thread = { .cv = PTHREAD_COND_INITIALIZER };

static struct {
  pthread_mutex_t m;
  GoThread *holder;          /* NULL while nobody runs Ant code */
  GoThread *qhead, *qtail;   /* waiting for the GIL in arrival order; empty while holder is NULL */
} gil = { .m = PTHREAD_MUTEX_INITIALIZER };

/* Written under gil.m, polled by the holder without it: accessed only
 * through request_switch and ANT_SAFEPOINT (ant.h), both relaxed atomics. */
int ant_switch_requested;

static void request_switch(int on) {
  __atomic_store_n(&ant_switch_requested, on, __ATOMIC_RELAXED);
}

/* The holder's goroutine. A plain global rather than thread-local storage:
 * it is written under gil.m whenever a thread becomes the holder, and
 * nothing reads it without holding the GIL. */
AntGoroutine *ant_g = &main_thread.g;

static __thread GoThread *self;   /* the goroutine on the calling thread */

/* ---------------------------------------------------------------- the GIL (gil.m held) */

static void enqueue(GoThread *t) {
  t->qnext = NULL;
  if (gil.qtail) gil.qtail->qnext = t;
  else gil.qhead = t;
  gil.qtail = t;
}

static GoThread *dequeue(void) {
  GoThread *t = gil.qhead;
  if (t) {
    gil.qhead = t->qnext;
    if (!gil.qhead) gil.qtail = NULL;
    t->qnext = NULL;
  }
  return t;
}

/* The holder gives the GIL to the first waiter, or to nobody; the waiter
 * that becomes the head of the queue is woken too, to start its timer. */
static void handoff(void) {
  gil.holder = dequeue();
  request_switch(0);
  if (gil.holder) pthread_cond_signal(&gil.holder->cv);
  if (gil.qhead) pthread_cond_signal(&gil.qhead->cv);
}

/* Waits, queued, until this thread is the holder. Only the head of the
 * queue keeps a timer: a slice of GIL_INTERVAL_NS that ends without it
 * becoming the holder requests a switch from the holder. Every other
 * waiter sleeps until a hand-off makes it the head or the holder, so a
 * long queue costs nothing while it drains. */
static void wait_turn(GoThread *me) {
  static const struct timespec slice = { 0, GIL_INTERVAL_NS };
  while (gil.holder != me) {
    if (gil.qhead != me) pthread_cond_wait(&me->cv, &gil.m);
    else if (pthread_cond_timedwait_relative_np(&me->cv, &gil.m, &slice) == ETIMEDOUT && gil.holder != me)
      request_switch(1);   /* the slice passed with no hand-off to me */
  }
  ant_g = &me->g;
}

/* Takes the GIL at once when it is free (the queue is then empty), else queues. */
static void acquire_locked(GoThread *me) {
  if (!gil.holder) {
    gil.holder = me;
    ant_g = &me->g;
  } else {
    enqueue(me);
    wait_turn(me);
  }
}

/* ---------------------------------------------------------------- entry points */

void ant_gil_acquire(void) {
  pthread_mutex_lock(&gil.m);
  acquire_locked(self);
  pthread_mutex_unlock(&gil.m);
}

void ant_gil_release(void) {
  pthread_mutex_lock(&gil.m);
  handoff();
  pthread_mutex_unlock(&gil.m);
}

/* A safepoint with a switch requested: the first waiter runs and the
 * holder re-acquires behind the queue. */
void ant_gil_yield(void) {
  pthread_mutex_lock(&gil.m);
  if (gil.qhead) {
    handoff();
    acquire_locked(self);
  } else {
    request_switch(0);
  }
  pthread_mutex_unlock(&gil.m);
}

/* The caller tested its predicate while holding the GIL. Another goroutine
 * can only change it after becoming the holder, which happens in handoff()
 * under gil.m; pthread_cond_wait releases gil.m only once the caller is
 * parked on cv, so the signal that follows the change cannot be missed. */
void ant_block_on(pthread_cond_t *cv) {
  pthread_mutex_lock(&gil.m);
  handoff();
  pthread_cond_wait(cv, &gil.m);
  acquire_locked(self);
  pthread_mutex_unlock(&gil.m);
}

void ant_cond_init(pthread_cond_t *cv) {
  pthread_cond_init(cv, NULL);
}

void ant_cond_signal(pthread_cond_t *cv) {
  pthread_mutex_lock(&gil.m);
  pthread_cond_signal(cv);
  pthread_mutex_unlock(&gil.m);
}

void ant_cond_broadcast(pthread_cond_t *cv) {
  pthread_mutex_lock(&gil.m);
  pthread_cond_broadcast(cv);
  pthread_mutex_unlock(&gil.m);
}

/* ---------------------------------------------------------------- goroutines */

/* A goroutine's thread. A pending panic is reported while this thread
 * still holds the GIL: `main`, woken at the pthread level by the usual
 * `defer { WaitGroup.done(wg) }`, gets the GIL only after the report, so
 * it cannot end the process before the report exists. Nothing between
 * the last deferred block and the report runs a safepoint. */
static void *trampoline(void *p) {
  GoThread *t = p;
  self = t;
  ant_gil_acquire();
  ant_call(t->closure, 0, NULL);
  if (ant_g->unwind.mode == U_PANIC) ant_report_panic();
  ant_gil_release();
  pthread_cond_destroy(&t->cv);
  ant_free(t);
  return NULL;
}

/* `go { }`: starts the closure on its own thread and yields void at once;
 * the new goroutine queues for the GIL behind the caller. The GoThread is
 * uncollectable memory, which the collector scans as a root, so the
 * closure and the unwind state stay reachable while the thread runs. */
AntValue ant_go(AntValue closure) {
  GoThread *t = ant_alloc_uncollectable(sizeof *t);
  ant_cond_init(&t->cv);
  t->closure = closure;
  int rc = ant_thread_spawn(trampoline, t, GO_STACK);
  if (rc != 0) {
    pthread_cond_destroy(&t->cv);
    ant_free(t);
    ant_panicf("cannot start goroutine: %s", strerror(rc));
  }
  return ANT_VOID;
}

/* The main goroutine takes the GIL on the thread that runs `main`. */
void ant_sched_enter_main(void) {
  self = &main_thread;
  ant_gil_acquire();
}
