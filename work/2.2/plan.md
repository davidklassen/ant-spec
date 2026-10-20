# Plan for step 2.2 "rt-types-errors"

Acceptance (verbatim): `python3 -I src/tests/run.py --timeout 600 --run "python3 -I bootstrap/antpy.py run
src/antc/main.ant run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run src/antc/main.ant run
--unchecked {file} {args}" --tier run --tier panic --tier unchecked --filter /smoke/ --filter /types/
--filter /nullability/ --filter /errors/` (143 tests, 144 checks: 103 run, 19 panic, 21 unchecked).
Follow `docs/runtime-design.md`; the runtime API and emitter are as `work/2.1/plan-v1.md` sections 2-3 and
`work/2.1/report.md` ("What was built", "Decisions and deviations") describe them.

## 0. What the planner found (verified 2026-10-21 on the committed tree; do not re-investigate)

- Step 2.1 already built every item of this row's deliverable, because its own tests needed them (every
  2.1 test imports `expect.ant`, which uses records, `==`, templates, `??`): records with tags and shapes
  (`runtime/record.c`, `symbol.c`), arrays (`array.c`), maps (`map.c`, the compact dict), `is`
  (`value.c` `ant_is_kind`, `record.c` `ant_is_tag`, emitter `emitIs`/`KIND_TESTS`), structural `==` with the
  pair stack and the consistent hash (`equal.c`), rendering with `...` for cycles and shortest round-trip
  floats (`render.c`), spread (`ant_record_build`, `ant_array_extend`), `?.` (`emitChain`/`nullSkip`), `??`
  (`emitNullish`), `try` (`func.c` `ant_try`, `emitUnary`), conversions (`number.c` `ant_convert`), replacing
  panics (`unwind.c` `ant_run_defers`), the prelude tags and `Result`/`Record` (`prelude.c`, `result.c`).
- The acceptance command passes as is: `144 checks, 0 failures, 0 warnings (implementation mode, 143 tests)`,
  exit 0 (`work/2.2/baseline.log`, about 6 minutes). `git status` is clean; `Os.exec` is in HEAD's
  `bootstrap/antpy/system.py` (nothing under `bootstrap/` needs to change in this step).
