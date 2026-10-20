# Plan for step 0.4 "antpy-concurrency"

## 1. Scope and baseline

Acceptance (verbatim):

    python3 -I src/tests/run.py --run "python3 -I bootstrap/antpy.py run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --tier programs --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /functions/ --filter /expressions/ --filter /types/ --filter /nullability/ --filter /errors/ --filter /stdlib/ --filter /concurrency/ --filter race-memory-safe --filter parallel-sum --filter producer-consumer

Baseline on the current tree: `379 checks, 13 failures, 0 warnings (implementation mode, 378 tests)`, 12 s wall. Every failure is a missing prelude name: `Atomic` (3), `Cond` (3), `RwMutex` (3), `Once` (2), `Time` (1), plus `sync-identity-eq-render` (needs all of them). Everything the roadmap row lists besides the new sync objects already works and passes: `go` as a daemon thread with a child scope (per-iteration loop variables), `Mutex` (`new`/`lock`/`unlock`/`withLock`), `WaitGroup` (all four, both `negative counter` panics), goroutine panic isolation with defers and the stderr report, preemption (`goroutines-preemptive`: 8 spinners), exit when `main` returns (`go-main-exit-cuts`), `race-memory-safe`, `prog-parallel-sum`.

So the step is: (a) the four missing sync objects and `Time.sleep`, in a module of their own; (b) three small robustness changes to the goroutine runtime so the stderr-checked tests are deterministic and a data race cannot crash the interpreter. Do not touch `run.py`, the tests, or anything outside `bootstrap/` and `work/0.4/`.

The GIL is on (`sys._is_gil_enabled()` is `True` on this Python 3.15 build); the memory-safety argument of the roadmap rests on it.

## 2. Files

| File | Change |
|---|---|
| `bootstrap/antpy/sync.py` | **new**: the six sync value classes, their `KIND` registration, the natives, the registries `MUTEX`, `RWMUTEX`, `WAITGROUP`, `ONCE`, `COND`, `ATOMIC`, `TIME` |
| `bootstrap/antpy/values.py` | remove the `Mutex` and `WaitGroup` classes, their two `KIND` rows, and the `threading` import (they move to `sync.py`) |
| `bootstrap/antpy/prelude.py` | remove the sync section (`mutex_lock` .. `wait_group_wait`) and the `Mutex`/`WaitGroup` imports; `NAMESPACES` gets the seven registries from `antpy.sync` |
| `bootstrap/antpy/interp.py` | `report_panic` writes one string; the "unwinding goroutines" set and the bounded exit wait in `run_program`; `IndexError` guards at the three list-index sites |
| `bootstrap/antpy/library.py` | `IndexError` guards in `array_get`, `array_pop`, `iter_from_array` |
| `bootstrap/antpy/equal.py` | `AntMap.items` snapshots the bucket list; `AntMap.remove` tolerates a racing removal |
| `bootstrap/antpy/__init__.py` | add `sync` to the module list in the docstring |
| `work/0.4/report.md`, `work/0.4/test.log` | the report and the acceptance log |

`render.py` needs no change: a sync value renders as `type(v).__name__`, so the class names must be exactly `Mutex`, `RwMutex`, `WaitGroup`, `Once`, `Cond`, `Atomic` (test `sync-identity-eq-render`). `equal.py` needs no change for identity: kind `"sync"` is in `REFERENCE_KINDS` (compared with `is`, hashed by `id`).

Import graph after the step (no cycles): `values` <- `equal` <- `arith`, `render` <- `library` <- `sync` <- `prelude` <- `interp`. `sync.py` imports `threading`, `time`, `collections`, `VOID`/`KIND`/`panic` from `values`, `need`/`need_int`/`need_func`/`need_number` from `library`, `eq` from `equal`, `arith`.

## 3. `sync.py`

