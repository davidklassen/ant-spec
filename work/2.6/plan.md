# Plan for step 2.6 "rt-concurrency"

Row: GIL scheduler, goroutines, sync objects, `Time.sleep`. Allowed: `runtime/`, `src/antc/`, `Makefile`,
`bootstrap/antpy/system.py` (`Os.exec` only; nothing to do there), `work/2.6/`. Design: `docs/runtime-design.md`
section 3 (follow it; the deviations in section 9 below go into the report).

Acceptance (verbatim):

    make fixpoint && python3 -I src/tests/run.py --run "bin/antc run {file} {args}" --run-unchecked "bin/antc run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --tier programs --filter /concurrency/ --filter race-memory-safe --filter parallel-sum --filter producer-consumer

It selects 31 tests: `run/concurrency` 19, `panic/concurrency` 8, `unchecked/concurrency` 1, programs
`race-memory-safe`, `prog-parallel-sum`, `prog-producer-consumer`. Runner facts that matter: a run-tier test must
exit 0 and its stdout must match; `stderr:` is a substring check (four tests use it for a goroutine panic); stderr
is otherwise ignored; timeout 60 s per test, which is how a deadlock shows up ("timed out").

## 0. State of the tree (verified by the planner; do not re-investigate)

- `runtime/sched.c` is a 10-line stub: `ant_go` runs the closure synchronously and drops its panic.
  `runtime/sync.c` already has every sync object with the design's state and panics (`not locked`, `negative
  counter`, `wrong kind: expected a Mutex`, `Atomic.add` through `ant_add`, `compareAndSwap` through
  `ant_equal`, `Once` counting a panicking `f` as done, `with_lock` releasing on panic); every operation that
  would block panics `deadlock` instead. Keep all of that; replace only the blocking.
- `runtime/ant.h`: `#define ANT_SAFEPOINT() ((void)0)`; `AntGoroutine { unwind, top, pairs }`;
  `extern AntGoroutine *ant_g` (defined in `unwind.c` as a plain global pointing at a static). The emitter
  already writes `ANT_SAFEPOINT();` after `ant_frame_push` of every function and at the top of every `while`
  iteration (`emitFunction`, `emitWhile`), and `ant_go(closure)` for `go { }` (`emitGo`). **No emitter change
  is needed**; `src/antc/` stays untouched, so the emitted C is byte-identical to today's and `make fixpoint`
  holds by construction (it still rebuilds the runtime and the three stages because `ant.h` changes).
- `for` bodies are closures with no entry safepoint (`emitForBody` emits none; `work/2.1/report.md` asks 2.6 to
  poll in `iterate()` and `yield_code()` of `func.c`).
