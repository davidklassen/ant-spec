# Report for step 0.4 "antpy-concurrency"

## Result

Acceptance command (verbatim from the roadmap row) exits 0; its last line:

    379 checks, 0 failures, 0 warnings (implementation mode, 378 tests)

Full output in `work/0.4/test.log`. The same command with `--consistency`
also reports `379 checks, 0 failures` (`work/0.4/consistency.log`). The
31-test concurrency subset (`--filter /concurrency/ --filter race-memory-safe
--filter parallel-sum --filter producer-consumer`) was run five times in a
row after the runtime changes and three more times while the full suite ran
in parallel for load: eight runs, 0 failures each. `ruff check --select
F401,F821,F841 bootstrap/` is clean.

Baseline before the step: 379 checks, 13 failures, all `unbound name` for
`Atomic`, `Cond`, `RwMutex`, `Once`, `Time`.

## What was built

New module `bootstrap/antpy/sync.py` (the six sync objects of section 11 and
`Time.sleep` from stdlib.md), wired into the prelude; `Mutex` and `WaitGroup`
moved there from `values.py`/`prelude.py` unchanged. Three small robustness
changes to the goroutine runtime in `interp.py`, `library.py`, `equal.py`.

Files touched: `bootstrap/antpy/sync.py` (new), `values.py`, `prelude.py`,
`__init__.py`, `interp.py`, `library.py`, `equal.py`. Nothing outside
`bootstrap/` and `work/0.4/`.

### sync.py

- Every class is registered as kind `"sync"` after its definition (the trick
  `equal.py` uses for `AntMap`), so a sync value compares by identity and
  renders as its class name; the class names are exactly the namespace names.
- `Mutex`: a `threading.Lock`; `unlock` on a free lock is `not locked`. No
  owner tracking (Go semantics: another goroutine may unlock it).
- `RwMutex` (G51): `readers` count and `writer` flag under one
  `threading.Condition`. Readers share; a writer waits for the writer and
  every reader; `readUnlock` with no readers and `writeUnlock` with no writer
  both panic `not locked`, so the "wrong mode" unlocks of the two panic tests
  are covered. No writer preference.
- `WaitGroup`: unchanged; `done` is `add(wg, -1)`.
- `Once` (G30): `do` holds `once.lock` while `f` runs and sets `done` in a
  `finally`, so callers arriving during `f` wait for it and a panicking `f`
  counts as done.
- `Cond`: own waiter list (a deque of acquired `threading.Lock`s guarded by a
  small lock), not `threading.Condition`, because its `notify` requires the
  lock while section 11 gives `signal`/`broadcast` no panic. `wait` checks the
  mutex is held (`not locked` otherwise), registers its waiter before
  releasing so a signal cannot be lost, blocks on the waiter, then reacquires
  the mutex before returning. `signal` wakes the oldest waiter; `broadcast`
  all. No spurious wakeups.
- `Atomic`: a value under a lock. `add` goes through the `+` operator
  (`arith.binary`), so an int64 overflow is `integer overflow`, a delta of
  another kind `kind mismatch`, and floats work; it returns the new value.
  `compareAndSwap` compares with structural `eq` (G40).
- `Time.sleep(ms)`: `need_int`, `time.sleep(ms / 1000)` when positive (the
  GIL is released, so only the caller blocks). `Time.now`/`Time.monotonic`
  are left for 0.6, as the plan says.
- The `with` forms (`withLock`, `withRead`, `withWrite`) and `Once.do` are
  `ctx=True` natives calling `interp.call(f, [], (), frame)` and release in a
  `finally`, so a panic in `f` propagates after the release.

### Runtime changes

1. `Interp.report_panic` builds the whole report (message plus trace lines)
   and writes it with one `sys.stderr.write`, so two goroutines panicking at
   once cannot interleave their lines.
2. Bounded exit wait for unwinding goroutines. `Interp.unwinding` is a set of
   thread idents; the innermost `run_stmts` catch (which runs before any
   deferred block) adds the current ident when it fills a panic's trace;
   `run_goroutine` and `run_main` discard it in a `finally`, after the report
   is written. `run_program` constructs the interpreter before the worker
   thread and, after joining it, waits up to `UNWIND_GRACE = 1.0` s while the
   set is non-empty. This makes the four `stderr:`-checked goroutine-panic
   tests deterministic: their `defer { WaitGroup.done(wg) }` releases main
   before the report is written, and main could otherwise reach the exit
   first under load. A sleeping or spinning goroutine is not unwinding, so
   `go-main-exit-cuts` and `goroutines-preemptive` still exit at once.
3. Data-race guards (section 12 "Goroutines": a data race cannot crash the
   interpreter). A Python `IndexError`/`RuntimeError` escaping the evaluator
   would be reported as an internal error and skip the goroutine's deferred
   blocks (hanging a `WaitGroup.wait` in main). The sites where an index is
   used after a length check now catch `IndexError`: `Interp.index`,
   `ev_AssignIndex`, the array branch of `ev_For`, `library.array_get`,
   `array_pop`, `iter_from_array`. `AntMap.items` snapshots the bucket list
   before iterating (a concurrent insert of a new bucket would raise
   "dictionary changed size during iteration"), and `AntMap.remove` returns
   `MISSING` if a racing removal already deleted the pair or bucket. Single-
   goroutine behaviour is unchanged: `xs[i] += f()` where `f` shrinks the
   array still panics `index out of range`.

