# Report for step 2.6 "rt-concurrency"

Row: GIL scheduler, goroutines, sync objects, `Time.sleep`. Plan: `work/2.6/plan.md`; design:
`docs/runtime-design.md` section 3. Runtime-only change: `src/antc/` and `bootstrap/` are untouched (the
`Makefile`'s one change, from round 2, is a header dependency), so the C that `antc` emits is byte-identical
to step 2.5's and the fixpoint holds by construction. Round 2 (the section at the end) answers the "red after
refactoring" feedback.

## What was built

| File | Change |
|---|---|
| `runtime/sched.c` | rewritten: the GIL (`gil.m`, `holder`, FIFO queue), `wait_turn` with the 5 ms switch request (timed by the queue head only, since round 3), `handoff`, `ant_gil_acquire/release/yield`, `ant_block_on`, `ant_cond_*`, the goroutine trampoline, `ant_go`, `ant_sched_enter_main`; `ant_g` and `ant_switch_requested` live here |
| `runtime/sched.h` | new, private (`sched.c`, `sync.c`): `ant_block_on`, `ant_cond_init/signal/broadcast` over `<pthread.h>` |
| `runtime/sync.c` | every blocking object starts with a `Monitor { AntSync; pthread_cond_t }`; waits are `while (!predicate) ant_block_on(&cv)`; every state change that can satisfy a predicate signals (Mutex) or broadcasts (RwMutex, WaitGroup, Once); `Once` has NEW/RUNNING/DONE; `Cond` releases, waits with wake-up counters, re-acquires; `deadlock()` is gone; kind checks, panics, `with_lock`, `Atomic` unchanged |
| `runtime/ant.h` | the real `ANT_SAFEPOINT()` (a relaxed atomic load of `ant_switch_requested`, then `ant_gil_yield()`), declarations for the scheduler and the allocation helpers |
| `runtime/alloc.c` | `ant_alloc_uncollectable` (`GC_MALLOC_UNCOLLECTABLE`), `ant_free` (`GC_FREE`), `ant_thread_spawn` (detached, explicit stack size, created through Boehm's `pthread_create` redirect, so every goroutine thread is registered with the collector) |
| `runtime/unwind.c` | `ant_g`'s definition moved to `sched.c`; `run_main_thread` takes the GIL first (`ant_sched_enter_main`) and exits the process itself, from the thread that holds the GIL (`ant_exit(0)`, or the report and `ant_exit(2)`) |
| `runtime/io.c` | `Time.sleep` releases the GIL around `nanosleep` and re-acquires it; nothing in between touches an `AntValue` |
| `runtime/func.c` | `ANT_SAFEPOINT()` at the top of `iterate()` and `yield_code()`, so `for` bodies (closures without an entry safepoint) are preemptible |
| `Makefile` | (round 2) the object rule depends on `$(wildcard runtime/*.h)` instead of `runtime/ant.h` alone, so an edit to the new `runtime/sched.h` rebuilds `sched.c` and `sync.c` in an incremental build |

### How the scheduler works

One OS thread per goroutine; one global lock (the GIL) that a thread must hold to run Ant code or touch an
`AntValue`. The pthread mutex `gil.m` protects only the scheduler's fields and the sync objects' condition
variables and is never held while running Ant code or allocating, so it never nests with the collector's
lock. Hand-off is direct and FIFO: the releaser dequeues the next holder, clears the switch request and
signals that thread's own condition variable, and wakes the new head of the queue, if any, so that it starts
its timer; a releaser can therefore never re-acquire ahead of a waiter. Only the head of the queue waits on
its condition variable in 5 ms slices; when a slice ends without it having become the holder it sets
`ant_switch_requested`, which the holder sees at its next safepoint (function entry, `while` back-edge, every
`for` iteration) and yields: it hands off and re-acquires behind the queue. Every other waiter sleeps untimed
until a hand-off makes it the head or the holder. Blocking natives call `ant_block_on(cv)`: hand off, `pthread_cond_wait(cv, &gil.m)`,
then take the GIL again (at once when it is free and the queue is empty, else queued). The predicate is
tested under the GIL and can only change once another goroutine holds the GIL, which it gets inside
`ant_block_on` under `gil.m`, after the caller is parked: no lost wake-ups. A goroutine's thread reports its
pending panic while it still holds the GIL and releases only afterwards, so `main`, woken by the usual
`defer { WaitGroup.done(wg) }`, cannot end the process before the report exists. `main` exits the process
from its own thread while holding the GIL (`fflush` + `_exit`), so no other goroutine is mid-operation.

## Decisions and deviations

From `docs/runtime-design.md` (all anticipated by the plan's section 9 unless marked):

1. **`ant_g` is a plain global**, not `__thread`. The scheduler stores `ant_g = &holder->g` under `gil.m`
   whenever a thread becomes the holder, and nothing reads it without holding the GIL (the only code that
   runs without it is `nanosleep` in `Time.sleep`, the trampoline before `ant_gil_acquire`, and the scheduler
   itself). Generated code and natives keep reading one global instead of going through Darwin's TLS thunk
   at every `CHK`. The per-thread identity the scheduler needs is a separate `static __thread GoThread *self`
   that only `sched.c` reads.
2. **No ticker thread.** The waiter at the head of the queue requests the switch after 5 ms of its own waiting,
   unless that hand-off just made it the holder. Same granularity, one thread fewer, nothing to start.
3. **Direct FIFO hand-off through per-goroutine condition variables** instead of a broadcast on a shared
   `switch_cv` plus a generation wait; it is what makes a spinner unable to starve `main`.
4. **Goroutine stacks are 64 MB** (design: 1 MB), virtual and lazily committed; generated frames are large.
   100 goroutines reserve 6.4 GB of address space, which is fine on arm64 macOS.
5. **No deadlock detection.** A program whose goroutines all block hangs until the runner's timeout; the 2.5
   stub's `deadlock` panic was a single-threaded artefact.
6. **`Cond` uses wake-up counters** (`waiting`, `wakeups`): `signal` grants one wake-up only when an ungranted
   waiter exists, `broadcast` grants one to every current waiter, and a woken waiter leaves only if it can
   consume one. The number of waiters released is exact whatever pthread wakes; both use
   `pthread_cond_broadcast` underneath (the plan allowed `signal` as an optimisation; not needed).
7. **(plan deviation) No "all goroutines" list.** The plan kept a doubly linked list of `GoThread`s so the
   collector would see them. A `GC_MALLOC_UNCOLLECTABLE` object is itself scanned as a root by Boehm, so the
   `GoThread` (which holds the closure, `unwind.value`, the trace and the pair stack) keeps everything
   reachable on its own, and the running thread's stack keeps the `GoThread` pointer. The list, its lock
   discipline and the `next/prev` fields are gone.
8. **(plan deviation) No `ant_sched_init`.** `gil.m` and `main_thread.cv` use `PTHREAD_MUTEX_INITIALIZER` /
   `PTHREAD_COND_INITIALIZER`, so there is nothing to initialize at startup.
9. `Os.exec` keeps the GIL during `waitpid`: only `antc` uses it and `antc` has no goroutines. A later step
   can release the GIL around it the way `Time.sleep` does. (Superseded: see Refactor below.)
10. `Mutex` is not owned: any goroutine may unlock it, as the bootstrap and the spec's `Cond.wait` phrase
    (`not locked` means the mutex is free) imply.

## Acceptance

Command (from the repository root):

    make fixpoint && python3 -I src/tests/run.py --run "bin/antc run {file} {args}" --run-unchecked "bin/antc run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --tier programs --filter /concurrency/ --filter race-memory-safe --filter parallel-sum --filter producer-consumer

Last line (`work/2.6/test.log`, the final run of round 3 after the refactor below, exit 0; round 2's run
ended with the same line):

    32 checks, 0 failures, 0 warnings (implementation mode, 31 tests)

Robustness, round 2 (`work/2.6/repeat.r2.log`, `work/2.6/regression.r2.log`, `work/2.6/build.clean.log`):

- The 31-test subset ten times in a row while the 2.5 acceptance ran in parallel for load: ten times
  `exit=0 32 checks, 0 failures, 0 warnings (implementation mode, 31 tests)`, 28-31 s each.
- `--consistency` on the subset: `exit=0 32 checks, 0 failures, 0 warnings (implementation mode, 31 tests)`.
- The 2.5 acceptance in parallel with the above: `396 checks, 0 failures, 0 warnings (implementation mode,
  395 tests)`, exit 0.
- `make clean && make fixpoint`: exit 0, no compiler warning, `cmp` clean.

Robustness, round 1 (plan section 6.6):

- The 31-test subset five more times in a row while the 2.5 acceptance ran in parallel for load
  (`work/2.6/repeat.log`): every run `32 checks, 0 failures, 0 warnings`, 27-29 s each.
- `--consistency` on the subset: `32 checks, 0 failures, 0 warnings (implementation mode, 31 tests)`.
- Regression, the 2.5 acceptance (same command with `--exclude` instead of `--filter`, `work/2.6/regression.log`):
  `396 checks, 0 failures, 0 warnings (implementation mode, 395 tests)`, exit 0.
- `--filter /smoke/`: `6 checks, 0 failures, 0 warnings`.
- Hand timings (including the clang build of each program): `mutex-counter` (100 goroutines, 10,000 lock
  round-trips) 0.60 s, `goroutines-preemptive` 0.96 s, `once-do` 0.71 s, `sleep-blocks-only-caller` 1.52 s.

## Memory-safety audit (plan section 5)

Under the GIL every runtime operation is atomic with respect to other goroutines; the one hazard is a native
that caches a pointer into an array buffer across a callback. Walked every `ant_call` / `ant_call_truth` site:

- `array.c` `map`, `filter`, `reduce`, `find`, `some`, `every`: each loop re-reads `xs->len` in its condition
  and indexes `xs->data[i]` afresh after the callback; no pointer is held across a call. `sorted_copy` copies
  the elements into the result's private buffer before the first comparison and sorts that copy. `sortBy`
  computes the keys into a private buffer, sorts an index array, then reads `xs->data[idx]` with indices
  below the length it observed; buffers only grow (`array.c` never shrinks `cap`), so those reads stay inside
  the live buffer even if the array was popped meanwhile (stale values are allowed by section 9).
- `map.c` `map`, `filter`: iterate a snapshot of the entries. `iter.c`: `fromArray` re-reads `xs->len` per
  element; the stages pass values, not pointers. `result.c` `map`, `mapErr`: a single value.
- `func.c` `ant_for_each`: reads `a->len` / `a->data[i]` per iteration, `ant_map_slots` / `ant_map_entry` per
  iteration (dead slots skipped), strings are immutable. `sync.c` `with_lock` / `once_do` hold only the sync
  object's own state across `f`, by design.

`race-memory-safe` itself only races `Array.push` against `Array.get`, which are whole GIL-held operations.

## Test changes

None. No selected test contradicts the spec.

## Known limitations

- No deadlock detection (deviation 5): a fully blocked program hangs.
- ~~Every goroutine queued for the GIL wakes every 5 ms to decide whether to request a switch.~~ Removed in
  round 3 (below): only the head of the queue keeps the timer.
- A goroutine whose deferred block does more work after waking `main` (for example
  `defer { WaitGroup.done(wg); f() }`) can hand the GIL to `main` at `f`'s entry safepoint, and `main` may end
  the process before the goroutine's panic report. Section 12 allows it (the program ends "whatever other
  goroutines are doing"); the usual `defer { WaitGroup.done(wg) }` as the last deferred statement is safe
  because nothing between it and the report runs a safepoint.
- ~~`Os.exec` holds the GIL while the child runs (deviation 9).~~ Removed by the refactor below.
- The condition variable inside a sync object is never destroyed; on Darwin a `pthread_cond_t` holds no
  kernel resource, so collecting the object leaks nothing.

## Refactor

Behaviour-preserving cleanups after the review; the tree stayed green after each one (subset runs) and the
full acceptance command afterwards (`work/2.6/test.log`, exit 0,
`32 checks, 0 failures, 0 warnings (implementation mode, 31 tests)`).

- `runtime/alloc.c`: one static `start_thread(thread, fn, arg, stack, detachstate)` builds the pthread
  attributes for both `ant_thread_spawn` (detached) and `ant_run_on_big_stack` (joinable), instead of two
  copies of the attribute dance; the big-stack path now also destroys its attributes.
- `runtime/sched.c` `ant_go`: the `memset` after `ant_alloc_uncollectable` is gone; Boehm's non-atomic
  allocators return cleared memory (`gc.h`), which `sync_new` already relies on. `ant.h`/`alloc.c` document
  `ant_alloc_uncollectable` as zeroed.
- `runtime/sched.c` `wait_turn`: the timeout test is `rc == ETIMEDOUT && gil.generation == gen`; the former
  `gil.holder != me` was implied (the holder becomes `me` only through `handoff`, which bumps `generation`).
- `runtime/sched.c` `acquire_locked`: tests `!gil.holder` alone; the queue is empty whenever the holder is
  NULL (struct invariant), so the second half of the old double test was derivable.
- `runtime/sync.c`: `mutex_acquire(m)` / `mutex_release(m)` hold the Mutex wake-up discipline (wait on
  `mon.cv`, signal one waiter) in one place; `mutex_lock`, `mutex_unlock` and both halves of `cond_wait` use
  them instead of duplicating the bodies.
- `runtime/io.c` `os_exec`: the GIL is released around the `waitpid` loop and re-acquired after it, the same
  shape as `Time.sleep` (`pid` and `status` are C data; nothing in between touches an `AntValue`). This
  removes deviation 9 and the matching known limitation: a child process no longer holds every goroutine.

## Round 2: the "red after refactoring" verdict

The feedback for this round said the acceptance command was red after the refactor, and quoted
`32 checks, 0 failures, 0 warnings (implementation mode, 31 tests)` as its last line. That line is the green
summary (the runner exits 1 only when `failures` is non-empty, `src/tests/run.py` line 466), and every log of
the previous round (`test.log`, `test.impl.log`, `repeat.log`, `tester-repeat.log`, `regression.log`) ends
with the same line and exit 0. So the verdict was not reproducible from the tree itself; this round treated it
as a possible build or flakiness problem and tried to provoke one:

1. The acceptance command as left by round 1 (`work/2.6/test.r2a.log`): `make fixpoint` up to date, exit 0,
   `32 checks, 0 failures, 0 warnings (implementation mode, 31 tests)`.
2. From a clean tree (`make clean && make fixpoint`, `work/2.6/build.clean.log`): the runtime compiles without a
   warning under `-Wall -Wextra`, stage 1 under antpy, stages 2 and 3, `cmp` clean, exit 0. A fresh checkout
   builds; no untracked file is needed (`runtime/sched.h` is included by `sched.c` and `sync.c` only and is
   part of the step's diff).
3. Flakiness under load (`work/2.6/repeat.r2.log`): the 31-test subset ten times in a row with the 2.5
   acceptance (395 tests, `work/2.6/regression.r2.log`) running in parallel the whole time, then once more with
   `--consistency`. Twelve runner invocations, every one exit 0 with 0 failures; the regression ended
   `396 checks, 0 failures, 0 warnings (implementation mode, 395 tests)`. Together with round 1's five
   repeats, the subset has now passed 17 times in a row under load and never failed in this tree.
4. The timing-dependent tests were re-read for margins: `sleep-blocks-only-caller` and `go-main-exit-cuts`
   rest on a 500 ms sleep against work that takes microseconds; `goroutines-preemptive` needs about 8 x 5 ms of
   hand-offs against a 60 s timeout. Nothing in the 31 tests depends on a tight timing.
5. The scheduler was re-audited against the two classic bugs (a predicate tested after `handoff()`, a signal
   without a prior state change) and the panic-report invariant; no defect was found. The analysis is in
   "How the scheduler works" above and was not changed.

The one defect found is in the build, not the runtime: `Makefile`'s object rule listed `runtime/ant.h` as the
only header dependency, so an edit to the new `runtime/sched.h` would not have rebuilt `sched.c` and `sync.c`
in an incremental build (a clean build was unaffected). The rule now depends on `$(wildcard runtime/*.h)`.
Object timestamps are newer than the headers, so the change triggers no rebuild and the fixpoint is untouched.

The most plausible explanation for the harness's verdict, given that it cannot be reproduced, is outside the
tree: two runners executing the same test at the same moment share `antc build`'s temporary path when both
clocks and the entry agree to the millisecond (`src/antc/codegen/build.ant` `tempPath`), or a verdict taken
from a log whose last line was not the summary. Neither is a runtime behaviour, and `tempPath` is step 2.5's
documented design, so it was left alone.

Two untracked files at the repository root, `hello` and `hello.ant`, are a hand-written sample program that was
being compiled in this tree by another session while this round ran (their timestamps fall inside the stress
run). They are not part of the step and were left alone; the round-1 `git status` already listed them, and
they were gone again from `git status` by the end of this round.

## Refactor (round 3)

Behaviour-preserving changes after the second review. The 31-test subset was green after each one, and the
full acceptance command afterwards (`work/2.6/test.log`, exit 0: `make fixpoint` rebuilt the three stages,
`cmp` clean, `32 checks, 0 failures, 0 warnings (implementation mode, 31 tests)`). `src/antc/` and
`bootstrap/` are untouched, so the C that `antc` emits is unchanged and the fixpoint holds by construction.

Simplify pass (one inline pass over reuse, simplification, efficiency, altitude):

- `runtime/sched.c` `ant_gil_yield`: `handoff(); acquire_locked(self);`, the shape `ant_block_on` already
  has, instead of a hand-rolled `enqueue; handoff; wait_turn`. The queue order is identical either way: the
  old head is dequeued and the holder lands at the back.
- `runtime/sched.c` `wait_turn`: the 5 ms slice is `pthread_cond_timedwait_relative_np` with a static
  relative `timespec`, replacing the `clock_gettime(CLOCK_REALTIME)` deadline and its nanosecond carry. Darwin
  only, like the rest of the target; a wall-clock jump no longer stretches or shortens a slice.
- Reviewed and left alone: the two `pthread_cond_destroy` + `ant_free` sites in `trampoline` and `ant_go`
  (two lines each); `ant_cond_init` as the one seam between the sync objects and the scheduler's condition
  variables; the safepoints in `func.c`'s `iterate`/`yield_code`, which sit in the single driver of all four
  iteration kinds (and the emitted C must not change). The timed-wait cost is reviewer suggestion 1 below.

Reviewer suggestion 1, only the head of the queue keeps the timer:

- `handoff()` signals the new head of the queue after the dequeue, so it starts its timer; `wait_turn` does an
  untimed `pthread_cond_wait` while `gil.qhead != me` and the 5 ms timed wait only as the head. `gil.generation`
  became derivable and is gone: a hand-off always dequeues the head, so for the head "a hand-off happened
  during my slice" is exactly "I am now the holder", and the timeout test is `== ETIMEDOUT && gil.holder != me`.
- Probes (not tests): `work/2.6/probe-broadcast.ant`, 2000 goroutines parked on one WaitGroup and released by
  a single broadcast; `work/2.6/probe-serial.ant`, the same 2000 released one at a time through a Mutex
  chain. Each is one `bin/antc run`, including its clang build; `/usr/bin/time`, before and after:

  | probe | before (real / user / sys) | after |
  |---|---|---|
  | broadcast | 3.48 s / 0.43 s / 12.60 s | 1.20 s / 0.16 s / 0.37 s |
  | serial | 1.17 s / 0.14 s / 0.37 s | 1.22 s / 0.16 s / 0.40 s |

  The broadcast release now costs what the serial one does. `goroutines-preemptive` takes 0.71 s (round 1
  measured 0.96 s).

Reviewer suggestion 2, a defined cross-thread flag:

- `ant_switch_requested` is a plain `int` accessed only through `__atomic_load_n` (in `ANT_SAFEPOINT`) and
  `__atomic_store_n` (`request_switch()` in `sched.c`, used by `handoff`, `wait_turn` and `ant_gil_yield`),
  both `__ATOMIC_RELAXED`: the `volatile` data race is gone and the poll stays a plain load on arm64.
  `_Atomic int` was tried first and dropped: `<stdatomic.h>` defines `atomic_load`/`atomic_store` as macros,
  which capture `sync.c`'s natives of those names, and clang refuses the `__atomic_*` builtins on an `_Atomic`
  pointee, so the header would have had to reach every generated translation unit.

Regression after all three changes, since `ant.h`'s safepoint macro reaches every generated function: the 2.5
acceptance subset (`--exclude` instead of `--filter`, `work/2.6/regression.r3.log`) ended
`396 checks, 0 failures, 0 warnings (implementation mode, 395 tests)`, exit 0. `make runtime` and the
acceptance's clang runs produced no warning.
