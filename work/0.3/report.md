# Report for step 0.3 "antpy-stdlib"

## What was built

The starting tree failed 40 of the 85 `/stdlib/` checks, every one a missing native (`no field: <name>` or `unbound name: Math`). As the plan says, rendering (`render.py`), Bits, Result, and Record already passed and were not touched. The step adds the missing natives and restructures the library into a module of its own:

| File | Change |
|---|---|
| `bootstrap/antpy/library.py` | **new** (813 lines): the argument checks (`need_*`, `record_field`, `compare_values`, `check_slice`), and the data namespaces Array, Map, String, Iter, Math with one registry dict each (`ARRAY`, `MAP`, `STRING`, `ITER`, `MATH`, plus `MATH_CONSTANTS`). The 0.1/0.2 functions moved here unchanged. |
| `bootstrap/antpy/prelude.py` | 554 to 222 lines: keeps the scope (`make_prelude`, `namespace`, `NAMESPACES`, `CONSTANTS`), Result, Record, Bits, Mutex (now with `lock`/`unlock`), WaitGroup, `print`, `panic`. `namespace()` takes an optional `constants` dict so `Math.PI`/`Math.E` are plain fields. |
| `bootstrap/antpy/values.py` | the five prelude tags (`OK`, `ERR`, `ERROR`, `ENTRY`, `INDEXED`) moved next to `MODULE_TAG`; `prelude.py` re-imports them, so `prelude.OK`/`ERR`/`ENTRY` in `interp.py` keep working. |
| `bootstrap/antpy/interp.py` | three lines: `ev_Try` reads payloads through `library.record_field` (the renamed `result_payload`). No semantic change. |
| `bootstrap/antpy/__init__.py` | the module list in the docstring. |

Import graph (no cycles): `values` <- `equal` <- `arith`, `render` <- `library` <- `prelude` <- `interp`.

The natives added, with their semantics:

| Namespace | Added | Semantics |
|---|---|---|
| Math | `abs`, `min`, `max`, `pow`, `sqrt`, `floor`, `ceil`, `round`, `PI`, `E` | `abs`: integers keep their kind and negate through `arith.neg` (so the most negative value panics `integer overflow`); floats use `math.fabs` (clears the sign bit, G56; `float32` stays `float32`). `min`/`max`: both arguments must be a number or a string (`wrong kind`), compared with the `<` operator (`kind mismatch` across kinds, like `Array.sort`); a tie or a NaN gives the first argument. `pow`/`sqrt`: `float64` only (`float32` and `int` are `wrong kind`); Python's `OverflowError`/`ValueError` are mapped to IEEE results (`pow(0.0, -1.0)` is `Infinity`, `pow(-0.0, -1.0)` is `-Infinity`, `pow(-8.0, 0.5)` and `sqrt(-1.0)` are `NaN`, `pow(-10.0, 401.0)` is `-Infinity`); never a panic. `floor`/`ceil`/`round` return `float64`, pass inf/NaN through, keep the sign of a zero result (`ceil(-0.5)` is `-0.0`); `round` is half away from zero via `floor(abs(x))` plus a `>= 0.5` test on the exact fraction, so `round(0.49999999999999994)` is `0.0` and `round(-2.5)` is `-3.0`. |
| String | `slice`, `concat`, `split`, `repeat`, `trim`, `contains`, `startsWith`, `endsWith`, `indexOf`, `replace`, `toFloat` | All indices are code points (Python `str`). `slice` shares `check_slice` with `Array.slice` (`index out of range` for `from > to` or `to > length`). `split` panics `empty separator`, gives `[""]` for `""` and `["", ""]` for a lone separator (G36). `repeat` needs an `int` (`1.0` is `wrong kind`) and panics `negative count`. `trim` strips exactly the 25 Unicode White_Space code points (a spelled-out constant, not `str.strip()`, which also strips U+001C..U+001F; U+200B stays). `indexOf("")` is `0`. `replace` panics `empty pattern`; non-overlapping, left to right, never rescanned. `toFloat` is gated by `[+-]?[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?` and returns `null` when the value is not finite. |
| Array | `concat`, `indexOf`, `sortBy`, `reverse`, `range`, `filled`, `flatten` | `concat`/`reverse`/`sortBy`/`flatten` return new arrays. `indexOf` uses `==` (a NaN is never found); `contains` is now `indexOf(...) != null`. `sortBy` calls `key` once per element in order, requires every key to be Ordered, and sorts the indices by the precomputed keys, so it is stable (G35). `sort` now pre-checks every element with `need_ordered` (`[true]` is `wrong kind`), and `[1, "a"]` still panics `kind mismatch` in the comparison (G15's test). `range` is empty when `to <= from`. `filled` panics `negative count` and puts the same `v` in every slot. `flatten` is one level and requires every element to be an array. |
| Map | `keys`, `values`, `entries`, `fromEntries`, `map`, `filter`, `merge` | One helper `entry(k, v)` builds `Entry` records. `fromEntries` needs an array of records (`wrong kind`), reads `key`/`value` through `record_field` (`no field: key`), later entries win. `map` keeps every key, `filter` keeps the entries whose predicate is `true` (`not a bool` otherwise), `merge` copies `m` then `n` (n wins); all three build a new `AntMap`. `AntMap.items()` is a snapshot, so a callback that mutates the map does not break the loop. |
| Iter | `fromArray`, `map`, `filter`, `take` | Every stage checks its arguments at the call and returns a `Native` of `yield` that runs its source only when run itself. One driver `drive(interp, frame, source, body)` runs a source with a fresh `YieldFn` per run (so a `Seq` can be run again) and marks it done when the source returns. Each stage's body returns what the downstream yield returned, so a `break` travels back through every stage (`iter-break-through-stages`: `produced == 5`). `fromArray` re-reads the array's length like `for`. `take(s, -1)` panics `negative count` at the call (G65); `take(s, 0)` never runs the source (G37); otherwise the body returns `false` right after the nth element, so a huge range stops and an upstream mapper runs exactly n times. `toArray` is rewritten over `drive`. |
| Mutex | `lock`, `unlock` | On the same `threading.Lock` that `withLock` holds. `lock` blocks (not reentrant); `unlock` of an unlocked mutex panics `not locked`. |

## Acceptance

Command (verbatim from the task):

    python3 -I src/tests/run.py --run "python3 -I bootstrap/antpy.py run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /functions/ --filter /expressions/ --filter /types/ --filter /nullability/ --filter /errors/ --filter /stdlib/

Exit status 0; full output in `work/0.3/test.log` (git-ignored); last line:

    348 checks, 0 failures, 0 warnings (implementation mode, 347 tests)

The same command with `--consistency` added also ends in `348 checks, 0 failures, 0 warnings (implementation mode, 347 tests)`.

Intermediate checkpoints, in the order of the plan: after the pure restructure, the 0.2 acceptance (the command above without `--filter /stdlib/`) was `264 checks, 0 failures`; then per section: Math `10 checks`, String `22 checks`, Array `25 checks`, Map `9 checks`, Iter `8 checks`, Mutex `2 checks`, all `0 failures`.

`ruff check --select F401,F821,F841 bootstrap/` reports nothing.

Side effect outside this step's filters: `run/concurrency/mutex-withlock-returns-and-releases` (0.4's section), which 0.2 reported as failing only at `Mutex.lock`, now passes.