Conventions, as in `library.py`: a native checks the kinds of its arguments first (`wrong kind`), then the documented value-dependent panics, then does the work. One helper `need_sync(v, cls)` = `need(v, cls, "a " + cls.__name__)` (use `an` for `Atomic`; the runner only checks `wrong kind`). Registries use section 11's parameter names so named arguments resolve: `m`, `f`, `wg`, `n`, `once`, `c`, `a`, `v`, `d`, `expected`, `ms`. Natives that call back into Ant (`withLock`, `withRead`, `withWrite`, `Once.do`) are `ctx=True` and call `interp.call(f, [], (), frame)`: the lambda sees `x == void` (G23). After the class definitions: `for cls in (Mutex, RwMutex, WaitGroup, Once, Cond, Atomic): KIND[cls] = "sync"`, the same trick `equal.py` uses for `AntMap`.

### Mutex (moved, unchanged)

`Mutex.lock` is a `threading.Lock`. `lock` acquires (blocks, GIL released, not reentrant, like Go). `unlock` releases; `RuntimeError` means it was free: panic `not locked`. No owner tracking: "not locked" means nobody holds it, and an unlock from another goroutine is allowed (Go semantics). `withLock`: acquire, `try: return interp.call(f, ...)` `finally: release`; `AntPanic` and `ReturnSignal` are `BaseException`, so `finally` covers them and the panic propagates after the release (test `mutex-withlock-returns-and-releases`).

### RwMutex (new; G51: readers share, a writer excludes readers and writers)

Fields: `cond = threading.Condition()`, `readers = 0`, `writer = False`. All five operations run under `with m.cond:`; a `panic` inside the `with` releases it.

- `readLock`: `while m.writer: m.cond.wait()`; `readers += 1`.
- `readUnlock`: `if m.readers == 0: panic("not locked")`; `readers -= 1`; `if readers == 0: notify_all()`.
- `writeLock`: `while m.writer or m.readers: wait()`; `writer = True`.
- `writeUnlock`: `if not m.writer: panic("not locked")`; `writer = False`; `notify_all()`.
- `withRead`/`withWrite`: lock, `try: return interp.call(f, ...)` `finally:` the matching unlock.

The panics are per mode: `readUnlock` on a write-locked mutex and `writeUnlock` on a read-locked one both say `not locked` (tests `rwmutex-read-unlock-wrong`, `rwmutex-write-unlock-wrong`). No writer preference (unspecified; the tests never need it). Test `rwmutex-ops`: main holds a read lock while a goroutine takes a second one, then a write lock after both are released.

### WaitGroup (moved, unchanged)

`count` + `threading.Condition()`. `add(wg, n)`: `need_int(n)`; under the condition, `count + n < 0` panics `negative counter`, else add and `notify_all` when it reaches zero. `done` is `add(wg, -1)` (so `done` at zero is `negative counter`, test `waitgroup-done-at-zero`). `wait` loops on `count > 0`. `add` with a negative `n` that keeps the counter at or above zero is fine (`waitgroup-zero`).

### Once (new; G30)

Fields: `lock = threading.Lock()`, `done = False`.

```
def once_do(interp, frame, once, f):
    need_sync(once, Once); need_func(f)
    with once.lock:
        if not once.done:
            try:
                interp.call(f, [], (), frame)
            finally:
                once.done = True        # a panicking f counts as done (G30)
    return VOID
```

Holding `once.lock` while `f` runs is what makes "later calls wait for it" true: in `once-do`, 19 goroutines block on the lock while the first caller's `f` waits on a gate, and every one of them sees `initialized` afterwards. In `once-panic-in-f`, the panic propagates out of the `with` (released), the goroutine reports `panic: init failed`, and main's two later calls return at once with `calls == 1`. `f`'s value is discarded. A recursive `Once.do` on the same object deadlocks, as in Go; record it as an untested choice.

### Cond (new): write it, do not wrap `threading.Condition`

`threading.Condition.notify` raises when the lock is not held, but section 11 gives `signal`/`broadcast` no panic (Go allows signalling without the lock), and dropping such a signal would lose a wakeup. The waiter-list algorithm is CPython's own, ~25 lines:

Fields: `mutex` (the `Mutex` from `Cond.new(m)`, checked with `need_sync`), `waiters = collections.deque()`, `guard = threading.Lock()`.

