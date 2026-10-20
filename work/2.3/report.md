# Report for step 2.3 "rt-stdlib"

## Status

Acceptance (`work/2.3/test.log`, exit 0), last line of the runner's output:

    90 checks, 0 failures, 0 warnings (implementation mode, 89 tests)

Self-check with `--consistency` (`work/2.3/consistency.log`, exit 0; gate 2.G requires it), last line:

    90 checks, 0 failures, 0 warnings (implementation mode, 89 tests)

(Both logs end with an extra `exit=0` line appended by the shell wrapper that ran them.) The committed tree
ended `90 checks, 1 failures` (`work/2.3/baseline.log`): `run/stdlib/str-case.ant`, because `String.toUpper`
mapped ASCII only.

## What was built

Step 2.1 had already built every native of this row (`work/2.3/plan.md` section 0: Array with the stable
merge sort, Map, String, Iter, Math, Bits, Result, Record, the sync stubs, rendering). This step fixed the
two known defects, swept the whole row against the bootstrap with 115 probe programs, and fixed the two
divergences the sweep found. Diff against HEAD after the refactor pass (below): 6 files changed (70
insertions, 20 deletions) plus the generated `runtime/unicase.c` (399 lines) and its generator
`runtime/gen_unicase.py`. `make runtime` builds without warnings under `-Wall -Wextra`.

### A. Unicode simple case mapping (`str-case`; G36)

- `runtime/gen_unicase.py` (development-only, run once by hand with `python3 -I`; nothing in the build or
  the runtime runs it) writes `runtime/unicase.c` from Python's Unicode tables with exactly the bootstrap's
  rule (`bootstrap/antpy/library.py` `case_map`): a code point maps when `str.upper()` / `str.lower()` of it
  is a single code point that differs from it, and stays otherwise. Python 3.15.0, Unicode 17.0.0;
  1478 UPPER pairs, 1487 LOWER pairs, sorted by code point; the header comment of the generated file names
  the generator and the versions. Checked in; regenerate rather than edit.
- `runtime/unicase.c`: the two tables and `ant_cp_upper` / `ant_cp_lower` (binary search; the input on a
  miss), declared in `runtime/ant.h`.
- `runtime/string.c` `case_map`: a code-point walk through `ant_utf8_decode`, the tables, and the new
  builder method `ant_sb_append_cp` (UTF-8, 1 to 4 bytes; `<ctype.h>` dropped: locale dependent in
  principle), finished with `ant_sb_finish` so `ncps` / `ascii` are recomputed; the byte length may change
  (`ı` U+0131, two bytes, maps to `I`).
- Verified against the bootstrap on every code point (`work/2.3/harness/`, below): 0 differences for
  `toUpper` and 0 for `toLower` over 1,112,061 code points, plus mixed strings (`ß` stays, `ı` to `I`,
  `İ` stays under `toLower`, `ǅ` to `Ǆ` / `ǆ`, `σς` to `ΣΣ`, Deseret and Georgian Mtavruli map).

### B. Shortest round-trip float digits (`render.c`; G33, G55)