## Test changes

None. `work/TEST-CHANGES.md` is unchanged.

## Decisions and deviations from the plan

1. **`Array.contains` reuses `Array.indexOf`** (`array_index_of(xs, v) is not None`) instead of keeping its own loop. Same behaviour, one loop fewer.
2. **The library's sequences are named `Iter.<name>`** through a small `stage(name, run)` helper (the plan's `iter_range` named its native bare `range`). The name is only descriptive; natives have no frame in traces.
3. **Registries are in section 11's order** rather than the order the functions were added, so a reader can check them against the spec top to bottom. Pure reordering.
4. **`WHITE_SPACE` is spelled with `\u` escapes**, not literal characters, so the constant is readable in any editor.
5. `interp.ev_For` still builds its `Entry` with `prelude.ENTRY` rather than `library.entry`; the plan allowed either and I kept `interp.py` at the three planned lines.
6. Nothing else deviates. `Mutex.lock`/`unlock` live in `prelude.py` next to `withLock`, as planned; `Array.sort` got the `need_ordered` pre-check; `toFloat` is regex-gated; `Math.round` avoids both `round()` (half to even) and `floor(x + 0.5)`.

## Untested choices (plan section 5), verified by probes

Each line is one program run under `python3 -I bootstrap/antpy.py run`:

- `Math.min(1, 1.0)`: `kind mismatch`; `Math.min(true, false)`: `wrong kind: expected a number or a string`; `Math.min(1.0, NaN)` is `1.0`, `Math.max(NaN, 1.0)` is `NaN` (the first argument); `Math.min(4, 4)` is `4`.
- `Math.floor(float32(1.5))`: `wrong kind: expected a float64`. `Math.abs(uint8(0))` is `0`, `Math.abs(float32(-1.5))` is `1.5` and stays `float32`.
- `Math.pow(0.0, -1.0)` is `Infinity`, `Math.pow(-0.0, -1.0)` is `-Infinity`, `Math.pow(-8.0, 0.5)` is `NaN`, `Math.pow(10.0, 400.0)` is `Infinity`, `Math.pow(-10.0, 401.0)` is `-Infinity`, `Math.sqrt(-1.0)` is `NaN`; `Math.floor(Infinity)` is `Infinity`, `Math.round(1e300)` is `1e300`, `Math.ceil(-0.5)`, `Math.floor(-0.0)`, and `Math.round(-0.4)` are `-0.0`.
- `String.toFloat` returns `null` for `".5"`, `"5."`, `"inf"`, `"nan"`, `"1_0"`, `"1e999"` (overflow) and `-0.0` for `"-1e-400"` (underflow is finite). `String.trim("\u{1C}x\u{1F}")` keeps all three code points; U+0085, U+1680, U+2000..U+200A, U+2028, U+2029, U+202F, U+205F are trimmed.
- `String.split("abc", "abc")` is `["", ""]`; `String.indexOf("aaa", "aa")` is `0`; `String.replace("aaa", "a", "")` is `""`; `String.slice("abc", 2, 1)` is `index out of range`; `String.contains("a", 1)` is `wrong kind: expected a string`.
- `Array.sort([true])`: `wrong kind` (the pre-check precedes any comparison); `Array.sort([1, "a"])`: `kind mismatch`; `Array.sortBy([1, 2], { true })`: `wrong kind`; `Array.sortWith` with a comparator returning `1.0`: `wrong kind: expected an int`; `Array.flatten([1])`: `wrong kind: expected an array`; `Array.range(0, 2.0)`: `wrong kind: expected an int`; `Array.concat([1], Map{})`: `wrong kind: expected an array`.
- `Map.fromEntries([1])`: `wrong kind: expected a record`; `Map.fromEntries([Ok{}])`: `no field: key`; `Map.merge(m, [])`: `wrong kind: expected a map`; `Map.entries(Map{ null: 1 })` is `[Entry{ key: null, value: 1 }]`; `Map.merge(m, Map{})` is a new map equal to `m`.
- `Iter.take(s, -1)` panics `negative count` at the call, before any run. `Iter.map(1, f)`: `wrong kind: expected a function`. Running a stage with a yield that returns a non-bool (`s({ 1 })`): `not a bool`. A `take(3)` over a mapped `range(0, 100)` runs the mapper exactly 3 times per run and can be run twice (6 calls).
- `Mutex.unlock` of an unlocked mutex: `not locked`; `lock` then `unlock` on one goroutine works; `lock` is not reentrant.

## Known limitations

- `Map.keys`/`values`/`entries` return the bucket order of `AntMap`, which section 11 leaves unspecified; tests sort before comparing.
- `Mutex.lock` blocks the calling thread; a goroutine that never unlocks blocks only itself, and `main` returning still exits the process (`os._exit`). `RwMutex`, `Once`, `Cond`, `Atomic`, and `Time.sleep` arrive in 0.4; `File`, `Json`, `Time`, `Os` in 0.6.
- `String.toFloat` follows C `strtod` on underflow (a finite denormal or signed zero, not `null`); only overflow to infinity is `null`, matching `toInt`'s overflow rule (G36).

## Refactor

A single-pass `simplify` review (the skill's four-agent fan-out was unavailable, so the four angles were worked through inline), then the reviewer's three suggestions, each followed by a `--filter` subset run and the full acceptance command at the end. No behaviour changed; the acceptance command still ends in `348 checks, 0 failures, 0 warnings (implementation mode, 347 tests)` (`work/0.3/test.log` is the post-refactor run), with `--consistency` as well, and `ruff check --select F401,F821,F841 bootstrap/` is clean.

From the simplify pass:

- `prelude.namespace(name, members)` takes one registry: a tuple spec becomes a `Native`, any other value is a constant field. `Math.PI` and `Math.E` are now plain entries of `MATH`; `MATH_CONSTANTS`, `CONSTANTS`, and the `constants=` parameter are gone (this supersedes the `CONSTANTS`/`MATH_CONSTANTS` mentions in the file table above).
- `interp.ev_For` runs a sequence through `library.drive` instead of its own `YieldFn` + try/finally copy, so "run a source with a fresh yield and mark it done" has one definition shared with the `Iter` stages.
- `interp.destructure` reads each field through `library.record_field`.
- `interp.py` imports `OK` and `ERR` from `values.py`, where they are defined, instead of through `prelude`'s re-export; `prelude` is now imported only for `make_prelude`.
- `library.iter_take` counts down with a `nonlocal` instead of a one-element list.

Skipped: a `need_mutex`/`need_wait_group` pair (three and two uses; 0.4 adds the other sync objects and is the place to settle that), and factoring the `Iter` stages' shared `run` shape (an abstraction for three functions).

From the review:

1. `WHITE_SPACE` is spelled entirely with `\x`/`\u` escapes (decision 4 above now holds; the set was checked equal to the previous literal: 25 code points).
2. `interp.ev_For`'s map branch builds its `Entry` with `library.entry(k, v)` (decision 5 above is superseded).
3. `Interp.field` keeps its `not a record` check, then reads through `record_field`, then checks `UNINIT`, so section 12's `no field` rule lives in one place.
