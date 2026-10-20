/* The sync objects (sections 9 and 11) for a single-threaded runtime: the
 * state and panics are those of the design, and an operation that would
 * block forever without another goroutine panics `deadlock`. Step 2.6
 * replaces the waiting. */
#include "ant.h"

typedef struct { AntSync base; bool locked; } Mutex;
typedef struct { AntSync base; int64_t readers; bool writer; } RwMutex;
typedef struct { AntSync base; int64_t n; } WaitGroup;
typedef struct { AntSync base; int state; } Once;
typedef struct { AntSync base; Mutex *m; } Cond;
typedef struct { AntSync base; AntValue v; } Atomic;

static AntValue sync_new(const char *name, size_t size) {
  AntSync *s = ant_alloc(size);
  s->name = name;
  return ant_ptr(K_SYNC, s);
}

static void *need(AntValue v, const char *name) {
  if (v.kind != K_SYNC || ((AntSync *)v.as.p)->name != name) {
    ant_panicf("wrong kind: expected a %s", name);
    return NULL;
  }
  return v.as.p;
}

static AntValue deadlock(const char *what) {
  ant_panicf("deadlock: %s with no other goroutines (step 2.6)", what);
  return ANT_VOID;
}

/* `f` between lock and unlock (both natives on argv[0]): the unlock runs
 * even when f panics, and the panic survives it. */
static AntValue with_lock(AntFunc *self, AntValue *argv, AntNative lock, AntNative unlock) {
  FUNC(argv[1]);
  lock(self, 1, argv);
  if (ant_g->unwind.mode) return ANT_VOID;
  AntValue r = ant_call(argv[1], 0, NULL);
  AntUnwind saved = ant_g->unwind;
  ant_g->unwind.mode = U_NONE;
  unlock(self, 1, argv);
  if (saved.mode) ant_g->unwind = saved;
  return ant_g->unwind.mode ? ANT_VOID : r;
}

/* ---------------------------------------------------------------- Mutex */

static const char MUTEX[] = "Mutex";

static AntValue mutex_new(AntFunc *self, int argc, AntValue *argv) {
  return sync_new(MUTEX, sizeof(Mutex));
}

static AntValue mutex_lock(AntFunc *self, int argc, AntValue *argv) {
  Mutex *m = need(argv[0], MUTEX);
  if (!m) return ANT_VOID;
  if (m->locked) return deadlock("Mutex.lock");
  m->locked = true;
  return ANT_VOID;
}

static AntValue mutex_unlock(AntFunc *self, int argc, AntValue *argv) {
  Mutex *m = need(argv[0], MUTEX);
  if (!m) return ANT_VOID;
  if (!m->locked) { ant_panic("not locked"); return ANT_VOID; }
  m->locked = false;
  return ANT_VOID;
}

static AntValue mutex_with_lock(AntFunc *self, int argc, AntValue *argv) {
  return with_lock(self, argv, mutex_lock, mutex_unlock);
}

AntValue ant_ns_mutex(void) {
  static const AntMember members[] = {
    { "new", "", mutex_new },
    { "lock", "m", mutex_lock },
    { "unlock", "m", mutex_unlock },
    { "withLock", "m,f", mutex_with_lock },
  };
  return ANT_NAMESPACE("Mutex", members);
}

/* ---------------------------------------------------------------- RwMutex */

static const char RWMUTEX[] = "RwMutex";

static AntValue rw_new(AntFunc *self, int argc, AntValue *argv) {
  return sync_new(RWMUTEX, sizeof(RwMutex));
}

static AntValue rw_read_lock(AntFunc *self, int argc, AntValue *argv) {
  RwMutex *m = need(argv[0], RWMUTEX);
  if (!m) return ANT_VOID;
  if (m->writer) return deadlock("RwMutex.readLock");
  m->readers++;
  return ANT_VOID;
}

static AntValue rw_read_unlock(AntFunc *self, int argc, AntValue *argv) {
  RwMutex *m = need(argv[0], RWMUTEX);
  if (!m) return ANT_VOID;
  if (m->readers == 0) { ant_panic("not locked"); return ANT_VOID; }
  m->readers--;
  return ANT_VOID;
}

static AntValue rw_write_lock(AntFunc *self, int argc, AntValue *argv) {
  RwMutex *m = need(argv[0], RWMUTEX);
  if (!m) return ANT_VOID;
  if (m->writer || m->readers) return deadlock("RwMutex.writeLock");
  m->writer = true;
  return ANT_VOID;
}

static AntValue rw_write_unlock(AntFunc *self, int argc, AntValue *argv) {
  RwMutex *m = need(argv[0], RWMUTEX);
  if (!m) return ANT_VOID;
  if (!m->writer) { ant_panic("not locked"); return ANT_VOID; }
  m->writer = false;
  return ANT_VOID;
}

static AntValue rw_with_read(AntFunc *self, int argc, AntValue *argv) {
  return with_lock(self, argv, rw_read_lock, rw_read_unlock);
}

static AntValue rw_with_write(AntFunc *self, int argc, AntValue *argv) {
  return with_lock(self, argv, rw_write_lock, rw_write_unlock);
}

AntValue ant_ns_rwmutex(void) {
  static const AntMember members[] = {
    { "new", "", rw_new },
    { "readLock", "m", rw_read_lock },
    { "readUnlock", "m", rw_read_unlock },
    { "writeLock", "m", rw_write_lock },
    { "writeUnlock", "m", rw_write_unlock },
    { "withRead", "m,f", rw_with_read },
    { "withWrite", "m,f", rw_with_write },
  };
  return ANT_NAMESPACE("RwMutex", members);
}