`shortest_digits` printed the correctly rounded p-digit decimal for p = 1..17 and kept the first that
re-parsed to x. At a power of two the round-trip interval is asymmetric (narrower below), so the nearest
p-digit decimal can lie outside it while the next p-digit decimal above lies inside; the loop then went on
to p + 1 and printed 17 digits where 16 suffice (`Math.pow(2.0, -24.0)` printed `5.9604644775390625e-8`;
the bootstrap, Python's `repr` and JavaScript print `5.960464477539063e-8`). Now, at each precision, when the
rounded candidate fails, its upper decimal neighbour (`next_decimal`: last digit plus one with carry;
`9.99e+05` becomes `1.00e+06`) is tried before moving on; only a candidate that reads back is ever accepted.
The same test applies to the float32 path (`strtof`).

Verified with `work/2.3/harness/` against `bootstrap/antpy/render.py`: 402,557 doubles (every power of
two from 2^-1074 to 2^1023, every power of ten, the G33 examples, 300,000 random bit patterns, 100,000
random magnitudes): 0 differences. 199,514 float32 values: 0 wrong; 3 where the runtime prints a shorter
string that round-trips (`1.2621775e-29` for the bootstrap's `1.26217745e-29`, and two more), because the
bootstrap's float32 loop has no neighbour test; G33 says "shortest decimal that round-trips", so the runtime
is the one the spec supports and `bootstrap/` is closed to this step. One float32 near `FLT_MAX` crashes the
bootstrap (next section).

### C. Probe sweep (`work/2.3/probes/`, log `work/2.3/probes.log`)

115 programs in the 15 groups of the plan (panicking cases as separate `NN-pK-*` programs), run with
`sh work/2.3/probes/run.sh [NN ...]` from the repository root; each runs under the bootstrap and under antc
and prints `SAME` or `DIFF` (stdout, exit status, first stderr line). First full run: 109 `SAME`, 6
`DIFF`. After the changes below (the log records the reruns): 113 `SAME`, 2 `DIFF`, both kept:

1. `05-p1-and-mixed` (`Bits.and(int8(1), 1)`) and `05-p5-and-float` (`Bits.and(1.0, 1.0)`): the bootstrap
   prints the bare `panic: wrong kind`; the runtime `panic: wrong kind: expected integers of one kind` /
   `... expected an integer`. Only the detail after the canonical phrase differs; `src/tests/README.md`
   fixes the phrase and the runner matches it as a substring (`run.py` `expect_run`: `phrase not in err`),
   and the bootstrap itself appends such details everywhere else (`wrong kind: expected a number`). Kept
   as the runtime's uniform style.

The other four `DIFF`s were settled as follows:

2. `02-float-shortest`, the line `print(float32(3.4028234e38))`: the runtime prints `3.4028235e38`; the
   bootstrap dies with `panic: internal interpreter error` (`bootstrap/antpy/render.py` `shortest_digits`
   calls `round_f32` on a short candidate such as `3.403e+38` that exceeds `FLT_MAX`, and `struct.pack("f")`
   raises). It is the bootstrap `float32` defect `work/2.2/report.md` already records, in a file closed to
   this step. The probe's input was changed to `float32(3.4e38)` so that the other 33 lines stay
   comparable; the rerun is `SAME`.
3. `15-p14-atomic-load-int`: the runtime printed `wrong kind: expected a Atomic`. Fixed in `runtime/sync.c`
   `need`: the article follows the name (`an Atomic`).
4. `15-p3-atomic-add-float` (`Atomic.add(Atomic.new(1), 1.0)`): the bootstrap panics `kind mismatch`, the
   runtime panicked `wrong kind: expected numbers of one kind`; `15-p12-atomic-add-string`
   (`Atomic.add(Atomic.new("a"), "b")`): `wrong kind: expected a number` versus the same misleading
   message. Fixed in `runtime/sync.c` `atomic_add`: each operand must be a number (`wrong kind: expected a
   number`), then the sum goes through `ant_add`, so operands of different kinds are `kind mismatch` and an
   overflow is `integer overflow`. See "Decisions and deviations".

Everything else the plan listed is `SAME`: code-point case mapping, float layouts in arrays, records and
templates, quoting and escapes (including a raw NUL and U+1F600 inside containers), sized ints, `Math`
edge cases (NaN and tie rules of `min` / `max`, `-0.0` through `floor` / `ceil` / `round`, `pow` / `sqrt`
domain results), every `Bits` row including the `shift count` panics, the 25 White_Space code points in
`trim` and the kept U+001C..U+001F, U+200B, U+FEFF, U+180E, `toInt` / `toFloat` edge syntax (overflow,
underflow to `-0.0`, Arabic digits), strict `fromBytes` (overlong, surrogate, above U+10FFFF, truncated),
live length in `Array.map` / `filter` callbacks, `sortBy` key evaluation order, stability of `sortWith`,
code-point order of string sorts, `Array.filled` aliasing, `Array.sum` kinds and overflow, NaN and `null`
keys, array / record / map keys, snapshots in `Map.map` / `filter`, a 1000-entry map with half the keys
removed, `Iter` laziness, `take(s, 0)`, `return` and panic through three stages with the source's `defer`,
re-running stages, hand-driven stages, `Result` / `Record` rows, named arguments and the four call-shape
panics on natives, pipes into natives, natives as values, the sync namespaces' rendering, kind checks,
`not locked` / `negative counter`, `compareAndSwap` by `==`, `Once.do` after a panic, and `withLock`
releasing on a panic.

### D. The harness (`work/2.3/harness/`)

`harness.c` links `libantrt.a` and prints the runtime's rendering of doubles / float32s given as hex bits and
`String.toUpper` / `toLower` of lines; `check.py` compares it with `bootstrap/antpy`'s `render_float` and
`case_map` at the scale stated above (3.5 seconds). Development-only, run by hand (its README has the two
commands); nothing in the build or the runtime uses it.

## Decisions and deviations

- `Atomic.add` with operands of different kinds panics `kind mismatch`, where `docs/runtime-design.md`
  section 3 says "`add` requires two numbers of the same kind (`wrong kind`)". The spec supports the
  bootstrap: section 11 defines `Atomic.add[T: Number](a, d)` as returning the new value with the overflow
  panic of `+`, and section 12's operator rule makes operands of different kinds `kind mismatch`; the
  runtime already follows that rule for `Math.min(1, 1.0)` and `Array.sum([1, 1.0])` (both `kind mismatch`,
  pinned by probes and by the plan's section 3), and the bootstrap (`bootstrap/antpy/sync.py`
  `atomic_add`) is the reference every test has run against. Each operand that is not a number is still
  `wrong kind` (section 12's library rule). No test in any tier pins the mixed case
  (`run/concurrency/atomic-add-float.ant` adds floats to a float).
- Float32 rendering keeps the neighbour test although the bootstrap's float32 path lacks it (3 of 199,514
  values print shorter under the runtime; all round-trip). G33 decides for the shortest string.
- Probe `02` does not include a float32 within rounding distance of `FLT_MAX` because the bootstrap cannot
  render one; the runtime's `3.4028235e38` for `float32(3.4028234e38)` was checked by hand (the harness run
  covers 2^127 and the random values, and asserts that every runtime output re-parses to the input).
- The `Bits` `wrong kind` suffixes stay (probe section C.1).
- Procedure: the acceptance and `--consistency` runs were started while the probe sweep was still running,
  as in step 2.2 (`antc build` names its temporary files by clock and hash; the runner builds tests side by
  side). Both were green before the `sync.c` change and were rerun after it; the logs are the reruns.
- `src/antc/`, `Makefile` and `bootstrap/antpy/system.py` are unchanged: the row is runtime natives, and
  `RT_SRC = $(wildcard runtime/*.c)` picks up `unicase.c`.

## Test changes

None. No stdlib test contradicts the spec; `work/TEST-CHANGES.md` is unchanged.

## Known limitations (carried forward)

- `Map.keys` / `values` / `entries` and map rendering follow insertion order, which section 11 leaves
  unspecified ("unspecified order"); the bootstrap does the same, and no test asserts a multi-entry order.
- The sync namespaces are the single-threaded stubs of 2.1: anything that would block (`Mutex.lock` twice,
  `WaitGroup.wait` above zero, `Cond.wait` under the lock) panics `deadlock` until step 2.6; the kind
  checks, `not locked`, `negative counter`, `Once` and `Atomic` rows are final.
- `String.toLower("İ")` leaves U+0130 unchanged (its lowercase is two code points), as the one-code-point
  rule of G36 and the bootstrap read it; `ß` stays under `toUpper`.
- The number of comparator calls `Array.sortWith` makes, and the relative order of NaNs in `Array.sort`,
  differ from the bootstrap's timsort; the spec fixes neither (G35: NaN order untested) and no probe logs
  them.
- The bootstrap's `float32` crash on values near `FLT_MAX` (both in conversion, 2.2, and in rendering, this
  step) and its `map key too deep` panic on cyclic keys: `bootstrap/` is closed; for step 4.1.
