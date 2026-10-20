# Plan for step 2.3 "rt-stdlib"

Row: "the core library natives, float formatting, stable sort". Directories: `runtime/`, `src/antc/`, `Makefile`,
`bootstrap/antpy/system.py` (`Os.exec` only; nothing to do there), plus `work/2.3/`.

Acceptance (verbatim, run from the repository root; about 2 minutes under antpy for these 89 tests, measured
on the baseline; run it in the background with output to `work/2.3/test.log` and wait):

    python3 -I src/tests/run.py --timeout 600 --run "python3 -I bootstrap/antpy.py run src/antc/main.ant run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run src/antc/main.ant run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --filter /smoke/ --filter /stdlib/

## 0. Where the tree stands (measured, `work/2.3/baseline.log`)

The acceptance command on the committed tree ends with `90 checks, 1 failures, 0 warnings (implementation mode,
89 tests)`. The one failure is `run/stdlib/str-case.ant`: `panic: non-ASCII upper: got é, want É`, because
`runtime/string.c` `case_map` maps ASCII only (a limitation `work/2.1/report.md` left for this step).

Step 2.1 already built everything else this row names, in the files `docs/runtime-design.md` section 5 assigns:
Array (`runtime/array.c`, with the stable merge sort whose comparator bails out on a panic), Map (`map.c`),
String (`string.c`), Iter (`iter.c`), Math and Bits (`number.c`), Result and Record (`result.c`), rendering
(`render.c`), the sync namespaces as single-threaded stubs (`sync.c`), `print`/`panic` (`io.c`), and the
argument-check macros `ARR`/`MAP`/`STR`/`INT`/`FUNC`/`UNWOUND` in `ant.h`. Every `/stdlib/` test but `str-case`
passes natively, including `print-float-format`, the sort tests, and all 15 `unchecked/stdlib/lib-kind-*` tests.

This step therefore has two known defects to fix and a verification sweep (as step 2.2 did) to find the rest:

1. `String.toUpper`/`toLower` must apply simple one-to-one Unicode mappings (G36; the test).
2. Float rendering is not always the shortest round-trip string (G33): `render.c` `shortest_digits` prints the
   correctly rounded p-digit decimal for p = 1..17 and keeps the first that re-parses to x. At a power of two the
   rounding interval is asymmetric (narrower below), so the nearest p-digit decimal can lie outside it while the
   next p-digit decimal above lies inside; the loop then goes on to p+1. Measured against Python's `repr` (the
   bootstrap's rendering) over every power of two: 46 doubles get 17 digits instead of 16, e.g.
   `Math.pow(2.0, -24.0)` prints `5.9604644775390625e-8` where the bootstrap (and JavaScript) print
   `5.960464477539063e-8`. Random doubles never differ. No test pins this, but the deliverable names float
   formatting and G33 says "shortest decimal that round-trips", so fix it.

Nothing under `src/antc/` is expected to change; the sections are runtime natives.

## 1. Files

| File | Change |
|---|---|
| `work/2.3/gen_unicase.py` | new, development-only generator (run by hand once; never by the build or the runtime) that writes `runtime/unicase.c` from Python's Unicode tables |
| `runtime/unicase.c` | new, generated: two sorted `uint32_t [][2]` tables (code point, mapped code point) and `int32_t ant_cp_upper(int32_t)` / `ant_cp_lower(int32_t)` (binary search; the code point itself on a miss) |
| `runtime/ant.h` | declare `ant_cp_upper`, `ant_cp_lower` |
| `runtime/string.c` | `case_map`: code-point mapping through the tables, with a static UTF-8 encoder; keep an ASCII fast path |
| `runtime/render.c` | `shortest_digits`: also try the upper decimal neighbour of the rounded candidate at each precision |
| `Makefile` | nothing: `RT_SRC = $(wildcard runtime/*.c)` picks up `unicase.c`; `make runtime` must be run after every C change (`antc build` links the archive and never rebuilds it) |
| `work/2.3/probes/*.ant`, `work/2.3/probes/run.sh`, `work/2.3/probes/.gitignore` | the probe set of section 2, step C, with step 2.2's runner (`work/2.2/probes/run.sh` with `D=work/2.3/probes`) and its `.gitignore` (`*.out`, `*.err`) |
| `work/2.3/test.log`, `work/2.3/consistency.log`, `work/2.3/probes.log` | run logs (git-ignored by `work/**/*.log`) |
| `work/2.3/report.md` | the report, in the shape of `work/2.2/report.md` |

