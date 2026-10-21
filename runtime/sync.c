/* The sync objects (sections 9 and 11): monitors on the GIL. Each blocking
 * object carries a condition variable that waits on the scheduler's mutex;
 * an operation that must wait loops on its predicate around ant_block_on,
 * and every state change that can satisfy a predicate signals or
 * broadcasts it. All state is read and written under the GIL, so no
 * wake-up is lost between a test and its wait. */
#include "ant.h"
#include "sched.h"

typedef struct { AntSync base; pthread_cond_t cv; } Monitor;   /* the first field of every blocking object */
typedef struct { Monitor mon; bool locked; } Mutex;
typedef struct { Monitor mon; int64_t readers; bool writer; } RwMutex;
typedef struct { Monitor mon; int64_t n; } WaitGroup;
typedef struct { Monitor mon; int state; } Once;
typedef struct { Monitor mon; Mutex *m; int64_t waiting, wakeups; } Cond;
typedef struct { AntSync base; AntValue v; } Atomic;

static AntValue sync_new(const char *name, size_t size) {
  AntSync *s = ant_alloc(size);
  s->name = name;
  return ant_ptr(K_SYNC, s);
}

static AntValue monitor_new(const char *name, size_t size) {
  AntValue v = sync_new(name, size);
  ant_cond_init(&((Monitor *)v.as.p)->cv);
  return v;
}

void *ant_sync_need(AntValue v, const char *name) {
  if (v.kind != K_SYNC || ((AntSync *)v.as.p)->name != name) {
    ant_wrong_kind(ant_fmt("a%s %s", name[0] == 'A' ? "n" : "", name));   /* "an Atomic" */
    return NULL;
  }
  return v.as.p;
}

/* `f` between lock and unlock (both natives on argv[0]): the unlock runs
 * even when f panics, with the panic set aside like a deferred block's. */
static AntValue with_lock(AntFunc *self, AntValue *argv, AntNative lock, AntNative unlock) {
  FUNC(argv[1]);
  lock(self, 1, argv);
  UNWOUND();
  AntValue r = ant_call(argv[1], 0, NULL);
  AntUnwind saved;
  ant_unwind_aside(&saved);
  unlock(self, 1, argv);
  ant_unwind_merge(&saved);
  UNWOUND();
  return r;
}

/* ---------------------------------------------------------------- Mutex */

/* Not owned: any goroutine may unlock it (section 11). */
static const char MUTEX[] = "Mutex";

static AntValue mutex_new(AntFunc *self, int argc, AntValue *argv) {
  return monitor_new(MUTEX, sizeof(Mutex));
}

/* The wake-up discipline, shared with Cond: a release signals one waiter,
 * which re-tests. */
static void mutex_acquire(Mutex *m) {
  while (m->locked) ant_block_on(&m->mon.cv);
  m->locked = true;
}

static void mutex_release(Mutex *m) {
  m->locked = false;
  ant_cond_signal(&m->mon.cv);
}

static AntValue mutex_lock(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], Mutex, MUTEX, m);
  mutex_acquire(m);
  return ANT_VOID;
}

static AntValue mutex_unlock(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], Mutex, MUTEX, m);
  if (!m->locked) { ant_panic("not locked"); return ANT_VOID; }
  mutex_release(m);
  return ANT_VOID;
}

static AntValue mutex_with_lock(AntFunc *self, int argc, AntValue *argv) {
  return with_lock(self, argv, mutex_lock, mutex_unlock);
}

void ant_ns_mutex(void) {
  static const AntMember members[] = {
    { "new", "", mutex_new },
    { "lock", "m", mutex_lock },
    { "unlock", "m", mutex_unlock },
    { "withLock", "m,f", mutex_with_lock },
  };
  ANT_NAMESPACE("Mutex", members);
}

/* ---------------------------------------------------------------- RwMutex */

/* Readers are admitted while no writer holds it (G51); no writer preference. */
static const char RWMUTEX[] = "RwMutex";