- `runtime/unwind.c`: `ant_run_main` runs `init` + `main` on a 512 MB thread (`ant_run_on_big_stack`, alloc.c,
  created through Boehm's `pthread_create` redirect: `#define GC_THREADS` + `<gc.h>` in alloc.c only) and the
  process's original thread then calls `ant_exit`. `ant_report_panic` writes `panic: <msg>` + trace lines, flushing
  stdout first, and clears the unwind state. `ant_exit` is `fflush` + `_exit`.
- `runtime/io.c`: `Time.sleep` is `nanosleep` without releasing anything; `Os.exit` is `ant_exit`.
- `runtime/alloc.c`: `GC_MALLOC`/`ATOMIC`/`REALLOC` wrappers, `GC_INIT`, the big-stack thread. Homebrew bdw-gc
  8.2.12 (`/opt/homebrew/include/gc.h`, `-lgc -lpthread` already in `runtime/libantrt.flags`).
- Arrays never shrink their buffers (`array.c` only grows `cap`), which section 5 relies on.
- Mutable file-scope state in the runtime: the symbol/shape tables, the prelude globals, `yield_info`, the
  per-site `AntFieldCache` statics in generated code. All are touched only under the GIL; none is held across a
  safepoint, so nothing needs a lock.
- Natives that call back into Ant code: `array.c` (map, filter, reduce, find, some, every, sortBy, sortWith),
  `map.c` (map, filter), `iter.c` (every stage), `result.c` (map, mapErr), `func.c` (`iterate`, `yield_code`,
  `ant_call_truth`, `ant_run_defers`), `sync.c` (`with_lock`, `once_do`). Section 5 is their audit.

## 1. Files

| File | Change |
|---|---|
| `runtime/sched.c` | rewrite: GIL, goroutine threads, `ant_go`, safepoint yield, blocking helpers, the main goroutine |
| `runtime/sched.h` | new, private (included by `sched.c`, `sync.c`, `io.c` only): `<pthread.h>`, `ant_block_on`, `ant_cond_*` |
| `runtime/ant.h` | real `ANT_SAFEPOINT`, `ant_switch_requested`, `ant_gil_yield/release/acquire`, `ant_sched_init`, `ant_sched_enter_main`, alloc helpers |
| `runtime/alloc.c` | `ant_alloc_uncollectable`, `ant_free`, `ant_thread_spawn` (detached, registered with the collector) |
| `runtime/unwind.c` | drop `main_goroutine`/`ant_g` definitions (moved to sched.c); `run_main_thread` takes the GIL and exits in place |
| `runtime/sync.c` | condition variables and wait loops replace `deadlock()`; `Cond` gets wake-up counters |
| `runtime/io.c` | `Time.sleep` releases the GIL around `nanosleep` |
| `runtime/func.c` | `ANT_SAFEPOINT()` at the top of `iterate()` and `yield_code()` |
| `work/2.6/test.log`, `work/2.6/report.md` | acceptance output; report (section 9 lists what it must record) |

`Makefile`, `src/antc/`, `bootstrap/` unchanged. No test needs changing (none contradicts the spec).

## 2. The scheduler (`runtime/sched.c`)

### 2.1 Model

One OS thread per goroutine; one global interpreter lock (GIL) that a thread must hold to run Ant code or touch
any `AntValue`; the holder gives it up at safepoints when asked, when it blocks on a sync object, when it sleeps,
and when it ends. Hand-off is direct and FIFO: the releaser names the next holder, so a releaser can never
re-acquire ahead of a waiter and a spinner cannot starve `main` (goroutines-preemptive: 8 spinners, main must
print and exit). There is no ticker thread: a waiter that has waited 5 ms without a hand-off requests the switch
itself (CPython's drop request). The pthread mutex `gil.m` protects only the scheduler's own fields and is never
held while running Ant code or allocating.

### 2.2 Data

```c
/* sched.c */
typedef struct GoThread GoThread;
struct GoThread {
  AntGoroutine g;            /* first member: (GoThread *)ant_g */
  pthread_cond_t cv;         /* signalled when this thread becomes the holder */
  GoThread *qnext;           /* the GIL wait queue (FIFO) */
  GoThread *next, *prev;     /* every live goroutine, so the collector sees g (Boehm does not scan __thread) */
  AntValue closure;          /* the go block until it has started; ANT_VOID afterwards */
};
static struct {
  pthread_mutex_t m;
  GoThread *holder;          /* NULL when nobody runs Ant code */
  GoThread *qhead, *qtail;   /* waiting for the GIL, in arrival order */
  GoThread *all;             /* list head; main_thread is always on it */
  uint64_t generation;       /* incremented by every hand-off */
} gil;
volatile int ant_switch_requested;        /* read by ANT_SAFEPOINT in generated code */
static GoThread main_thread;
AntGoroutine *ant_g = &main_thread.g;     /* moved here from unwind.c */
static __thread GoThread *self;           /* who is calling the scheduler; set at thread start */
enum { GIL_INTERVAL_NS = 5 * 1000 * 1000, GO_STACK = 64 << 20 };
```

`ant_g` stays a plain global (design section 1 says `__thread`; see section 9): the scheduler stores
`ant_g = &holder->g` every time a thread becomes the holder, so generated code and natives keep reading one
global after every call instead of going through Darwin's TLS thunk at every `CHK`. The rule that makes this
sound: **no code touches `ant_g` or any `AntValue` unless its thread holds the GIL** (the only code that runs
without it is `nanosleep` in `Time.sleep`, the trampoline before `ant_gil_acquire`, and the scheduler itself).

### 2.3 Operations (all with `gil.m` locked unless said otherwise)

```
wait_turn(me):                       /* m locked; me is queued; returns as holder, m still locked */
  while (gil.holder != me) {
    gen = gil.generation
    rc = pthread_cond_timedwait(&me->cv, &gil.m, now + GIL_INTERVAL_NS)   /* CLOCK_REALTIME absolute */
    if (rc == ETIMEDOUT && gil.holder != me && gil.generation == gen) ant_switch_requested = 1
  }
  ant_g = &me->g

handoff():                            /* m locked; the caller is the holder and gives the GIL away */
  gil.holder = dequeue()              /* NULL when the queue is empty */
  gil.generation++; ant_switch_requested = 0
  if (gil.holder) pthread_cond_signal(&gil.holder->cv)

ant_gil_acquire():      lock(m); if (!holder && !qhead) { holder = self; ant_g = &self->g; } else { enqueue(self); wait_turn(self); } unlock(m)
ant_gil_release():      lock(m); handoff(); unlock(m)
ant_gil_yield():        lock(m); if (!qhead) { ant_switch_requested = 0; unlock(m); return; }
                        enqueue(self); handoff(); wait_turn(self); unlock(m)
ant_block_on(cv):       lock(m); handoff(); pthread_cond_wait(cv, &m);       /* one wake-up */
                        if (!holder && !qhead) { holder = self; ant_g = &self->g; } else { enqueue(self); wait_turn(self); }
                        unlock(m)
ant_cond_signal(cv):    lock(m); pthread_cond_signal(cv); unlock(m)          /* caller holds the GIL */
ant_cond_broadcast(cv): lock(m); pthread_cond_broadcast(cv); unlock(m)
ant_cond_init(cv):      pthread_cond_init(cv, NULL)
```

Why there are no lost wake-ups: a sync native tests its predicate while holding the GIL, and the only way
another goroutine can change that predicate is to become the holder, which happens inside `ant_block_on` under
`gil.m`, after which `pthread_cond_wait` releases `gil.m` atomically; the new holder cannot even return from its
own `cond_wait` on `gil.m` before the blocking thread is parked on `cv`. Every caller of `ant_block_on` loops on
its predicate (spurious wake-ups and barging are fine). `generation` keeps a waiter from re-requesting a switch
that just happened; the new holder then runs up to 5 ms before the next request.

### 2.4 Goroutines, `main`, exit

```
ant_go(closure):                               /* caller holds the GIL */
  t = ant_alloc_uncollectable(sizeof *t); memset 0; ant_cond_init(&t->cv); t->closure = closure
  lock(m); link t into gil.all; unlock(m)
  if (ant_thread_spawn(trampoline, t, GO_STACK) != 0) { lock; unlink; unlock; ant_free(t); ant_panicf("cannot start goroutine: %s", strerror(rc)); }
  return ANT_VOID                               /* `go` yields void; main continues until a safepoint or a block */

trampoline(t):
  self = t; ant_gil_acquire()
  ant_call(t->closure, 0, NULL); t->closure = ANT_VOID
  if (ant_g->unwind.mode == U_PANIC) ant_report_panic()    /* STILL HOLDING THE GIL (see below) */
  ant_g->unwind.mode = U_NONE
  lock(m); unlink t; handoff(); unlock(m)
  pthread_cond_destroy(&t->cv); ant_free(t); return NULL

ant_sched_init():        /* from ant_runtime_init, on the process's original thread */
  pthread_mutex_init(&gil.m); ant_cond_init(&main_thread.cv); gil.all = &main_thread
ant_sched_enter_main():  /* first line of run_main_thread (unwind.c) */
  self = &main_thread; ant_gil_acquire()
```

`run_main_thread` (unwind.c) becomes: `ant_sched_enter_main(); init(); if no unwind: call main; if PANIC:
report, ant_exit(2); else ant_exit(0)`. The main goroutine's thread exits the process itself while holding the
GIL, so no other goroutine is mid-operation: `fflush(stdout); fflush(stderr); _exit` (never `exit()`: no atexit
with live threads). `ant_run_main` keeps its signature (the generated `main` calls it); after
`ant_run_on_big_stack` returns, which it no longer does, leave `ant_exit(2)`. `Os.exit` is unchanged (`ant_exit`
from whichever goroutine holds the GIL, no deferred blocks, G39). `go-main-exit-cuts`: main returns while the
goroutine sleeps outside the GIL; the process is gone before "late".

**Invariant for the goroutine panic report** (goroutine-panic-isolated, goroutine-panic-defers-run,
mutex-withlock-returns-and-releases, once-panic-in-f all check `stderr:`): the report is written by the thread
that still holds the GIL, and between the last deferred block's return and `ant_report_panic` nothing runs a
safepoint (only `ant_run_defers`'s loop, block exits, the closure epilogue and the trampoline, all C). The usual
`defer { WaitGroup.done(wg) }` therefore wakes `main` at the pthread level only; `main` gets the GIL after the
report, so it can never `_exit` before the report exists. Do not add a safepoint to `ant_run_defers`, the
function epilogue or the trampoline.

### 2.5 Safepoints

`ant.h`: `extern volatile int ant_switch_requested; void ant_gil_yield(void);
#define ANT_SAFEPOINT() do { if (ant_switch_requested) ant_gil_yield(); } while (0)`. The emitter already places
it at every function entry and `while` back-edge; add it at the top of `iterate()` (arrays, maps, strings) and
`yield_code()` (sequences, native and user) in `func.c` so a `for` loop whose body is a closure without an
entry safepoint is preemptible too (race-memory-safe's 32 goroutines spend their lives in `for`). Natives
themselves need none: they are bounded except through callbacks, whose callees have entry safepoints.

### 2.6 `alloc.c` helpers

`ant_alloc_uncollectable` (`GC_MALLOC_UNCOLLECTABLE`; the goroutine struct holds `unwind.value`, the trace, the
closure and the pair stack, so the collector must scan it and must not free it while its thread runs),
`ant_free` (`GC_FREE`), and `ant_thread_spawn(fn, arg, stack)`: `pthread_attr_setstacksize`,
`PTHREAD_CREATE_DETACHED`, `pthread_create` (which gc.h redirects to `GC_pthread_create`, so every goroutine thread
is registered with the collector exactly as the big-stack main thread is); returns 0 or the errno. `gc.h` stays
in alloc.c alone; `sched.c` includes `<pthread.h>` through `sched.h` and never gc.h. Never call `ant_alloc` while
holding `gil.m` (Boehm takes its own lock and may stop the world; keep the two locks unordered by never nesting
them). 64 MB per goroutine stack is virtual and lazily committed (100 goroutines in mutex-counter and
atomic-add-float = 6.4 GB reserved, fine on arm64 macOS); generated frames are large, as the 512 MB main stack
shows.

## 3. Sync objects (`runtime/sync.c`)

Add `pthread_cond_t cv` to `Mutex`, `RwMutex`, `WaitGroup`, `Once`, `Cond` (initialised in `sync_new`; a
condvar in collectable memory is fine on macOS, there is no kernel object to leak); delete `deadlock()`. Every
blocking operation is `while (!predicate) ant_block_on(&x->cv);` and every state change that can satisfy a
predicate signals or broadcasts. The kind checks, panics and the `with_lock`/`once_do` shapes stay as they are.

- `Mutex`: lock `while (m->locked) block; m->locked = true`. unlock: `not locked` when free, else
  `locked = false; signal`. No owner: any goroutine may unlock (Go semantics; the bootstrap agrees).
- `RwMutex` (G51), one cv: readLock `while (writer) block; readers++`; readUnlock: `readers == 0` -> `not locked`,
  else `readers--; if (readers == 0) broadcast`; writeLock `while (writer || readers > 0) block; writer = true`;
  writeUnlock: `!writer` -> `not locked`, else `writer = false; broadcast`. No writer preference. rwmutex-ops
  holds two read locks across goroutines, then takes the write lock after both are released.
- `WaitGroup`: add: `n + d < 0` -> `negative counter`; `n += d; if (n == 0) broadcast` (add(wg, 0) and a wait on
  zero return at once: waitgroup-zero). done: `n == 0` -> `negative counter`; `n--; if (n == 0) broadcast`.
  wait: `while (n > 0) block`.
- `Once`: states NEW/RUNNING/DONE. do: DONE -> return; RUNNING -> `while (state == RUNNING) block; return`
  (once-do: 19 callers wait while the first is held in `f` by a WaitGroup); NEW -> `state = RUNNING; ant_call(f);
  state = DONE; broadcast; return` with the panic, if any, left pending (G30, once-panic-in-f: later calls return
  at once, `calls == 1`).
- `Cond` `{ Mutex *m; cv; int64 waiting, wakeups; }`: wait: `!m->locked` -> `not locked`
  (cond-wait-unlocked); then `m->locked = false; ant_cond_signal(&m->cv); waiting++;
  do block_on(&c->cv) while (wakeups == 0); wakeups--; waiting--; while (m->locked) block_on(&m->cv);
  m->locked = true`. signal: `if (waiting > wakeups) { wakeups++; ant_cond_broadcast(&c->cv); }`. broadcast:
  `wakeups = waiting; ant_cond_broadcast(&c->cv)`. The counters make the number of waiters released exact
  whatever pthread wakes; `pthread_cond_signal` for `signal` is an optional optimisation.
- `Atomic`: unchanged (every operation is one GIL-held C function with no callback, hence atomic;
  atomic-add-float's 100 goroutines and the `integer overflow` panic are already right).
- `with_lock` and `once_do` call Ant code while holding a sync object's state: that is correct, the GIL may move
  during `f`, and `with_lock` already unlocks before propagating the panic.

## 4. `Time.sleep` (`runtime/io.c`)

`INT(argv[0], ms); if (ms <= 0) return ANT_VOID; ant_gil_release(); nanosleep loop (EINTR); ant_gil_acquire();
return ANT_VOID;`. Nothing between release and acquire touches an `AntValue` or `ant_g` (the argument is already
a C integer). sleep-blocks-only-caller: the sleeper releases the GIL for 500 ms, the worker and main run,
`["worker", "main", "sleeper"]`. `Os.exec` keeps the GIL during `waitpid` (antc has no goroutines; note it in the
report as a possible later release).

## 5. Memory-safety audit (§9 and §12 "a data race cannot corrupt memory")

Under the GIL every runtime operation is atomic with respect to other goroutines; the one hazard is a native that
caches a pointer into an array buffer, calls back into Ant (safepoint inside), and then uses the pointer with a
length read before the callback. Rule: between two callbacks, re-read `a->len` and index through the live
`a->data` (`xs->data[i]` with `i < xs->len` in the same GIL-held stretch); never hold `AntValue *p = a->data`
across a callback. Because buffers only grow, an index below any length ever observed is inside the buffer
`a->data` points at now, so `a_sort_by`'s final loop (`xs->data[o->data[i].as.i]` with indices below the old
length) is safe; `sorted_copy` must copy the elements into its private buffer before the first comparison and
sort that copy (check; fix if it indexes the live array between comparisons). Walk every `ant_call(` and
`ant_call_truth(` site listed in section 0 and confirm the rule; `m_map`/`m_filter` snapshot the entries first
and `ant_for_each` reads `a->len`/`a->data` per iteration, which is the pattern. Values read stale are allowed
(§9: "not any particular result"). race-memory-safe itself only races `Array.push` against `Array.get` and
needs no callback at all.

## 6. Order of work

1. `alloc.c` helpers; `sched.h`; `sched.c` per section 2 (move `ant_g` there); `ant.h` declarations and the
   real `ANT_SAFEPOINT`; `unwind.c` `run_main_thread`; `func.c` safepoints. `make runtime` clean under
   `-Wall -Wextra`. At this point a program without goroutines behaves exactly as before: check
   `bin/antc` is still built by `make fixpoint` (runtime rebuilt, stage1 under antpy ~6 s, stages 2/3, `cmp`) and
   run `--filter /smoke/` as a sanity check.
2. `sync.c` Mutex + WaitGroup + `Time.sleep`: `go-void-immediate`, `go-waitgroup-mutex-collect`, `mutex-counter`,
   `go-loop-var-per-goroutine`, `go-nested`, `waitgroup-zero`, `sleep-blocks-only-caller`, `go-main-exit-cuts`,
   `goroutines-preemptive`, `prog-parallel-sum`, `race-memory-safe`.
3. Panic isolation (trampoline report, the invariant of 2.4): `goroutine-panic-isolated`,
   `goroutine-panic-defers-run`, `mutex-withlock-returns-and-releases`, `go-panic-in-main-after-wait`.
4. `Once`, `Cond`, `RwMutex`: `once-do`, `once-panic-in-f`, `cond-producer-consumer`, `prog-producer-consumer`,
   `rwmutex-ops`.
5. Keep green what is green today (no blocking in them): `atomic-ops`, `atomic-add-float`,
   `sync-identity-eq-render`, the eight `panic/concurrency` tests, `unchecked/concurrency/lock-wrong-kind`.
6. Section 5 audit. Then the acceptance command from the repository root with output to `work/2.6/test.log`
   (about 2 minutes; run it in the background anyway and wait). Then the 31-test subset five more times in a row
   (flakiness; 0.4 did the same) and once while the 2.5 acceptance (the `--exclude` form of the same command,
   ~4 min, to `work/2.6/regression.log`) runs in parallel for load. Optional: `--consistency` on the subset.

Use `bin/antc run <file>` by hand on a single test when debugging; a hang is a deadlock in the scheduler or a
missed wake-up: `sample <pid> 1` shows every thread's wait site.

## 7. Spec rules that are easy to get wrong

- `go` yields `void` and returns at once; the body may run before or after `main`'s next statement (the tests
  gate with WaitGroups). `return` inside a `go` block leaves the block only (G24; the closure epilogue does this
  already); `break`/`continue` there are checker errors.
- A goroutine panic: its deferred blocks run first, then `panic: <msg>` and the trace go to stderr, the program
  continues, exit status unaffected; a `main` panic exits 2 after the report. Panics never cross goroutines.
- The program ends when `main` returns "whatever other goroutines are doing": no join, no grace period; same
  for `Os.exit` from any goroutine, with no deferred blocks run (G39).
- Preemption is mandatory (§12): `while true { }` in eight goroutines must not keep `main` from printing.
- `Mutex` is not owned: `Cond.wait`'s `not locked` means the mutex is free; `unlock` by another goroutine is
  fine. `Cond.wait` must release, wait, reacquire in that order, and must work with two conds on one mutex.
- `WaitGroup.add` may be negative as long as the counter stays at or above zero; `done` at zero and `add` below
  zero are both `negative counter`; waiters wake when the counter reaches zero through either call.
- `Once.do`: concurrent callers wait for the first `f` to finish; a panicking `f` still counts as done (G30).
- `with` forms return `f`'s value and release even when `f` panics; the panic survives the release.
- `Atomic.add` returns the new value and panics like `+` (`integer overflow`, `kind mismatch`, floats work);
  `compareAndSwap` uses structural `==` (G40); sync objects compare by identity and render as their namespace
  name (already implemented; do not break it when the structs change).
- `Time.sleep` blocks only the caller; non-positive durations return at once.
- The canonical phrases are substrings: `wrong kind: expected a Mutex` is fine, `not locked`, `negative counter`
  must stay verbatim.

## 8. Risks

1. **Starvation or unfairness**: a spinner holding the GIL with `main` queued. Mitigated by FIFO hand-off plus
   the waiter's 5 ms request; verify with goroutines-preemptive repeated under load (step 6.6).
2. **Lost panic report**: `main` exits before a goroutine's report. Mitigated by reporting before releasing the
   GIL and by never adding a safepoint between the last deferred block and the report (2.4).
3. **Boehm with threads**: every thread must come from the gc.h redirect in alloc.c (`ant_thread_spawn`), the
   goroutine struct must be scanned (uncollectable + the `all` list), and `gil.m` must never be held while
   allocating. A crash inside `GC_*` means one of these was violated.
4. **A sync call outside the GIL**: anything touching `ant_g`/`AntValue` after `ant_gil_release` (only
   `Time.sleep` releases). Keep the released stretch to the syscall.
5. **Deadlock in the scheduler itself** shows as a 60 s timeout; the two classic bugs are testing a predicate
   after `handoff()` (test under the GIL, before `ant_block_on`) and signalling a cv without first changing the
   state the waiter re-tests.
6. **Timing**: `pthread_cond_timedwait` needs an absolute CLOCK_REALTIME deadline (or use
   `pthread_cond_timedwait_relative_np`); 5 ms intervals keep goroutines-preemptive well under its 60 s.
7. **The fixpoint**: no emitter change is planned; if one becomes necessary, it must be deterministic, and
   `make fixpoint` must still `cmp` clean.
8. **Regression outside the step**: `ANT_SAFEPOINT` and the global `ant_g` touch every program; the 2.5
   acceptance run in 6.6 catches it. The compiler compiling itself (0.06 s of antc) should not change measurably.

## 9. Deviations from `docs/runtime-design.md` to record in the report

- `ant_g` is a global written at every hand-off, not `__thread` (section 2.2 gives the reason and the rule).
- No ticker thread: the queued waiter requests the switch after 5 ms (generation-checked); same 5 ms
  granularity, one thread fewer, and nothing to start lazily.
- Direct FIFO hand-off through per-goroutine condvars instead of a broadcast on `switch_cv` + generation wait.
- Goroutine stacks 64 MB (design: 1 MB); virtual only.
- No deadlock detection: a program whose goroutines all block hangs (the runner's timeout); the stub's
  `deadlock` panic was a single-threaded artefact.
- `Cond` uses wake-up counters so `signal` releases exactly one waiter.