Anything else only if a probe in step C finds a divergence the spec settles against the runtime (step D).

## 2. Approach and order

### A. Unicode simple case mapping (`str-case`)

The reading, fixed by G36 and by the bootstrap (`bootstrap/antpy/library.py` `case_map`, which is the reference
every program has run against so far): a code point maps when its mapping is a single code point, and is left
alone otherwise. `"ß"` stays `"ß"` (its uppercase is `"SS"`), `"İ"` stays under `toLower` (Python's lowercase of
it is two code points), `"é"` becomes `"É"`, `"ı"` becomes `"I"`, `"ǅ"` becomes `"Ǆ"`/`"ǆ"`. No locale, no
context rules (final sigma), no title case.

1. `work/2.3/gen_unicase.py`: for every code point 0..0x10FFFF except surrogates, with `c = chr(cp)`: emit
   `(cp, ord(c.upper()))` into UPPER when `len(c.upper()) == 1 and c.upper() != c`; likewise LOWER with
   `c.lower()`. This is exactly the bootstrap's rule, so the runtime and the bootstrap agree on every code point.
   Write `runtime/unicase.c` with a header comment naming the generator, the Python version and
   `unicodedata.unidata_version` (here: Python 3.15.0, Unicode 17.0.0; 1478 UPPER pairs, 1487 LOWER pairs),
   the two tables as `static const uint32_t UPPER[][2]` / `LOWER[][2]`, eight pairs per line, sorted by code
   point, and the two lookup functions (binary search; return the input when absent). The generated file must
   compile under the Makefile's `-Wall -Wextra` without warnings. Run the generator with `python3 -I`. The
   tables are checked in; the runtime executes nothing, and the build runs only clang.
2. `runtime/string.c`: replace `case_map`. If `s->ascii`, keep a byte loop but use explicit arithmetic
   (`c - 'a' + 'A'`) rather than `<ctype.h>` (locale-dependent in principle; drop the include if unused).
   Otherwise walk the string with `ant_utf8_decode`, map each code point through `ant_cp_upper`/`ant_cp_lower`,
   and append it with a new `static int utf8_encode(int32_t cp, char out[4])` (1 to 4 bytes; 2.1 removed the
   old `ant_utf8_encode` as dead, so write it fresh and keep it static). Finish with `ant_sb_finish`
   (`ant_str_new` recomputes `ncps` and `ascii`). The result's byte length may differ from the input's
   (e.g. `ı` U+0131, two bytes, maps to `I`, one byte), so never assume equal sizes.
3. `make runtime`, then run `--filter /stdlib/str-` (fast) and the whole acceptance later.

### B. Shortest round-trip float digits (`render.c`)

In `shortest_digits`, for each precision p: format with `%.*e` as now; if it round-trips, done; otherwise build
the upper neighbour of the candidate, the same mantissa with one added to its last digit and the carry
propagated (`9.99e5` becomes `1.00e6`: when the carry runs off the front, the mantissa becomes `1` followed by
zeros and the exponent grows by one), and test that; accept it when it round-trips. Only then move to p+1; at
p = 17 (9 for float32) keep the rounded candidate as now. Verified in Python over every power of two (all 46
fixed) and 300,000 random doubles (no regression): the round-tripping p-digit decimals are contiguous and the
rounded one is nearest to x, so when it fails only its upper neighbour can succeed (the narrow side of an
asymmetric interval is always below a power of two). Apply the same neighbour test on the float32 path
(`strtof`). Keep the rest of `render_float`/`layout` as is: it already matches G33/G55 and `print-float-format`
passes.

Check with probe `02-float-shortest` (step C) and rerun `--filter print-float`.

### C. Probe sweep against the bootstrap