static AntValue rw_new(AntFunc *self, int argc, AntValue *argv) {
  return monitor_new(RWMUTEX, sizeof(RwMutex));
}

static AntValue rw_read_lock(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], RwMutex, RWMUTEX, m);
  while (m->writer) ant_block_on(&m->mon.cv);
  m->readers++;
  return ANT_VOID;
}

static AntValue rw_read_unlock(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], RwMutex, RWMUTEX, m);
  if (m->readers == 0) { ant_panic("not locked"); return ANT_VOID; }
  m->readers--;
  if (m->readers == 0) ant_cond_broadcast(&m->mon.cv);
  return ANT_VOID;
}

static AntValue rw_write_lock(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], RwMutex, RWMUTEX, m);
  while (m->writer || m->readers > 0) ant_block_on(&m->mon.cv);
  m->writer = true;
  return ANT_VOID;
}

static AntValue rw_write_unlock(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], RwMutex, RWMUTEX, m);
  if (!m->writer) { ant_panic("not locked"); return ANT_VOID; }
  m->writer = false;
  ant_cond_broadcast(&m->mon.cv);
  return ANT_VOID;
}

static AntValue rw_with_read(AntFunc *self, int argc, AntValue *argv) {
  return with_lock(self, argv, rw_read_lock, rw_read_unlock);
}

static AntValue rw_with_write(AntFunc *self, int argc, AntValue *argv) {
  return with_lock(self, argv, rw_write_lock, rw_write_unlock);
}

void ant_ns_rwmutex(void) {
  static const AntMember members[] = {
    { "new", "", rw_new },
    { "readLock", "m", rw_read_lock },
    { "readUnlock", "m", rw_read_unlock },
    { "writeLock", "m", rw_write_lock },
    { "writeUnlock", "m", rw_write_unlock },
    { "withRead", "m,f", rw_with_read },
    { "withWrite", "m,f", rw_with_write },
  };
  ANT_NAMESPACE("RwMutex", members);
}

/* ---------------------------------------------------------------- WaitGroup */

static const char WAITGROUP[] = "WaitGroup";

static AntValue wg_new(AntFunc *self, int argc, AntValue *argv) {
  return monitor_new(WAITGROUP, sizeof(WaitGroup));
}

/* The counter rule of add and done: never below zero, and waiters wake
 * when it reaches zero. */
static AntValue wg_change(WaitGroup *wg, int64_t n) {
  if (wg->n + n < 0) { ant_panic("negative counter"); return ANT_VOID; }
  wg->n += n;
  if (wg->n == 0) ant_cond_broadcast(&wg->mon.cv);
  return ANT_VOID;
}

static AntValue wg_add(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], WaitGroup, WAITGROUP, wg);
  INT(argv[1], n);
  return wg_change(wg, n);
}

static AntValue wg_done(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], WaitGroup, WAITGROUP, wg);
  return wg_change(wg, -1);
}

static AntValue wg_wait(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], WaitGroup, WAITGROUP, wg);
  while (wg->n > 0) ant_block_on(&wg->mon.cv);
  return ANT_VOID;
}

void ant_ns_waitgroup(void) {
  static const AntMember members[] = {
    { "new", "", wg_new },
    { "add", "wg,n", wg_add },
    { "done", "wg", wg_done },
    { "wait", "wg", wg_wait },
  };
  ANT_NAMESPACE("WaitGroup", members);
}

/* ---------------------------------------------------------------- Once */

static const char ONCE[] = "Once";
enum { ONCE_NEW, ONCE_RUNNING, ONCE_DONE };

static AntValue once_new(AntFunc *self, int argc, AntValue *argv) {
  return monitor_new(ONCE, sizeof(Once));
}

/* The first call runs f and counts as done even when f panics (G30); the
 * panic stays pending for that caller. Concurrent callers wait for f to
 * finish; later ones return at once. */
