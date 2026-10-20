# 0.G bootstrap gate: report (round 2)

## What was built

The gate step has no plan: its deliverables are the acceptance command
(every tier except `check`, with `--consistency`), a refactor pass over
`bootstrap/`, and `bootstrap/README.md`. Round 1 left the acceptance
passing but delivered neither the README nor the refactor; this round adds
both.

- `bootstrap/README.md` (new): the command-line contract (`run
  [--unchecked] FILE [ARGS...]`, exit 0 / 2 / 1 / n and what each writes to
  stderr), the module layout with one line per module of
  `bootstrap/antpy/`, the design decisions the roadmap's M0 paragraph names
  as built (kind by exact Python type, control flow as `BaseException`
  subclasses, defer lists owned by the blocks the lowering pass marks,
  goroutines as threads under the GIL with `setswitchinterval(0.005)`, exit
  through `os._exit`, rendering per G33/G55, the raised recursion limit and
  the 512 MiB thread stack), how to run the suite (the 0.G acceptance
  command verbatim and the `--tier`/`--filter`/`--exclude`/`-v`/`--consistency`
  options), the known limitations collected from `work/0.*/report.md`, and
  that `antpy` leaves the loop after step 2.5.
- The refactor below.

## Acceptance

```
python3 -I src/tests/run.py --run "python3 -I bootstrap/antpy.py run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run --unchecked {file} {args}" --consistency --tier run --tier panic --tier unchecked --tier programs --tier io
```

Last line, after the refactor (`work/0.G/test.log`):

```
445 checks, 0 failures, 0 warnings (implementation mode, 444 tests)
```

Exit status 0. `ruff check --select F401,F402,F821,F841 bootstrap/` is clean
(the F402 shadowing at the old `interp.py:627` and `:747` is gone).

## Refactor

The five behaviour-preserving changes the round-1 reviewer listed, applied
one or two at a time with a `--filter` subset after each and the full
acceptance command at the end. `git diff --stat`: 5 files, 64 insertions,
82 deletions. No test, message, or evaluation order changed.

1. **One `need_bool`.** `bootstrap/antpy/arith.py` gains `need_bool(v)`
   next to `not_` (panics `not a bool`, returns `v`); `not_` is now
   `not need_bool(v)`. `interp.py` uses it in `ev_And` (both operands),
   `ev_Or` (both), `ev_If`, and `ev_While`, which removes the six inline
   copies. `library.need_bool_result` is deleted and its eleven call sites
   (`Array.filter/find/some/every`, `Map.filter`, `Iter.range/fromArray/
   map/filter/take`) call `arith.need_bool`; `library.py` already imported
   `arith`. Subset: `--tier run --tier panic --tier unchecked --filter
   /expressions/ --filter /stdlib/ --filter /smoke/`: 167 checks, 0 failures.
2. **One `need_callable`.** A module-level `need_callable(fn)` in
   `interp.py` (panics `not a function`, returns `fn`) replaces the three
   copies in `ev_Call`, `ev_OptCall`, and `ev_Pipe`; each still checks the
   callee before `eval_args` runs, as section 12 requires. `Interp.call`'s
   trailing `panic("not a function")` (reached only by a library callback
   that is not a function, e.g. an unchecked `Array.map(xs, 1)`) is the same
   helper, so the message lives in one place.
3. **No shadowing.** The loop variables named `entry` in `ev_RecordLit` and
   `report_panic` shadowed the `entry` function imported from `library`
   (ruff F402); they are `item` and `line` now. Subset for 2 and 3:
   `--filter /functions/ --filter /nullability/ --filter /types/ --filter
   /errors/`: 165 checks, 0 failures.
4. **Dead `Result` tag.** `prelude.TAG_NAMES` no longer lists `"Result"`:
   `make_prelude` bound a `Tag("Result")` and then rebound the name to the
   `Result` namespace record in the `NAMESPACES` loop, so the tag was never
   observable. The comment says why `Result` is not a tag.
5. **One `with_held`.** `sync.py` gains `with_held(interp, frame, lock,
   unlock, m, f)`: check `f`, lock `m` through the namespace's own lock
   function (which checks `m`'s kind before anything is acquired), call `f`
   with no argument, unlock in `finally`. `mutex_with_lock`,
   `rwmutex_with_read`, and `rwmutex_with_write` are one-line delegations to
   it with their own lock/unlock pair; the module docstring points at it
   instead of describing the shape. Subset for 4 and 5: `--tier run --tier
   panic --tier unchecked --tier programs --filter /concurrency/ --filter
   /errors/ --filter /types/ --filter race-memory-safe --filter parallel-sum
   --filter producer-consumer`: 152 checks, 0 failures.

The probe `x is Result` (unchecked) still panics `not a tag` after change 4,
as before: the name was already the namespace record.

## Decisions and deviations

- `with_held` keeps the reviewer's parameter order `(interp, frame, lock,
  unlock, m, f)`, so the three `with*` natives stay as named one-line
  functions rather than `functools.partial` objects (a partial over that
  order would need keyword binding and read worse in the registry).
- `Interp.call` ends in `need_callable(fn)` rather than a bare `panic`, as
  the reviewer asked; a comment says which callers reach it.
- The README's "After M2" paragraph is worded against the roadmap rows:
  step 2.5 makes `bin/antc` native, every later acceptance uses it, `make
  stage1` still runs `antc` under antpy, and 4.1 only refactors `bootstrap/`.

## Test changes

None. `work/TEST-CHANGES.md` is unchanged.

## Known limitations

As listed in `bootstrap/README.md` (lone-surrogate strings, Python's
`Json.parse` messages, unspecified map and `File.list` order, `x is Result`
unchecked panics `not a tag` where G17 would read `false`, deep recursion
through library callbacks uses the C stack, memory safety under races rests
on the GIL, `Cond.wait` cannot tell who holds the mutex, `func main(): int`
waits for the checker).