/* ---------------------------------------------------------------- WaitGroup */

static const char WAITGROUP[] = "WaitGroup";

static AntValue wg_new(AntFunc *self, int argc, AntValue *argv) {
  return sync_new(WAITGROUP, sizeof(WaitGroup));
}

static AntValue wg_add(AntFunc *self, int argc, AntValue *argv) {
  WaitGroup *wg = need(argv[0], WAITGROUP);
  if (!wg) return ANT_VOID;
  INT(argv[1], n);
  if (wg->n + n < 0) { ant_panic("negative counter"); return ANT_VOID; }
  wg->n += n;
  return ANT_VOID;
}

static AntValue wg_done(AntFunc *self, int argc, AntValue *argv) {
  WaitGroup *wg = need(argv[0], WAITGROUP);
  if (!wg) return ANT_VOID;
  if (wg->n == 0) { ant_panic("negative counter"); return ANT_VOID; }
  wg->n--;
  return ANT_VOID;
}

static AntValue wg_wait(AntFunc *self, int argc, AntValue *argv) {
  WaitGroup *wg = need(argv[0], WAITGROUP);
  if (!wg) return ANT_VOID;
  if (wg->n > 0) return deadlock("WaitGroup.wait");
  return ANT_VOID;
}

AntValue ant_ns_waitgroup(void) {
  static const AntMember members[] = {
    { "new", "", wg_new },
    { "add", "wg,n", wg_add },
    { "done", "wg", wg_done },
    { "wait", "wg", wg_wait },
  };
  return ANT_NAMESPACE("WaitGroup", members);
}

/* ---------------------------------------------------------------- Once */

static const char ONCE[] = "Once";

static AntValue once_new(AntFunc *self, int argc, AntValue *argv) {
  return sync_new(ONCE, sizeof(Once));
}

/* The first call runs f and counts as done even when f panics (G30). */
static AntValue once_do(AntFunc *self, int argc, AntValue *argv) {
  Once *o = need(argv[0], ONCE);
  if (!o) return ANT_VOID;
  FUNC(argv[1]);
  if (o->state) return ANT_VOID;
  o->state = 1;
  ant_call(argv[1], 0, NULL);
  return ANT_VOID;
}

AntValue ant_ns_once(void) {
  static const AntMember members[] = {
    { "new", "", once_new },
    { "do", "once,f", once_do },
  };
  return ANT_NAMESPACE("Once", members);
}

/* ---------------------------------------------------------------- Cond */

static const char COND[] = "Cond";

static AntValue cond_new(AntFunc *self, int argc, AntValue *argv) {
  Mutex *m = need(argv[0], MUTEX);
  if (!m) return ANT_VOID;
  AntValue c = sync_new(COND, sizeof(Cond));
  ((Cond *)c.as.p)->m = m;
  return c;
}

static AntValue cond_wait(AntFunc *self, int argc, AntValue *argv) {
  Cond *c = need(argv[0], COND);
  if (!c) return ANT_VOID;
  if (!c->m->locked) { ant_panic("not locked"); return ANT_VOID; }
  return deadlock("Cond.wait");
}

static AntValue cond_signal(AntFunc *self, int argc, AntValue *argv) {
  if (!need(argv[0], COND)) return ANT_VOID;
  return ANT_VOID;
}

AntValue ant_ns_cond(void) {
  static const AntMember members[] = {
    { "new", "m", cond_new },
    { "wait", "c", cond_wait },
    { "signal", "c", cond_signal },
    { "broadcast", "c", cond_signal },
  };
  return ANT_NAMESPACE("Cond", members);
}

/* ---------------------------------------------------------------- Atomic */

static const char ATOMIC[] = "Atomic";

static AntValue atomic_new(AntFunc *self, int argc, AntValue *argv) {
  AntValue a = sync_new(ATOMIC, sizeof(Atomic));
  ((Atomic *)a.as.p)->v = argv[0];
  return a;
}

static AntValue atomic_load(AntFunc *self, int argc, AntValue *argv) {
  Atomic *a = need(argv[0], ATOMIC);
  if (!a) return ANT_VOID;
  return a->v;
}

static AntValue atomic_store(AntFunc *self, int argc, AntValue *argv) {
  Atomic *a = need(argv[0], ATOMIC);
  if (!a) return ANT_VOID;
  a->v = argv[1];
  return ANT_VOID;
}

static AntValue atomic_add(AntFunc *self, int argc, AntValue *argv) {
  Atomic *a = need(argv[0], ATOMIC);
  if (!a) return ANT_VOID;
  if (!ant_is_number(a->v.kind) || !ant_is_number(argv[1].kind) || a->v.kind != argv[1].kind) {
    ant_panic("wrong kind: expected numbers of one kind");
    return ANT_VOID;
  }
  AntValue r = ant_add(a->v, argv[1]);
  if (ant_g->unwind.mode) return ANT_VOID;
  a->v = r;
  return r;
}

static AntValue atomic_cas(AntFunc *self, int argc, AntValue *argv) {
  Atomic *a = need(argv[0], ATOMIC);
  if (!a) return ANT_VOID;
  if (!ant_equal(a->v, argv[1])) return ANT_FALSE;
  a->v = argv[2];
  return ANT_TRUE;
}

AntValue ant_ns_atomic(void) {
  static const AntMember members[] = {
    { "new", "v", atomic_new },
    { "load", "a", atomic_load },
    { "store", "a,v", atomic_store },
    { "add", "a,d", atomic_add },
    { "compareAndSwap", "a,expected,v", atomic_cas },
  };
  return ANT_NAMESPACE("Atomic", members);
}