Write the probes below in `work/2.3/probes/NN-name.ant` (plain `run`-style programs with `func main()`, using
`print`; no imports needed) and run `sh work/2.3/probes/run.sh` (copy `work/2.2/probes/run.sh`, change `D`).
Each probe prints `SAME` or `DIFF` (stdout, exit status, first stderr line) between the bootstrap and antc. The
expectations below come from `ant-lang.md` section 11/12 and the GAPS readings; the bootstrap matches all of
them per `work/0.3/report.md` ("Untested choices"), so a `DIFF` is a runtime defect unless noted.

1. `01-case-mapping`: the step A examples plus Greek (`σς` to `ΣΣ`, `Σ` to `σ`), Cyrillic (`я`/`Я`), fullwidth
   (`ａ`/`Ａ`), a non-BMP letter (Deseret U+10400 to U+10428), Georgian Mtavruli U+1C90 to U+10D0, mixed
   ASCII/non-ASCII strings, and `String.length` of each result (code points are preserved one to one).
2. `02-float-shortest`: `print(Math.pow(2.0, -24.0))` (`5.960464477539063e-8`), `Math.pow(2.0, 89.0)`
   (`6.189700196426902e26`), `Math.pow(2.0, -44.0)` (`5.684341886080802e-14`), `Math.pow(2.0, 53.0)`
   (`9007199254740992.0`), `Math.pow(2.0, 63.0)` (`9223372036854776000.0`), `1e23` (`1e23`), `5e-324`
   (`5e-324`), `1.7976931348623157e308`, `2.2250738585072014e-308`, `1e-7` (`1e-7`), `1e-6` (`0.000001`),
   `1e20` (`100000000000000000000.0`), `123456789012345680000.0`, `100.0 / 3.0`, `0.1 + 0.7`; float32:
   `float32(0.1)` (`0.1`), `float32(1.0) / float32(3.0)` (`0.33333334`), `float32(16777216.0)`,
   `float32(3.4028235e38)` (`3.4028235e38`), `float32(1e-45)` (`1e-45`), `-float32(0.0)` (`-0.0`), and a float32
   inside an array and a record.
3. `03-render-escapes`: strings with `\\`, `"`, `\n`, `\t`, `\r`, a NUL (`"\u{0}"`), `\u{1F600}` inside arrays,
   records and maps (`render_quoted` and the bootstrap's `QUOTE_TABLE` escape exactly `\\ " \n \t \r`; every
   other byte is copied, so both print the raw NUL); nested `Map{ "k": [Map{ 1: "a" }] }`; `Empty{}` inside an
   array; `String.from` of a template that contains a record; `print(int8(-128))`, `print(uint64(0) - uint64(0))`.
4. `04-math-edges`: `Math.min(1, 1.0)` (`kind mismatch`) in its own probe section via separate programs if a
   panic ends the probe early: split panicking lines into `04a`, `04b`, ... (one panic per program). Non-panicking
   lines: `Math.min(1.0, 0.0/0.0)` is `1.0`, `Math.max(0.0/0.0, 1.0)` is `NaN` (the first argument on a tie or
   NaN), `Math.min(4, 4)`, `Math.abs(float32(-1.5))` stays float32, `Math.abs(uint8(0))`,
   `Math.pow(0.0, -1.0)` (`Infinity`), `Math.pow(-0.0, -1.0)` (`-Infinity`), `Math.pow(-8.0, 0.5)` (`NaN`),
   `Math.sqrt(-1.0)` (`NaN`), `Math.floor(-0.0)`/`Math.ceil(-0.5)`/`Math.round(-0.4)` (`-0.0`),
   `Math.round(1e300)`, `Math.floor(1.0/0.0)`. Panics: `Math.min(true, false)` (`wrong kind`),
   `Math.floor(float32(1.5))` (`wrong kind`), `Math.pow(2, 2.0)` (`wrong kind`).
5. `05-bits`: `Bits.shiftLeft(int8(1), 7)` is `-128` (dropped bits never panic, G22), `Bits.shiftRight(int8(-128), 7)`
   is `-1` (arithmetic), `Bits.shiftRight(uint8(128), 7)` is `1`, `Bits.not(int8(0))`, `Bits.wrapAdd(int8(127), int8(1))`
   is `-128`, `Bits.wrapMul(uint8(16), uint8(16))` is `0`, `Bits.popCount(int16(-1))`; panics (separate programs):
   `Bits.and(int8(1), 1)` (`wrong kind`), `Bits.shiftLeft(1, 64)` (`shift count`), `Bits.shiftLeft(1, -1)`
   (`shift count`), `Bits.shiftLeft(1, 1.0)` (`wrong kind`), `Bits.and(1.0, 1.0)` (`wrong kind`).