static AntValue once_do(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], Once, ONCE, o);
  FUNC(argv[1]);
  if (o->state == ONCE_NEW) {
    o->state = ONCE_RUNNING;
    ant_call(argv[1], 0, NULL);
    o->state = ONCE_DONE;
    ant_cond_broadcast(&o->mon.cv);
  }
  while (o->state == ONCE_RUNNING) ant_block_on(&o->mon.cv);
  return ANT_VOID;
}

void ant_ns_once(void) {
  static const AntMember members[] = {
    { "new", "", once_new },
    { "do", "once,f", once_do },
  };
  ANT_NAMESPACE("Once", members);
}

/* ---------------------------------------------------------------- Cond */

/* `waiting` counts goroutines parked in wait and `wakeups` how many of
 * them signal or broadcast has released but which have not left yet, so
 * exactly that many leave whatever pthread wakes. */
static const char COND[] = "Cond";

static AntValue cond_new(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], Mutex, MUTEX, m);
  AntValue c = monitor_new(COND, sizeof(Cond));
  ((Cond *)c.as.p)->m = m;
  return c;
}

/* Releases the mutex, waits for a wake-up, re-acquires the mutex. */
static AntValue cond_wait(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], Cond, COND, c);
  Mutex *m = c->m;
  if (!m->locked) { ant_panic("not locked"); return ANT_VOID; }
  mutex_release(m);
  c->waiting++;
  do ant_block_on(&c->mon.cv); while (c->wakeups == 0);
  c->wakeups--;
  c->waiting--;
  mutex_acquire(m);
  return ANT_VOID;
}

static AntValue cond_signal(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], Cond, COND, c);
  if (c->waiting > c->wakeups) {
    c->wakeups++;
    ant_cond_broadcast(&c->mon.cv);
  }
  return ANT_VOID;
}

static AntValue cond_broadcast(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], Cond, COND, c);
  c->wakeups = c->waiting;
  ant_cond_broadcast(&c->mon.cv);
  return ANT_VOID;
}

void ant_ns_cond(void) {
  static const AntMember members[] = {
    { "new", "m", cond_new },
    { "wait", "c", cond_wait },
    { "signal", "c", cond_signal },
    { "broadcast", "c", cond_broadcast },
  };
  ANT_NAMESPACE("Cond", members);
}

/* ---------------------------------------------------------------- Atomic */

/* Every operation is one C function under the GIL with no callback, hence atomic. */
static const char ATOMIC[] = "Atomic";

static AntValue atomic_new(AntFunc *self, int argc, AntValue *argv) {
  AntValue a = sync_new(ATOMIC, sizeof(Atomic));
  ((Atomic *)a.as.p)->v = argv[0];
  return a;
}

static AntValue atomic_load(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], Atomic, ATOMIC, a);
  return a->v;
}

static AntValue atomic_store(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], Atomic, ATOMIC, a);
  a->v = argv[1];
  return ANT_VOID;
}

/* Through `+` (section 12), so a delta of another kind is `kind mismatch`
 * and an overflow `integer overflow`; returns the new value. */
static AntValue atomic_add(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], Atomic, ATOMIC, a);
  NUM(argv[1]);
  NUM(a->v);
  AntValue r = ant_add(a->v, argv[1]);
  UNWOUND();
  a->v = r;
  return r;
}

static AntValue atomic_cas(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], Atomic, ATOMIC, a);
  if (!ant_equal(a->v, argv[1])) return ANT_FALSE;
  a->v = argv[2];
  return ANT_TRUE;
}

void ant_ns_atomic(void) {
  static const AntMember members[] = {
    { "new", "v", atomic_new },
    { "load", "a", atomic_load },
    { "store", "a,v", atomic_store },
    { "add", "a,d", atomic_add },
    { "compareAndSwap", "a,expected,v", atomic_cas },
  };
  ANT_NAMESPACE("Atomic", members);
}