- `runtime/unicase.c` is tied to the Unicode version of the Python that generated it (17.0.0); a bootstrap
  run under another Python could disagree on code points whose mappings changed between versions. The
  header comment states the provenance so the table can be regenerated.

## Refactor

The refactor pass after the review: the `simplify` recipe applied in a single pass (no agent fan-out was
available), then the reviewer's two suggestions, each followed by a subset run and the full acceptance
command at the end. Behavior is unchanged.

- `runtime/render.c` `shortest_digits`: the `up` buffer and its two `memcpy`s are gone; `next_decimal`
  runs on `buf` in place after the rounded candidate fails to read back. Safe because the loop either
  breaks on success or overwrites `buf` with `snprintf` at the next precision, and the `p == maxp` break
  comes before any mutation.
- `runtime/string.c`: the static `utf8_encode` became the builder method `ant_sb_append_cp(sb, cp)`,
  declared in `ant.h` beside the other `ant_sb_*` functions as the counterpart of `ant_utf8_decode` (the
  `Json.parse` of step 2.7 needs the same encoder), so `case_map` appends each mapped code point directly.
  `case_map`'s separate ASCII loop is gone: the code-point loop handles ASCII (decode and encode are one
  comparison each; only the table search is extra, on a function nothing hot calls), and `upper` is a
  `bool`.
- `runtime/number.c`: `ant_need_number(v)` (`wrong kind: expected a number`, beside `ant_need_ordered`)
  replaces the four inline copies of that check in `ant_convert`, `Math.abs` (`number.c`), `Array.sum`
  (`array.c`) and `Atomic.add` (`sync.c`), centralizing the library `wrong kind` rule as
  `docs/runtime-design.md` section 6 asks.
- `runtime/sync.c` `need`: a comment on the article (`"an Atomic"`; the bootstrap's `need_sync` rule).
- `runtime/gen_unicase.py`: the generator moved from `work/2.3/` next to the file it produces; its root
  path and header text were adjusted and `runtime/unicase.c` regenerated with it (Python 3.15.0, Unicode
  17.0.0): the new file differs from the previous one in the header line only. The Makefile's `RT_SRC`
  wildcard matches `*.c` alone, so the build is unchanged and nothing executes the script.

Verification: `make runtime` is clean under `-Wall -Wextra`. The harness (section D) after the changes:
doubles 402,557 checked, 0 differ; float32 199,514 checked, 0 wrong (the same 3 shorter); `toUpper` and
`toLower` 0 differences over 1,112,061 code points. Subsets after each change: `refactor-subset1.log`
(strings, sums, kind checks, smoke) 22 checks, 0 failures; `refactor-subset2.log` (float rendering,
conversions, case mapping) 14 checks, 0 failures. The full acceptance command (`test.log`, exit 0):

    90 checks, 0 failures, 0 warnings (implementation mode, 89 tests)