6. `06-string-edges`: `String.split("abc", "abc")` is `["", ""]`, `String.indexOf("aaa", "aa")` is `0`,
   `String.replace("aaa", "a", "")` is `""`, `String.replace("", "a", "b")` is `""`, `String.trim` of each of the 25
   White_Space code points around `x` (U+0009..U+000D, U+0020, U+0085, U+00A0, U+1680, U+2000..U+200A, U+2028,
   U+2029, U+202F, U+205F, U+3000) and that U+001C..U+001F, U+200B, U+FEFF are kept; `String.toInt` of
   `"-9223372036854775808"` (the smallest int), `"+0"`, `"٣"` (Arabic digit: `null`); `String.toFloat` of `".5"`,
   `"5."`, `"inf"`, `"nan"`, `"1_0"`, `"1e"`, `"1e999"` (all `null`), `"-1e-400"` (`-0.0`, underflow is finite),
   `"1E5"`; `String.chars("")`, `String.slice("", 0, 0)`, `String.repeat("", 5)`, `String.join([], "-")`,
   `String.concat([])`; `String.fromBytes` of a 4-byte sequence, of `[byte(0xF4), byte(0x8F), byte(0xBF), byte(0xBF)]`
   (U+10FFFF, valid), of `[byte(0xC1), byte(0xBF)]` (overlong, `null`), of `[byte(0xE0), byte(0x80), byte(0x80)]`
   (overlong, `null`); `String.toBytes` of a 3-byte character.
7. `07-string-kinds` (one panic per program): `String.contains("a", 1)`, `String.join([1], ",")`,
   `String.concat(["a", 1])`, `String.fromBytes([1])` (`wrong kind`, the element is not a byte),
   `String.fromBytes("x")`, `String.slice("abc", 2, 1)` and `String.slice("abc", -1, 0)` (`index out of range`),
   `String.repeat(1, -1)` (`wrong kind` before `negative count`: the first argument is checked first),
   `String.split("", "")` (`empty separator`), `String.length(null)`.
8. `08-array-callbacks`: `Array.map(xs, f)` where `f` pushes onto `xs` (both visit the appended elements: the
   length is re-read each iteration, like `for`), `Array.filter` where `f` pops (shortened), `Array.find`
   returning the element itself (`Record.same`), `Array.reduce` with a two-parameter `func`, `Array.map` with a
   lambda returning `void`, `Array.some`/`every` short-circuit (count the calls), `Array.sortBy` where `key`
   runs once per element in order (log the calls), `Array.sortWith` with a comparator that always returns `0`
   (stable: input order), `Array.sort` of `[2.0, 1.0, 1.0]`, of strings mixing ASCII and non-ASCII (code point
   order), of `int8`s; `Array.flatten` of nested arrays stays one level; `Array.filled(2, [])` aliasing;
   `Array.range(-2, 1)`; `Array.sum([int8(1)])` keeps the kind; `Array.sum([1.0, 2.0])`.
   Note: the *number* of comparator calls `Array.sortWith` makes differs between the bootstrap (Python's
   timsort) and the merge sort; the spec fixes neither, so do not log comparator calls in a probe and do not
   chase it. The same holds for the relative order of NaNs in `Array.sort` (G35: untested).
9. `09-array-kinds` (one panic per program): `Array.sort([true])` (`wrong kind`, the pre-check precedes any
   comparison), `Array.sort([1, "a"])` (`kind mismatch`), `Array.sortBy([1, 2], { true })` (`wrong kind`),
   `Array.sortWith([2, 1], func(a: int, b: int) { 1.0 })` and `... { int8(-1) }` (`wrong kind: expected an int`),
   `Array.flatten([1])`, `Array.range(0, 2.0)`, `Array.concat([1], Map{})`, `Array.get([1], 1.0)`, `Array.push(1, 2)`,
   `Array.filled(2.0, 0)`, `Array.sum([1, "a"])` (`wrong kind`: every element must be a number before the fold),
   `Array.sum([1, 1.0])` (`kind mismatch`), `Array.slice("s", 1.0, 2)` (`wrong kind: expected an array`: the first
   argument is checked first), `Array.reduce([1], 0, { x })` (`too many arguments`: a lambda takes at most one),
   `Array.map([1], func(a: int, b: int) { a })` (`missing argument`), `Array.map([1], 1)` (`wrong kind`).