- 34 probe programs in `work/2.2/probes/` (runner `sh work/2.2/probes/run.sh [NN ...]`, which runs each probe
  under the bootstrap and under antc and prints `SAME`/`DIFF` over stdout + exit status + first stderr line)
  cover the corners of the deliverable the tests leave open. All agree except three:
  1. `01-spread-order`, `41-spread-dup-check-order`: a record literal whose spread operand is not a record.
     The bootstrap panics `not a record` at the spread's position, so later entries never evaluate; the
     runtime evaluates every entry first and `ant_record_build` checks afterwards. Section 12 ("Entries
     evaluate in order; a spread copies every field of a value that must be a record") and the array rule
     the runtime already follows (`ant_array_extend` checks in order) put the check at the spread. **Fix**
     (section 2). `work/2.1/report.md` lists this under "Known limitations" for 2.2 to settle.
  2. `37-cyclic-key`: a cyclic record or array used as a map key. The bootstrap panics `map key too deep
     (cyclic?)`; the runtime hashes with the depth cap of design section 1 ("recursion depth-capped at 8 ...
     so cyclic keys terminate") and finds the key. No spec row or README phrase names such a panic; keep the
     runtime's behaviour and record the divergence in the report.
  3. `06-conversions`, line `print(float32(3.4028235e38))` (a double above `FLT_MAX` that rounds to it): the
     runtime panics `conversion out of range` (`ant_convert`: `fabs(d) > FLT_MAX`, the literal reading of
     section 12 "outside the target's range"); the bootstrap dies with `panic: internal interpreter error`
     (a bug in `bootstrap/antpy/arith.py`, outside this step's directories). Keep the runtime's behaviour;
     note the bootstrap bug in the report for step 4.1. The probe's other 13 lines agree.
- Everything else probed agrees: duplicate-field and spread-leak field order and rendering (`02`), `a?.b.c`
  with a null `b` panics `not a record` (`03`), `try` on `7`/`void`/`Ok{}`/`Err{}` (`04`, `05`), the integer
  and float conversion edges (`06`, `07`), `is` on kinds, tags, `Point is Point`, `y is y` (`08`), nested
  quoting, float layout, sized ints, tags/funcs/sync, cyclic arrays, records and maps, templates (`09`),
  cyclic and NaN equality, map keys by `==`, `-0.0` (`10`), a panicking defer on `return`, `return` inside a
  defer during a panic (`11`, `12`), `??` with `void`, `continue`, `break` (`13`, `32`), the `Result` natives
  and their `wrong kind` / `no field` panics (`14`, `15`), index and field panics (`16`-`21`), `try` in `for`
  bodies, anonymous funcs and lambdas (`22`), `panic(1)` (`23`), head-before-entries and map/array literal
  order (`24`-`26`), conversions by named argument and of a string (`27`, `28`), `Entry`/`Indexed` tag
  identity (`29`), a top-level `try` (`31`: `return outside a function`), a panic unwinding through
  `Array.map` with defers pending (`40`).

## 1. Files

Change:
- `runtime/ant.h`: declare `AntValue ant_check_record(AntValue v);  /* the value, or `not a record` */` next
  to `ant_check_tag`.
- `runtime/record.c`: define it next to `ant_check_tag`:
  `AntValue ant_check_record(AntValue v) { if (v.kind != K_RECORD) ant_panic("not a record"); return v; }`.
- `src/antc/codegen/emit.ant`, `emitRecordLit`, the `en is Ast.Spread` branch: replace
  `Array.push(values, emitExpr(g, fn, en.value))` by
  `Array.push(values, checked(fn, T))` where `T` is the template string, in backticks,
  `ant_check_record(${emitExpr(g, fn, en.value)})`
  (`checked` lands the spread operand in a temp through `CHK`, so the panic fires in evaluation order and
  the later entries are not evaluated; `names`/`syms`/`spreads` stay aligned). Leave `ant_record_build`'s
  own `not a record` check in place.
Create: `work/2.2/test.log` (the acceptance), `work/2.2/consistency.log`, `work/2.2/report.md`.
Already present: `work/2.2/baseline.log`, `work/2.2/probes/*.ant`, `work/2.2/probes/run.sh`,
`work/2.2/probes/.gitignore` (hides the `.out`/`.err` files the runner writes).
Unchanged: `Makefile`, `bootstrap/antpy/system.py`, every test, every other runtime and antc file.

## 2. Procedure (literal)

1. `make runtime` (the archive and `runtime/obj/` are git-ignored; antc never rebuilds them; never end a
   round after `make clean`). `sh work/2.2/probes/run.sh 01 41` must print `DIFF` for both (the state
   before the fix; about 15 s).
2. Make the three edits of section 1. `make runtime`.
3. `sh work/2.2/probes/run.sh 01 41` must print `SAME` for both; the expected `.c.out` files:
   `01`: `a`, `exit=2`, `panic: not a record`; `41`: `before`, `a`, `exit=2`, `panic: not a record`.
   Then `sh work/2.2/probes/run.sh` (all probes, about 2.5 minutes): exactly `06-conversions` and
   `37-cyclic-key` may print `DIFF` (section 0). Any other `DIFF` is a regression to fix before going on.
4. Run the acceptance command in the background with output to `work/2.2/test.log` (about 6 minutes) and
   wait; it must exit 0 and its last line must be `144 checks, 0 failures, 0 warnings (implementation mode,
   143 tests)`.
5. Self-check (not required by the row, required by gate 2.G): the acceptance command plus `--consistency`,
   in the background to `work/2.2/consistency.log` (about 12 minutes); it must also end in `0 failures`.
6. Write `work/2.2/report.md` (section 5). Do not refactor: the refactor phase follows the review.

## 3. Spec rules of this row that are easy to get wrong (all pinned by a test or a probe; re-verify only what
a change touches)

- Record literal: the head is checked (`not a tag`) before any entry evaluates (`rec-head-not-tag-rt`, probe
  `24`); entries evaluate in written order, a spread in its place (`rec-field-eval-order`); a non-record
  spread panics at its position (the fix; probes `01`, `41`, `rec-spread-non-record-rt`); without spread a
  duplicate field keeps the first position and takes the last value (`rec-duplicate-field-rt`, probe `02`:
  `Point{ x: 2.0, y: 0.0 }`); with spread a later entry replaces in place (`Vec{ y: 9.0, ...p }` renders
  `Vec{ y: 2.0, x: 1.0 }`); a spread copies run-time fields the head's type never declared, visible to `==`
  and `String.from` (`rec-spread-leak`: `Named{ name: "ada", age: 36 }`); the tag comes from the head
  (`rec-spread`); the shape of a spread-built record is interned, so `==` and the field caches see one shape
  per name sequence and records with the same fields in another order still compare equal by name.
- Field access: `not a record` for anything but a record, including `null` and a tag (`Point.x`, G54:
  probe `20`); `no field: <name>` on a miss, for writes too (`rec-field-assign-missing-rt`); the target is
  checked before the right side of an assignment evaluates; `p.y += 1.0` reads once (G4).
- Index: `not indexable` for anything but an array or a map (`null[0]`, `p[0]`, `"abc"[0]`); array index
  must be `K_INT64` (`index not an int` for `int8(0)` and `0.0`), then `0 <= i < len` (`index out of range`,
  negative included, writes included); map index gives `null` when absent; `Array.get` gives `null` out of
  range and for a stored `null` alike.
- Maps: keys compare by `==` and hash consistently (records by tag + fields order-insensitively, arrays
  elementwise, `-0.0` with `0.0`, `1` and `"1"` and `int8(1)` distinct); a NaN key inserts a new entry every
  time and is never found, removed, or `has` (`map-nan-key`); `null` is an ordinary key; a later literal
  entry with an `==` key replaces the value in place (`map-entry-eval-order-and-dup-key`).
- `is`: a single identifier in `null void bool string func Array Map int8..uint64 float32 float64 int byte`
  is a kind test regardless of bindings (`int` is `K_INT64`, `byte` is `K_UINT8`: `int8(1) is int` is false,
  G48); any other path evaluates and must be a tag (`not a tag`: probe `08` `y is y`); a tag value itself is
  not a record (`Point is Point` is false); a union alias binds its own tag so `x is Shape` is false
  (`tdecl-is-union-alias-rt`, G17); `Ok`/`Err`/`Entry`/`Indexed` are the prelude tags, so literals and the
  natives build the same tags (`result-ctor-eq`, probe `29`).
- `==`: structural, never panics, no identity shortcut (`p == p` is false with a NaN field, `rec-eq-nan-field`);
  a pair already under comparison is equal (`rec-eq-cyclic`, probe `10` for arrays); different kinds are
  unequal (`1 == 1.0`, `int8(1) == 1`, `null == void` all false); functions, tags, sync objects by identity
  (two lambdas of the same text differ); maps by key set and values, order-insensitive.
- Rendering (`String.from`, `print`, templates): strings bare at the top level and quoted with `\\ \" \n \t
  \r` escapes inside containers; sized ints as bare digits; floats shortest round-trip with the JavaScript
  thresholds (`1e21`, `1.5e-7`, `123456789.0`, `100.0`, `-0.0`, `NaN`, `Infinity`, `-Infinity`; float32 by
  its own shortest digits: `float32(0.1)` renders `0.1`); `Tag{ a: 1 }`, `Tag{}`, `[1, 2]`, `[]`, `Map{ k: v }`,
  `Map{}`; a container already being rendered prints `...`; tags render as their name, functions as `func`,
  sync objects as their namespace name, `void` as `void`.
- `?.`: `a?.b`, `a?.[i]`, `f?.(x)`: a `null` left operand makes the whole postfix chain `null` and skips the
  rest, arguments included (`optchain-skips-rest`); only the `?.` steps test for null, so `u?.address.city`
  with a null `address` panics `not a record` (probe `03`); a non-null non-record panics `not a record`
  (`(1)?.x`, G21).
- `??`: the right side is evaluated only for a `null` left side (`void` is not `null`); it may be `return`,
  `break`, `continue`, or `panic(...)`, which just emit their cascade; `x ?? return y + 1` returns `y + 1`.
- `try`: `null` returns `null` from the enclosing function; an `Err` returns a fresh `Err` carrying the same
  `error` value (structurally equal, not `Record.same`, `try-err-fresh-record`); an `Ok` yields its `value`
  (one level only, `try-single-level`); anything else passes through (`try 7` is `7`, G28); `Ok{}`/`Err{}`
  without the field panic `no field: value` / `no field: error`; it binds like a prefix operator
  (`try-precedence`); in a `for` body it targets the enclosing function's frame (`self->ret`), in a lambda or
  anonymous `func` that body's own frame; at the top level it panics `return outside a function`.
- Conversions: the argument must be a number (`wrong kind`, `int("5")`); integer targets truncate toward
  zero, then range-check (`uint8(-0.5)` is `0`, `int8(-128.9)` is `-128`, `int8(127.9)` is `127`,
  `int64(9223372036854775807.0)` and `uint64(18446744073709551615.0)` panic because the doubles are `2^63` and
  `2^64`, `int64(-9223372036854775808.0)` is the minimum); NaN and infinities panic for integer targets and
  pass through to float targets; `float32` rounds to nearest (`float32(16777217.0) == float32(16777216.0)`,
  `float32(1e-50)` is `0.0`) and panics above `FLT_MAX` (G7); `float64(float32(0.1))` is
  `0.10000000149011612`; `int`/`byte` are `int64`/`uint8`; a conversion is a first-class function value
  (`conv-as-value`) with the parameter `x`.
- Defer and panics: a block's deferred closures run in reverse order on every exit; `return e` evaluates `e`
  before the defers run and they cannot change it (`defer-cannot-change-return`); a `return` inside a
  deferred block leaves that block only and a saved panic is restored afterwards (`defer-return-local`,
  probe `12`); a panic inside a deferred block replaces the pending `return` or panic, and the remaining
  deferred blocks still run (`defer-panic-replaces`: `body`, `second`, `first`, then `panic: replacement`);
  every block being evaluated exits in turn, through natives that call back (`rt-unwind-nested`, probe
  `40`); stdout printed before a panic reaches the terminal (flush before `_exit(2)`).
- `Result`/`Record`: `Result.ok(v)` is `Ok{ value: v }`; `map`/`mapErr` run `f` on one branch only and
  return a fresh record on both; `unwrap` on an `Err` panics `unwrap on Err`; a non-`Result` argument is
  `wrong kind`; `Record.same` is pointer identity and `wrong kind` for non-records; `Array.sort` of mixed
  kinds panics `kind mismatch` after the `wrong kind` check (`gen-union-solution-escapes-bound`).

## 4. Out of scope (do not touch; the report lists them as carried forward)

- `ant_call` raising `missing argument` before any default evaluates, the yield object accepting the named
  argument `x:`, `for`-body closures pushing no frame: 2.1 decisions, not in this row.
- Library widening (`String.toUpper` beyond ASCII, `Json`, file handles): 2.3 and 2.7.
- Goroutines and the sync objects beyond the synchronous stubs: 2.6.
- The bootstrap's `float32` crash and its `map key too deep` panic: `bootstrap/` is closed to this step.
- Tests: none contradicts the spec; `work/TEST-CHANGES.md` stays as it is.

## 5. `work/2.2/report.md`

- Status: the exact last line of `work/2.2/test.log` and of `work/2.2/consistency.log`.
- What was built: that 2.1 covered the deliverable, where each item lives (section 0, first bullet), and the
  one change (the spread check order) with the spec citation.
- Decisions: the cyclic-key and `float32` edge divergences from the bootstrap (section 0, items 2 and 3) with
  the reading chosen and why; the probe set and how to run it.
- Test changes: none.
- Known limitations for later steps (section 4).

## 6. Risks

1. The spread edit must keep `names`, `syms`, `values`, `spreads` aligned by index and keep evaluation order
   (the `checked` temp is taken right after the operand's own temp); verify with probes `01`, `41`, `02`,
   `rec-field-eval-order` and `rec-spread` before the full run.
2. A stale `runtime/libantrt.a`: run `make runtime` after the C edit, or the acceptance silently tests the
   old archive (antc links whatever is there).
3. Time: every test spends about 2.5 s in antpy plus clang; run the acceptance and the consistency check in
   the background with their logs under `work/2.2/` and wait for them; never run them in the foreground.
4. A reviewer may question a step that changes a dozen lines: the answer is section 0 (the design's build
   order put these pieces into 2.1 because `expect.ant` needs them; this step's job was to verify them,
   which the probes and the acceptance do) and the report must say so.
5. Do not "fix" `ant_convert`'s `float32` range check to the bootstrap's behaviour: the bootstrap crashes on
   that input, so there is nothing to match; the runtime's reading is the spec's wording.