## Decisions and deviations from the plan

- Followed the plan's design throughout. Small simplifications: `Cond.wait`
  has no `try/finally` around the waiter's `acquire` (a `Lock.acquire` cannot
  raise, so the mutex reacquire follows it directly); `signal`/`broadcast`
  release the waiter locks inside the guard (a release never blocks); one
  `need_sync(v, cls)` helper builds the `wrong kind: expected a <Class>`
  message from the class ("an Atomic"; the runner checks only the `wrong
  kind` prefix).
- The exit grace period is a named module constant `UNWIND_GRACE` rather than
  a literal, so a later milestone can find it.
- No test was modified. `work/TEST-CHANGES.md` was not touched.

## Untested choices, probed (`work/0.4/probes/*.ant`)

| Probe | Result |
|---|---|
| `Atomic.add` on an `Atomic` holding a string | `panic: wrong kind: expected a number` |
| `Atomic.add(Atomic.new(1), 1.0)` | `panic: kind mismatch` |
| `Atomic.compareAndSwap` on arrays with equal contents | swaps (`true`, then `[9]`) |
| `Cond.signal`/`broadcast` with no waiters and the mutex free | no panic |
| `Cond.wait` while another goroutine holds the mutex | wait releases that goroutine's lock, is signalled, reacquires; the other goroutine's later `unlock` succeeds (no owner tracking). A data race; unspecified |
| `RwMutex`: a reader arriving while a writer waits | admitted (no writer preference); the writer runs after both readers leave |
| `Once.do` recursive on the same object | deadlocks (5 s timeout), as in Go |
| `Mutex.unlock` from a goroutine other than the locker | allowed; main relocks afterwards |
| `Time.sleep(0)`, `Time.sleep(-5)` | return at once; `Time.sleep(1.0)` is `wrong kind: expected an int` |
| A goroutine whose deferred block blocks forever after a panic | the program exits 0 when main returns, after the 1 s grace (1.14 s wall); the report is never written |

## Known limitations

- Memory safety under data races rests on the GIL; this build
  (Python 3.15.0) has it enabled (`sys._is_gil_enabled()` is `True`). A
  free-threaded build would invalidate the argument.
- A goroutine that panics and then blocks forever in a deferred block delays
  the exit by up to `UNWIND_GRACE` (1 s).
- `Cond.wait` cannot tell who holds the mutex; "not locked" means free. A
  wait on a mutex held by another goroutine releases it (a data race).
- Thread stacks are 512 MB (virtual, lazily committed) process-wide; 100
  goroutines pass on this machine. If thread creation ever fails, the plan's
  fallback is a smaller `threading.stack_size` set after the worker thread
  starts.
- `goroutines-preemptive` depends on the GIL switch interval (5 ms, set
  explicitly in `run_program`).

## Refactor

Single-pass review of the step's working-tree diff (the `simplify` recipe
run inline: reuse, simplification, efficiency, altitude), then the two
reviewer suggestions. Only `bootstrap/antpy/sync.py` changed; nothing else
in `bootstrap/` was touched. Added probe
`work/0.4/probes/withlock-unlock-inside.ant`.

Simplify pass: no edits beyond what the reviewer suggestions cover. The
per-site `IndexError` guards are at the right altitude (each site wants a
different outcome: panic, `null`, or stop iterating, so one central catch
could not pick the message); `report_panic`, the exit grace loop, and
`Cond.wait`'s waiter removal were judged clear enough as written. The only
reuse finding was `mutex_with_lock` re-implementing `Mutex.lock`/`unlock`
inline, which is suggestion 1.

1. `mutex_with_lock` releases in its `finally` through `mutex_unlock(m)`,
   so a body that unlocks the mutex itself panics `not locked` instead of
   escaping as a Python `RuntimeError` (previously reported as
   `panic: internal interpreter error`, which skipped the goroutine's
   deferred blocks and could leave main hanging in `WaitGroup.wait`).
   Probe: `Mutex.withLock(m, { Mutex.unlock(m) })` inside a goroutine now
   prints `panic: not locked` with the goroutine trace, the `defer` runs,
   and main returns with exit 0.
2. `rwmutex_with_read` and `rwmutex_with_write` no longer call `need_sync`
   themselves: the lock call does the kind check before anything is
   acquired. `mutex_with_lock` was given the same shape (`need_func(f)`,
   then `mutex_lock(m)`), so the three `with` forms are structurally
   identical: check `f`, lock through the namespace's own lock, call,
   unlock through the namespace's own unlock in a `finally`. With both
   arguments of the wrong kind the message is now `wrong kind: expected a
   function` rather than `... expected a Mutex`; `wrong kind` is the
   canonical phrase, so the suffix is never compared. The module docstring
   describes the shared shape.

After each change the 31-test concurrency subset reported 32 checks,
0 failures. The full acceptance command after both changes exits 0 and ends
with `379 checks, 0 failures, 0 warnings (implementation mode, 378 tests)`
(`work/0.4/test.log`). `ruff check --select F401,F821,F841,F811 bootstrap/`
is clean.