10. `10-map-edges`: NaN keys (G13: `Map.set` twice gives size 2, `Map.get`/`Map.has`/`Map.remove` miss),
    `null` as a key, arrays and records as keys (by `==`), `Map.fromEntries(Map.entries(m)) == m`,
    `Map.entries(Map{ null: 1 })`, `Map.merge(m, Map{})` is a new equal map, `Map.map` whose callback inserts
    into `m` (the snapshot is unaffected), `Map.filter` whose callback removes from `m`, `Map.keys` after
    `Map.remove` (the removed key is gone; compare sorted), `Map.get` of a stored `null` with `Map.has`,
    `Map.size` after replacing a value.
11. `11-map-kinds` (one panic per program): `Map.fromEntries([1])` (`wrong kind: expected a record`),
    `Map.fromEntries([Ok{ value: 1 }])` (`no field: key`), `Map.merge(m, [])`, `Map.map(m, 1)`, `Map.filter(m, { 1 })`
    (`not a bool`), `Map.has([], 1)`.
12. `12-iter-edges`: `Iter.take(3)` over `Iter.map` over `Iter.range(0, 100)` runs the mapper exactly 3 times per
    run and twice gives 6; `Iter.take(s, 0)` never runs the source; a `return` from inside a `for` over a
    three-stage pipeline leaves the enclosing function (and the sequence's own `defer` runs); a panic in a
    mapper propagates through the stages (separate program); `Iter.filter` whose predicate returns a non-bool
    (`not a bool`, separate program); running a stage by hand with a yield that returns `1` (`not a bool`);
    `Iter.toArray` of a user sequence that ignores `false`; `Iter.fromArray` reading appended elements;
    `Iter.range(0, 3)` run twice; `Iter.take(1, -1)`, `Iter.map(1, f)`, `Iter.map(s, 1)` (`wrong kind`: the
    stage's arguments are checked at the call, G65).
13. `13-result-record` (panics separate): `Result.map(Ok{ value: 1, extra: 2 }, f)` reads `value` by name;
    `Result.mapErr` on `Ok` is a fresh `Ok` (`Record.same` is `false`); `Result.unwrapOr(Err{ error: 1 }, 5)`;
    `Result.unwrap(Err{ error: 1 })` (`unwrap on Err`); `Result.isOk(Error{ message: "x" })` (`wrong kind`);
    `Result.map(Ok{ value: 1 }, 1)` (`wrong kind`); `Record.same(1, 1)` (`wrong kind`); `Record.same` across tags
    (`false`); `Result.ok(void)` renders `Ok{ value: void }`.
14. `14-native-calls`: named arguments to natives (`Array.slice([1, 2, 3], from: 1, to: 2)`,
    `String.replace(s, new: "x", old: "a")`), `Array.slice(xs, 1)` (`missing argument`), `Array.slice(xs, 0, 1, 2)`
    (`too many arguments`), `Array.length(xs, extra: 1)` (`unknown parameter`), `Array.length(xs: [1], xs: [2])`
    (`parameter already filled`), piping into natives (`[3, 1] | Array.sort()`), a native stored in a variable
    and called later, `Math.PI` as a plain field, `print(Math.abs)` (`func`), `panic(1)` (`wrong kind`, G67),
    `print()` (`missing argument`), `print(1, 2)` (`too many arguments`).
15. `15-sync-render`: `print(Mutex.new())`, `RwMutex`, `WaitGroup`, `Once`, `Cond.new(Mutex.new())`,
    `Atomic.new(1)` (each renders as its namespace name), `Mutex.lock(1)` / `WaitGroup.add(wg, 1.0)` /
    `Atomic.add(a, 1.0)` (`wrong kind`), `Mutex.unlock(Mutex.new())` (`not locked`), `Atomic.compareAndSwap`
    with `==` on records (G40), `Once.do` runs once and counts as done after a panic (G30; separate program).
    Anything that would block (`Mutex.lock` twice, `WaitGroup.wait` above zero, `Cond.wait`) panics `deadlock`
    in the 2.1 stub and hangs the bootstrap: do not probe it (step 2.6).

Run everything after steps A and B (`sh work/2.3/probes/run.sh`, a few minutes: each program runs twice under
antpy), save the output as
`work/2.3/probes.log`, and list every `DIFF` in the report.

### D. Settle every DIFF by the spec

For each `DIFF`: find the sentence in `ant-lang.md` section 11/12, `stdlib.md`, or the GAPS entry that decides it.
When the spec supports the bootstrap, fix the runtime (in the file section 5 of the design assigns) and rerun
the probe. When the spec supports the runtime, or is silent and the bootstrap's behaviour is an artefact (as
2.2 found for the bootstrap's `map key too deep` panic and its `float32` crash), keep the runtime and record the
probe, the reading and the reason in the report; `bootstrap/` is closed to this step. Never change a test:
none of the stdlib tests contradicts the spec.

### E. Acceptance, consistency, report

1. `make runtime` (no warnings), then the acceptance command in the background into `work/2.3/test.log`; it must
   exit 0 and end with `0 failures`.
2. The same command with `--consistency` into `work/2.3/consistency.log` (gate 2.G requires it; it is cheap to
   know now). Both runs may overlap: `antc build` names its temporary files by clock and hash.
3. `work/2.3/report.md`: status with the last line of each log; what was built (the two fixes, the table's
   provenance and size, the probe set with its `SAME`/`DIFF` tally and every kept `DIFF` with its reason);
   decisions and deviations from this plan; "Test changes: none"; known limitations carried forward
   (`Map.keys` order is insertion order, unspecified by section 11; the sync stubs' `deadlock` panic until 2.6;
   `String.toLower("İ")` unchanged under the bootstrap's one-code-point rule; sort comparator call counts).
4. The refactor pass of the workflow follows; keep the generated `unicase.c` out of it (regenerate, never edit
   by hand), and keep `case_map`'s ASCII fast path.

## 3. Spec rules of this row that are easy to get wrong (each pinned by a test or a probe above)

- Library functions check their arguments' kinds first, first argument first, and panic `wrong kind` before any
  value-dependent check (`String.repeat(1, -1)` is `wrong kind`, not `negative count`); `panic(1)` is `wrong
  kind` (G67). The value-dependent rows use the canonical phrases of `src/tests/README.md`: `negative count`
  (`Array.filled`, `String.repeat`, `Iter.take`), `empty separator`, `empty pattern`, `index out of range`
  (`Array.slice`/`String.slice`: `from < 0`, `from > to`, or `to > length`), `unwrap on Err`, `not locked`,
  `negative counter`, `shift count` (count `< 0` or `>= width`), `integer overflow` (`Array.sum`, `Math.abs` of
  the most negative integer, `Atomic.add`).
- `Array.sort`: every element must be Ordered (`wrong kind`) before any comparison; elements of different kinds
  then panic `kind mismatch` from `<`. `Array.sortBy` requires every key Ordered, computes keys once in order,
  and is stable; `Array.sortWith`'s comparator must return an `int` (`wrong kind` otherwise), `< 0` puts a before
  b, and the sort is stable (G35). `Math.min`/`max` compare with `<`, so mixed kinds are `kind mismatch`, a tie
  or a NaN yields the first argument.
- Code points everywhere in `String`: `length`, `slice`, `indexOf`, `chars`, `for` over a string; `toBytes`
  gives UTF-8 bytes as `byte` (`uint8`); `fromBytes` is strict (overlong forms, surrogates, > U+10FFFF,
  truncated sequences give `null`) and requires every element to be a `byte`. `split("", sep)` is `[""]`,
  `split(sep, sep)` is `["", ""]`, `indexOf(s, "")` is `0`, `replace` is non-overlapping left to right and never
  rescans; `trim` strips exactly Unicode White_Space (25 code points; not U+200B, not U+FEFF, not U+001C..1F);
  `toInt` is an optional sign and ASCII decimal digits only, `null` on anything else including overflow; `toFloat`
  is sign, digits, optional `.digits`, optional exponent, `null` otherwise and on overflow to infinity (G36).
- Case mapping is one-to-one and context-free (G36): `ß` stays, `ı` becomes `I`, `İ` stays under `toLower`.
- Rendering (G33/G55): a string is bare at the top level and quoted inside containers with exactly `\\ \" \n \t
  \r` escaped; floats are the shortest round-trip digits in JavaScript's layout (exponent form at magnitude
  `>= 1e21` or `< 1e-6`, no `+` in the exponent, always a `.` or an exponent, `-0.0`, `NaN`, `Infinity`,
  `-Infinity`); float32 at float precision (`float32(0.1)` is `0.1`); sized ints as bare digits; `void` as `void`;
  records as `Tag{ a: 1 }` in field order, `Empty{}`; maps as `Map{ k: v }`/`Map{}`; a container being rendered
  renders as `...`; functions `func`, tags their name, sync objects their namespace name. `print` writes the
  rendering and a newline.
- `Array.sum` is `null` on an empty array, folds with `+` (so `kind mismatch` across kinds, `integer overflow`),
  and keeps the element kind. `Array.pop` on empty is `null` (G34). `Array.get` out of range is `null`;
  `xs[i]` panics. `Array.filled` puts the same reference in every slot. `Array.map`/`filter`/... visit elements
  appended by the callback (the length is live, like `for`).
- Maps are keyed by `==` with a consistent hash: NaN keys insert but are never found (G13); `null` is an ordinary
  key; arrays, records and maps work as keys; `Map.map`/`filter` take a snapshot, keep keys, and build a new map;
  `Map.merge` is a new map where `n` wins; `Map.fromEntries` reads `key`/`value` by name (`no field` on a record
  without them, `wrong kind` on a non-record) and later entries win.
- `Iter`: every stage checks its arguments at the call and runs nothing (G65: `take(s, -1)` panics at the call);
  `take(s, 0)` never runs the source (G37); a `false` from downstream travels back to the source through every
  stage; `return`/panic in a `for` body propagates through native stages like through user sequences (the
  stages test the unwind flag after every callback); a sequence may run again.
- `Result.*` require a record tagged `Ok` or `Err` (`wrong kind` otherwise) and read `value`/`error` by name;
  `map`/`mapErr` return a fresh record either way. `Record.same`/`Array.same`/`Map.same` compare pointers;
  `==` never does (G11).
- Natives take named arguments like declared functions (their parameter names are the spec's), and `ant_call`
  applies `too many arguments`/`missing argument`/`unknown parameter`/`parameter already filled` to them.

## 4. Risks

- The generated table: a hand edit or a stale regeneration silently desynchronises it from the bootstrap; the
  header comment must say how it was produced, and the report must state the Python/Unicode versions. Python's
  `str.upper()`/`lower()` are *full* mappings; the one-code-point filter is what makes them the simple mapping
  the bootstrap uses, so keep the filter exactly as stated (do not use `unicodedata` lookups that would differ).
- The float fix must only ever accept a candidate that re-parses to x, and only at the first precision that has
  one; getting the carry wrong (`9.9e1` to `1.0e2`) or testing the lower neighbour would print a wrong number.
  Probe `02` plus the passing `print-float-format` cover both. float32 re-parses with `strtof` where the bootstrap
  parses to double and rounds; a double-rounding difference is theoretically possible but was not found in
  200,000 random floats; if probe `02` shows one, parse with `strtod` and round (`(float)`) to mirror the
  bootstrap.
- `make runtime` is not run by `antc build`: forgetting it tests the old archive. Also run the acceptance only
  after `make runtime` succeeds without warnings (`-Wall -Wextra`).
- The acceptance takes about 2 minutes under antpy; use `--filter` subsets (`/stdlib/str-`, `print-float`,
  `/stdlib/arr-`) while iterating and the full command at each checkpoint and at the end, in the background
  with the log.
- Scope creep: do not touch `emit.ant`, the sync objects beyond kind checks, `Json`, file handles, or `Http`
  (steps 2.6/2.7); do not alter `Map.keys` order or `Array.sortWith`'s comparison count to mimic Python; do not
  add any test-name or output special case.