- `wait(c)`: `lk = c.mutex.lock`. `if not lk.locked(): panic("not locked")` (test `cond-wait-unlocked`: lock then unlock, then wait). Create `w = threading.Lock(); w.acquire()`; `with c.guard: c.waiters.append(w)` (register **before** releasing, so a signal sent while we hold the mutex cannot be lost); `try: lk.release() except RuntimeError:` deregister `w` and panic `not locked` (a racing unlock by another goroutine). Then `try: w.acquire()` (blocks until signalled) `finally: lk.acquire()` (reacquire before returning). Return `VOID`.
- `signal(c)`: `with c.guard:` pop the leftmost waiter if any; release it outside the guard.
- `broadcast(c)`: `with c.guard:` take all waiters and clear; release each.

No spurious wakeups, but the tests loop on their condition anyway. Two conds on one mutex share `c.mutex.lock` (`cond-producer-consumer`, `prog-producer-consumer`). "Locked" means held by anyone, consistent with `Mutex.unlock`; a `wait` by a goroutine that does not hold a mutex someone else holds is a data race the spec leaves unspecified (record it).

### Atomic (new)

Fields: `value`, `lock = threading.Lock()`. Every operation runs under the lock.

- `new(v)`: any value. `load(a)`: the value. `store(a, v)`: store, `VOID`.
- `add(a, d)`: `need_sync(a, Atomic)`, `need_number(d)`; under the lock `need_number(a.value)` (an `Atomic[string]` is an argument "of a kind the function does not take": `wrong kind`), then `new = arith.binary("+", a.value, d)`, store it, return it. Through the operator, an `int64` overflow is `integer overflow` (test `atomic-add-overflow`: `9223372036854775806 + 1` prints, the next add panics), different kinds are `kind mismatch`, floats never panic (`atomic-add-float`: 100 goroutines adding `0.25` give exactly `25.0`). **Returns the new value.**
- `compareAndSwap(a, expected, v)`: under the lock, `if eq(a.value, expected): a.value = v; return True` else `False`. Structural `==` (G40): an equal but distinct record swaps (`atomic-ops`).

Renders as `Atomic` without its value; compares by identity (`Atomic.new(1) == Atomic.new(1)` is `false`).

### Time

`TIME = {"sleep": (["ms"], time_sleep)}`: `need_int(ms)`; `if ms > 0: time.sleep(ms / 1000)`; `VOID`. `time.sleep` releases the GIL, so only the caller blocks (`sleep-blocks-only-caller`: worker, main, sleeper). A float `ms` is `wrong kind`; a non-positive count returns at once. `Time.now` and `Time.monotonic` are 0.6's (`io/time-now-monotonic-sleep` is their only test); do not add them here.

### prelude.py

`NAMESPACES` in section 11 order: `"Mutex": MUTEX, "RwMutex": RWMUTEX, "WaitGroup": WAITGROUP, "Once": ONCE, "Cond": COND, "Atomic": ATOMIC`, then `"Time": TIME`. `new` entries are `([], Mutex)` style (the class itself) except `Cond.new`, which is `(["m"], cond_new)` with the `need_sync(m, Mutex)` check. Drop the now-unused imports (`need`, `need_int`, `Mutex`, `WaitGroup`); `ruff check --select F401,F821,F841 bootstrap/` must stay clean.

## 4. `interp.py`: three runtime changes

### 4.1 One write per panic report

`report_panic` currently writes the message and each trace line separately. Build the whole report (`"panic: msg\n" + "  at ...\n" * n`) and write it with a single `sys.stderr.write` after `sys.stdout.flush()`. Two goroutines panicking at once then never interleave their traces, and the message line and its trace land in one syscall.

### 4.2 Exit waits (bounded) for goroutines that are already unwinding

Four tests (`goroutine-panic-isolated`, `goroutine-panic-defers-run`, `once-panic-in-f`, `mutex-withlock-returns-and-releases`) check `stderr: panic: ...` from a goroutine whose `defer { WaitGroup.done(wg) }` runs **before** the report (section 12: the report comes when unwinding finishes). After that `done`, main is runnable and can reach `os._exit` while the goroutine thread is still between its last deferred block and `sys.stderr.write`; each stdout flush on main's exit path releases the GIL. It passes today because the goroutine normally keeps the GIL for the switch interval, but under the runner's parallel load an OS deschedule makes it a flake. Make it deterministic:

- `Interp.__init__`: `self.unwinding = set()` (thread idents; `set.add`/`discard` are atomic under the GIL, no lock needed).
- `run_stmts`, in `except AntPanic as p:` when `p.trace is None` (the innermost catch, which runs **before** any deferred block): `self.unwinding.add(threading.get_ident())`. A replacing panic (G59) adds the same ident again, harmless.
- `run_goroutine` and `run_main`: wrap the body in `try/finally: self.unwinding.discard(threading.get_ident())`, so the ident leaves the set only after the report is written (and main's own ident never holds up exit).
- `run_program`: construct `interp = Interp(args)` before the worker thread (the worker calls `interp.run_main(path)`); after `thread.join()`, `deadline = time.monotonic() + 1.0; while interp.unwinding and time.monotonic() < deadline: time.sleep(0.001)`; then the flushes and return.

A sleeping or spinning goroutine is not unwinding, so `go-main-exit-cuts` and `goroutines-preemptive` still exit at once; the bound keeps "the program ends when main returns" true even when a panicking goroutine's deferred block blocks forever. Record the choice in the report.

### 4.3 A data race must not crash the interpreter (section 9, section 12 "Goroutines")

A Python `IndexError`/`RuntimeError` escaping the evaluator is reported as an internal error **and skips the goroutine's deferred blocks** (only `UNWINDING` exceptions run them), so a racing pop would hang main in `WaitGroup.wait`. Guard the sites where an index is used after a length check, nothing else:

- `Interp.index` (array branch): `if type(i) is not int: panic("index not an int")`; `if i >= 0: try: return obj[i] except IndexError: pass`; `panic("index out of range")`. The `i >= 0` test keeps Python's negative indexing out.
- `ev_AssignIndex` store: replace the `i >= len(obj)` re-check with `try: obj[i] = v except IndexError: panic("index out of range")` (keep `check_index_target` before the right side).
- `ev_For` array branch and `library.iter_from_array`: `try: v = it[i] except IndexError: break` (a removed element is not visited).
- `library.array_get`: same shape, returning `None`; `library.array_pop`: `try: return xs.pop() except IndexError: return None`.
- `equal.AntMap.items`: iterate `list(self.buckets.values())` (a concurrent insert of a new bucket otherwise raises "dictionary changed size during iteration"); `AntMap.remove`: `try/except (IndexError, KeyError)` around the two `del`s, returning `MISSING`.

Record fields and scope dicts never change size while visible to another goroutine (assignment requires an existing field; module names are hoisted before any code runs), so `eq`/`render` need nothing. `race-memory-safe` (writers push only) passes before and after; the guards are for the general case.

## 5. Order of work, with checkpoints

Define `A` as the acceptance command and `C` as `A` minus the `/smoke/ .. /stdlib/` filters (the 31-test concurrency subset, ~2 s). Run from the repo root.

1. **Pure move.** Create `sync.py` with `Mutex`/`WaitGroup` classes and natives moved verbatim; edit `values.py`, `prelude.py`, `__init__.py`. Checkpoint: `A` still reports exactly the 13 baseline failures, `ruff` clean.
2. **RwMutex.** Checkpoint: `C --filter rwmutex` (3 tests, 0 failures).
3. **Once.** `C --filter once-` (2 tests).
4. **Atomic.** `C --filter atomic` (3 tests); then `C --filter sync-identity` (needs all six classes and `Cond.new`; add `Cond.new` with a stub body if Cond is not yet written, or do step 5 first).
5. **Cond.** `C --filter cond- --filter producer-consumer` (3 tests incl. `programs/prog-producer-consumer`).
6. **Time.sleep.** `C --filter sleep-blocks`.
7. **interp changes (4.1, 4.2, 4.3).** Checkpoint: `C` run five times in a row (`for i in 1 2 3 4 5; do C | tail -1; done`), all `0 failures`; then the full `A` (`> work/0.4/test.log`), then `A --consistency`. The full run takes about 12 s, so no background needed.
8. Probe the untested choices listed in section 7 with one-line programs and record the results in `work/0.4/report.md`.

## 6. Spec rules that are easy to get wrong

- `not locked` means **free**, not "not held by me": no owner tracking anywhere (`Mutex.unlock`, `Cond.wait`, both `RwMutex` unlocks). `Cond.signal`/`broadcast` never panic.
- `Cond.wait` registers the waiter before releasing the mutex and reacquires it before returning; `Cond.new` takes the mutex and the panic is checked on **that** mutex's lock.
- `Once.do`: callers that arrive while `f` runs **wait** for it (they do not return immediately); a panicking `f` counts as done (G30); `f` runs with no argument.
- `Atomic.add` returns the **new** value and goes through the `+` operator (`integer overflow`, `kind mismatch`); it works on floats; `compareAndSwap` compares with structural `==` (G40) and returns a `bool`.
- Every sync object compares by identity and renders as its namespace name with no contents (`Atomic` renders `Atomic`), including inside templates.
- `WaitGroup.add` may be negative as long as the counter stays at or above zero; `done` at zero is `negative counter`; `wait` on zero returns at once.
- The `with` forms return `f`'s value and release in `finally`, so a panic in `f` propagates **after** the release and a later `lock` on the same mutex succeeds.
- `go` yields `void` immediately; its body is a child scope of the current scope, so each `for` iteration's fresh scope gives each goroutine its own loop variable (already so). A `return` in a `go` block leaves the block only (already so).
- A goroutine panic: defers run, then `panic: <message>` and the trace on stderr, the program continues, exit status unaffected; a `main` panic after waiting for workers still exits 2 with the workers' stdout intact.
- The program ends when `main` returns whatever other goroutines do: never join goroutines; the 4.2 wait is only for goroutines already past their `panic(...)` and is bounded.
- Library kind checks come first: `Mutex.lock("not a mutex")` is `wrong kind` (unchecked tier test), `Time.sleep(1.0)` is `wrong kind`.

## 7. Untested choices to probe and record

`Atomic.add` on an `Atomic` holding a non-number (`wrong kind`) and across kinds (`kind mismatch`); `Atomic.compareAndSwap` on arrays with equal contents (swaps); `Cond.signal` with no waiters and with the mutex free (no panic); `Cond.wait` while another goroutine holds the mutex (unspecified, data race); `RwMutex` readers admitted while a writer waits (no preference); `Once.do` recursive on the same object (deadlock, as Go); `Mutex.unlock` from a goroutine other than the locker (allowed); `Time.sleep(0)`/`Time.sleep(-5)` (return at once); a goroutine whose deferred block blocks after a panic (exit after at most 1 s).

## 8. Risks

- **Flaky stderr checks** are the main risk of this step; 4.1 + 4.2 address it. Verify with the five-run loop in step 7, ideally while the full suite runs in another terminal to add load.
- **Thread stacks**: `threading.stack_size` is 512 MB process-wide and `mutex-counter`/`atomic-add-float` start 100 goroutines each (51 GB virtual, lazily committed). It passes today on this machine. If thread creation ever fails ("can't start new thread"), set `threading.stack_size(64 * 1024 * 1024)` in `run_program` right after starting the worker thread (goroutines created afterwards get the smaller stack; the worker keeps 512 MB); the trade-off is that very deep recursion inside a goroutine could then overflow the C stack before `RecursionError`, which no test does.
- **Preemption under load**: `goroutines-preemptive` relies on the GIL switch interval (5 ms, the default; keep the explicit `setswitchinterval` call). It passes today with the runner's parallelism.
- **Deadlocks** in a wrong `Cond`/`RwMutex` show up as 60 s runner timeouts, not messages; use `C --filter <name> --timeout 10` while developing.
- **Python 3.15**: the GIL is on in this build; a free-threaded (`t`) build would invalidate the memory-safety argument. Note it in the report; nothing to do.
- **The pure-move step** touches `values.py`'s `KIND`; `render.py`'s `k == "sync"` branch and `equal.py`'s `REFERENCE_KINDS` depend on the registration running before any sync value exists, which holds because `prelude` imports `sync` at startup.
