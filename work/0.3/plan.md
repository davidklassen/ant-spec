# Plan for step 0.3 "antpy-stdlib"

Deliverable (ROADMAP.md): Array, Map, String (code points), Iter, Math, Bits, Result, Record; rendering of every kind.
Acceptance: the 0.2 filters plus `--filter /stdlib/` (84 stdlib tests: 56 run, 13 panic, 15 unchecked).

## 1. Starting point

Running the acceptance command's `/stdlib/` part against the current tree gives `85 checks, 40 failures`.
Every failure is `panic: no field: <name>` or `panic: unbound name: Math`, i.e. a native missing
from `bootstrap/antpy/prelude.py`. Nothing else is wrong:

- All 7 `print-*` tests, `str-from-kinds`, `template-uses-string-from` pass: `render.py` already
  renders every kind (G33, G55). Do not touch `render.py`.
- Bits (10 functions, in `arith.py`), Result (7), Record.same, and the Array/Map/String functions
  built in 0.1 and 0.2 pass their tests. Do not change their semantics.
- The evaluator (`interp.py`) needs no semantic change; `for` over a function already hands the
  sequence a `YieldFn` and marks it done when the sequence returns, which is exactly what the
  `Iter` stages build on.

Missing natives, by namespace (the name in parentheses is the test that pins it):

| Namespace | Missing |
|---|---|
| Array | `concat`, `indexOf`, `sortBy`, `reverse`, `range`, `filled`, `flatten` |
| Map | `keys`, `values`, `entries`, `fromEntries`, `map`, `filter`, `merge` |
| String | `slice`, `concat`, `split`, `repeat`, `trim`, `contains`, `startsWith`, `endsWith`, `indexOf`, `replace`, `toFloat` |
| Iter | `fromArray`, `map`, `filter`, `take` |
| Math | `abs`, `min`, `max`, `pow`, `sqrt`, `floor`, `ceil`, `round`, `PI`, `E` (the whole namespace) |
| Mutex | `lock`, `unlock` (`unchecked/stdlib/lib-kind-mutex-lock` needs `Mutex.lock(1)` to panic `wrong kind`; 0.4 owns the rest of sync, but these two are three lines and must exist now) |

## 2. Files

| File | Change |
|---|---|
| `bootstrap/antpy/library.py` | **new**: the argument checks (`need_*`), `record_field`, `compare_values`, and the data namespaces Array, Map, String, Iter, Math: every function plus one registry dict per namespace (`ARRAY`, `MAP`, `STRING`, `ITER`, `MATH`) and `MATH_CONSTANTS`. The existing Array/Map/String/Iter functions move here from `prelude.py` unchanged. |
| `bootstrap/antpy/prelude.py` | keeps the prelude scope: tags, conversions, `print`, `panic`, Result, Record, Bits, Mutex, WaitGroup, `namespace()`, `NAMESPACES`, `make_prelude()`. Imports the checks and registry dicts from `library.py`. Shrinks to roughly 250 lines. |
| `bootstrap/antpy/values.py` | the prelude tags `OK`, `ERR`, `ERROR`, `ENTRY`, `INDEXED` move here next to `MODULE_TAG` (`library.py` needs `ENTRY`/`INDEXED`, `prelude.py` needs all five; neither may import the other for them). `prelude.py` re-imports them so `prelude.OK`, `prelude.ERR`, `prelude.ENTRY` in `interp.py` keep working. |
| `bootstrap/antpy/interp.py` | three lines: `from antpy.library import record_field`; in `ev_Try` replace the two `prelude.result_payload(v, ...)` calls with `record_field(v, ...)`. Nothing else. |
| `bootstrap/antpy/__init__.py` | add `library` to the module list in the docstring. |
| `work/0.3/report.md`, `work/0.3/test.log` | the report and the acceptance log. |

Why the split: `prelude.py` is 554 lines before this step and would pass 1000 after it; 0.4 (sync)
and 0.6 (File/Json/Time/Os) add more. `library.py` holds "functions over data values", `prelude.py`
"the scope and what is bound in it". No subpackage, no per-namespace files (names like `string.py`
and `math.py` collide with the standard library in people's heads).

Import graph after the change (no cycles): `values` ← `equal` ← `arith`, `render` ← `library` ← `prelude` ← `interp`.

## 3. Approach

### 3.1 Restructure first (step 0)

1. Move the five tags into `values.py`; in `prelude.py` write `from antpy.values import OK, ERR, ERROR, ENTRY, INDEXED` (keep the names exported).
2. Create `library.py`; move `need`, `need_array`, `need_map`, `need_string`, `need_int`, `need_record`, `need_number`, `need_func`, `need_bool_result`, `compare_values`, and every `array_*`, `map_*`, `string_*`, `case_map`, `INT_LITERAL`, `iter_*` function into it. Leave `need_result`, `result_*`, `record_same`, `mutex_with_lock`, `wait_group_*`, `ant_print`, `ant_panic` in `prelude.py`.
3. Rename `result_payload` to `record_field(r, name)` in `library.py` (same body: a field read that panics `no field: <name>` on a missing field, section 12 `a.b`). `prelude.py`'s Result natives and `interp.py`'s `ev_Try` call it.
4. Add two helpers to `library.py`: `need_float64(v) = need(v, float, "a float64")` (note `Float32` is a different Python type, so a `float32` argument is `wrong kind`, as the `float64` signatures say) and `need_ordered(v)` (a number or a string, else `wrong kind: expected a number or a string`).
5. Registry dicts at the bottom of each section of `library.py`, in the existing `(params, fn)` / `(params, fn, True)` form; `prelude.py`'s `NAMESPACES` becomes `{"Array": ARRAY, "Map": MAP, "String": STRING, "Iter": ITER, "Math": MATH, "Bits": {...}, "Result": {...}, "Record": {...}, "Mutex": {...}, "WaitGroup": {...}}`.
6. `namespace(name, functions, constants=None)`: after the natives, `fields.update(constants or {})`. `make_prelude` passes `MATH_CONSTANTS` for `"Math"` (a module-level `CONSTANTS = {"Math": MATH_CONSTANTS}` lookup keeps it data-driven).
7. Run the full 0.2 acceptance (the 0.3 command without `--filter /stdlib/`): must still be `264 checks, 0 failures`. Only then add functions.

### 3.2 Data structures and conventions

- Values stay as 0.1 defined them: array = Python `list`, map = `AntMap` (`equal.py`), string = `str` (Python indexes by code point, which is what section 11 asks), records = `Record(tag, dict)`, a `Seq` = any function value; a sequence the library builds is a `Native(name, ["yield"], run, ctx=True)` exactly like the existing `iter_range`.
- Every native checks its arguments' kinds first (`wrong kind`), then the value-dependent panics, then does the work. Kind checks use `type(v) is ...` (never `isinstance`: `bool` is an `int` subclass, `Float32` a `float` subclass).
- Every non-`mutates` function returns a **new** list/map (`xs + ys`, `xs[::-1]`, `sorted(...)`, `[...]`, a fresh `AntMap`): the tests assert `Array.same`/`Map.same` is false on results.
- Registry parameter names are section 11's, so named arguments resolve: `xs`, `ys`, `i`, `v`, `from`, `to` (strings in the registry; Python functions use `start`/`end` since `from` is a keyword), `f`, `init`, `key`, `cmp`, `n`, `m`, `k`, `entries`, `s`, `sep`, `parts`, `t`, `old`, `new`, `bytes`, `value`, `a`, `b`, `x`, `y`.
- Natives that call back into Ant are `ctx=True` and receive `(interp, frame, *args)`; call with `interp.call(f, [arg], (), frame)`.
- An `Entry` record is `Record(ENTRY, {"key": k, "value": v})`; write one helper `entry(k, v)` and use it in `Map.entries/map/filter` (and `interp.ev_For` may stay as it is).

### 3.3 Math (step 1; tests `math-*`, `lib-kind-math*`)

```
abs(x)      need_number; integer kinds: x if x >= 0 else arith.neg(x)  (neg panics `integer overflow` on the
            most negative value and keeps the kind); float kinds: type(x)(math.fabs(x)) (clears the sign bit:
            abs(-0.0) is 0.0, G56; Float32 stays Float32)
min(a, b)   need_ordered both; return b if arith.binary("<", b, a) else a
max(a, b)   need_ordered both; return b if arith.binary("<", a, b) else a
            (`arith.binary` panics `kind mismatch` on mixed kinds, the same split 0.2 chose for Array.sum
            and that panic/types/gen-union-solution-escapes-bound expects of Array.sort; ties and NaN give a)
pow(x, y)   need_float64 both; math.pow in try: OverflowError -> inf with the sign of x when x < 0 and y is an
            odd integer, else +inf; ValueError -> for x == 0.0 (zero to a negative power) +inf, or -inf when x
            is -0.0 and y an odd integer, otherwise NaN (negative base, fractional exponent)
sqrt(x)     need_float64; math.sqrt, ValueError -> NaN (sqrt(-0.0) is -0.0 and inf/NaN pass through already)
floor(x)    need_float64; if not math.isfinite(x): return x; r = float(math.floor(x)); return math.copysign(r, x) if r == 0.0 else r
ceil(x)     same with math.ceil (ceil(-0.5) is -0.0)
round(x)    half away from zero: if not finite return x; ax = abs(x); r = math.floor(ax);
            if ax - r >= 0.5: r += 1; return math.copysign(float(r), x)
PI, E       MATH_CONSTANTS = {"PI": math.pi, "E": math.e}, plain floats on the namespace record
```

Wrong ways that look right: Python's `round()` is half-to-even (`round(2.5) == 2`); `math.floor(ax + 0.5)`
fails the test's `0.49999999999999994` (the sum is exactly `1.0`); `math.floor`/`ceil` raise on inf/NaN and
return `int`, which would print as `2` instead of `2.0`; `x ** y` returns a complex number for a negative
base and raises on overflow. `ax - math.floor(ax)` is exact for doubles, so the comparison with 0.5 is safe.

### 3.4 String (step 2; tests `str-*`, `lib-kind-string*`)

```
slice(s, from, to)      need_string, need_int both; require 0 <= from <= to <= len(s) else `index out of range`
                        (the same rule as array_slice; share one `check_slice(length, start, end)`); s[from:to]
concat(parts)           string_join(parts, "")
split(s, sep)           strings; sep == "" -> `empty separator`; s.split(sep)   (gives [""] for "", ["", ""] for ",")
repeat(s, n)            need_string, need_int (1.0 is `wrong kind`); n < 0 -> `negative count`; s * n
trim(s)                 s.strip(WHITE_SPACE) with the Unicode White_Space property spelled out as a constant:
                        "\t\n\v\f\r \x85\xa0                　"
                        (not bare str.strip(): Python also strips U+001C..U+001F, which are not White_Space;
                        U+200B is not White_Space and the test checks it stays)
contains(s, t)          t in s;  startsWith: s.startswith(t);  endsWith: s.endswith(t)   (empty t is true)
indexOf(s, t)           i = s.find(t); None if i < 0 else i   (code point index; find("") is 0, G36)
replace(s, old, new)    old == "" -> `empty pattern`; s.replace(old, new)   (non-overlapping, left to right, not rescanned)
toFloat(s)              re.fullmatch(r"[+-]?[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?", s) else null; v = float(s);
                        null when not math.isfinite(v) (overflow, like toInt and G1)
```

All of these also `need_string` every string argument before anything else. `toFloat` must gate on the
regex: Python's `float()` accepts `" 1.0"`, `"inf"`, `"nan"`, `"1_0"`, `".5"`, which are all `null` here.

### 3.5 Array (step 3; tests `arr-*`, `lib-kind-array*`)

```
concat(xs, ys)      need_array both; xs + ys
indexOf(xs, v)      first i with eq(xs[i], v), else null   (NaN is never found)
sortBy(xs, key)     need_array, need_func; keys = [need_ordered(call(key, [x])) for x in xs]  (key runs once per
                    element, in order); order = sorted(range(len(xs)), key=cmp_to_key(lambda i, j: compare_values(keys[i], keys[j])));
                    [xs[i] for i in order]   (sorted is stable: equal keys keep their order, G35)
sort(xs)            add need_ordered on every element before the existing sorted(...) (G15's test still gets
                    `kind mismatch` from the comparison of 1 and "a"; [true, false] becomes `wrong kind`)
reverse(xs)         xs[::-1]
range(from, to)     need_int both; list(range(from, to))   (empty when to <= from)
filled(n, v)        need_int(n); n < 0 -> `negative count`; [v] * n   (the same v in every slot: the aliasing test)
flatten(xs)         need_array; need_array on every element (`wrong kind`); [y for x in xs for y in x]  (one level)
```

### 3.6 Map (step 4; tests `map-*`, `lib-kind-map*`)

```
keys(m)             [k for k, _ in need_map(m).items()];   values: the v's;   entries: [entry(k, v) ...]
fromEntries(es)     need_array; out = AntMap(); for e in es: need_record(e); out.set(record_field(e, "key"), record_field(e, "value"))
                    (later entries win because set replaces)
map(m, f)           need_map, need_func; out.set(k, call(f, [entry(k, v)])) for each item
filter(m, f)        keep the entries whose need_bool_result(call(f, [entry(k, v)])) is true, in a new map
merge(m, n)         need_map both; new map: all of m, then all of n (n wins)
```

`AntMap.items()` is a snapshot list, so a callback that mutates the map does not break the loop.

### 3.7 Iter (step 5; tests `iter-*`, `lib-kind-iter`)

A sequence is a function taking `yield`. Each `Iter` function checks its arguments at the call, then returns a
`Native(..., ["yield"], run, ctx=True)` whose `run(interp, frame, y)` does `need_func(y)` and drives the source
with a fresh `YieldFn`. Write one helper and use it everywhere, including `iter_to_array`:

```python
def drive(interp, frame, source, body):
    """Runs `source` with a fresh yield whose body(v) returns True to continue; the yield is done
    once the source returns (section 12, `for`), so a late call panics `yield after done`."""
    y = YieldFn(body)
    try:
        interp.call(source, [y], (), frame)
    finally:
        y.done = True
    return VOID
```

```
fromArray(xs)   need_array at the call; run: i = 0; while i < len(xs): v = xs[i]; i += 1; if not call(y, [v]): break
                (re-reads the length like `for`: the test pushes after fromArray and expects the new element)
map(s, f)       need_func both at the call; run: drive(s, lambda v: need_bool_result(call(y, [call(f, [v])])))
filter(s, f)    run: drive(s, body) where body calls f, needs a bool, and returns call(y, [v]) (as a bool) when
                true, else True
take(s, n)      need_func, need_int at the call; n < 0 -> `negative count` at the call (G65's proposal; the test
                runs the sequence, so either placement passes); run: if n == 0: return VOID without touching
                the source (G37); else left = [n]; body: left[0] -= 1; more = need_bool_result(call(y, [v]));
                return more and left[0] > 0   (false right after the nth element, so a huge range stops and a
                mapper upstream runs exactly n times)
toArray(s)      existing behaviour, rewritten over drive
range           unchanged
```

Nothing may run the source at construction: `iter-lazy` counts mapper calls and expects 0 until
`toArray`. `iter-break-through-stages` checks that a `break` in the `for` body (the loop's yield returns
false) travels back through take, filter and map to the source as `false` from each stage's yield, so
`produced` is 5; this falls out of returning the downstream result unchanged. Pipes already work:
`source | Iter.map(f)` lowers to `Iter.map(source, f)` and `| Iter.toArray()` to `Iter.toArray(seq)`.

### 3.8 Mutex.lock / Mutex.unlock (step 6; `lib-kind-mutex-lock`)

In `prelude.py` next to `mutex_with_lock`: `lock(m)`: `need(m, Mutex, "a Mutex")`, `m.lock.acquire()`
(blocking; the GIL is released while it waits). `unlock(m)`: same check, `m.lock.release()` inside
`try/except RuntimeError: panic("not locked")`. Same `m.lock` that `withLock` holds. 0.4 adds the rest.

### 3.9 Finish (steps 7 and 8)

- Full acceptance command (verbatim from the task); then the same with `--consistency`; both must end in
  `0 failures`. Save the acceptance output to `work/0.3/test.log`.
- `ruff check --select F401,F821,F841 bootstrap/` (as 0.1 and 0.2 did) must be clean.
- `work/0.3/report.md`: what was built (table per namespace), the acceptance line, test changes (expected: none),
  decisions, the untested choices below, known limitations for 0.4.

Per-step test commands (filters are substrings of the test path, OR-ed):

```
RUN="python3 -I src/tests/run.py --run \"python3 -I bootstrap/antpy.py run {file} {args}\" --run-unchecked \"python3 -I bootstrap/antpy.py run --unchecked {file} {args}\" --tier run --tier panic --tier unchecked"
$RUN --filter /stdlib/math --filter lib-kind-math
$RUN --filter /stdlib/str- --filter lib-kind-string
$RUN --filter /stdlib/arr- --filter lib-kind-array
$RUN --filter /stdlib/map- --filter lib-kind-map
$RUN --filter /stdlib/iter- --filter lib-kind-iter
$RUN --filter lib-kind-mutex
```

## 4. Spec rules that are easy to get wrong

1. **Kind checks before value checks, library phrase `wrong kind`.** `Math.sqrt(4)` (an `int`), `String.repeat("a", 1.0)`, `Array.slice([1], 0.0, 1.0)`, `Mutex.lock(1)` are all `wrong kind`, even though the value would be fine. `bool` is not `int` (`type(True) is int` is false, good); `Float32` is not `float`.
2. **Mixed kinds inside an Ordered argument are `kind mismatch`, not `wrong kind`**: `panic/types/gen-union-solution-escapes-bound` pins this for `Array.sort([1, "a"])`; `Math.min/max` and `sortBy` keys follow the same rule by comparing through `arith.binary("<")`.
3. **Code points, never bytes**: lengths, `slice`, `indexOf`, `chars` count code points; Python `str` does, as long as nothing encodes to UTF-8 along the way.
4. **Canonical phrases** (README table): `negative count` (`Array.filled`, `String.repeat`, `Iter.take`), `empty separator` (`split`), `empty pattern` (`replace`), `index out of range` (`Array.slice`, `String.slice`, including `from > to` and `to > length`), `integer overflow` (`Math.abs`, `Array.sum`), `not locked`. The runner checks substrings; detail after a colon is fine.
5. **`Array.filled` aliases**: the same `v` in every slot; do not copy.
6. **Laziness and G37**: no `Iter` function runs its source at construction; `take(s, 0)` never runs it at all, not even one element; `take(s, -1)` panics `negative count`.
7. **Stability**: `sortBy` and `sortWith` keep the order of equal elements (Python's `sorted` is stable; sorting indices with precomputed keys keeps it so and calls `key` once per element).
8. **Half away from zero** for `Math.round`, and `floor`/`ceil`/`round` return `float64` (print as `2.0`), pass inf/NaN through, and keep the sign of a zero result.
9. **`Math.abs(-0.0)` is `+0.0`** (G56): `math.fabs`, not `-x if x < 0`.
10. **`String.trim` is Unicode White_Space**: NBSP and U+3000 are trimmed, U+200B is not.
11. **`toFloat`/`toInt` are strict**: no surrounding whitespace, no `_`, no radix prefix, `null` on overflow.
12. **New values**: `concat`, `reverse`, `sort*`, `slice`, `map`, `filter`, `merge`, `flatten`, `toArray` return fresh arrays/maps; `push`, `pop`, `set`, `remove` are the only mutators.
13. **`Map.fromEntries` and `Map.merge`: later/second wins**; `Map.map` keeps every key.
14. **`null`/NaN keys** (G13) are already handled by `AntMap`; `Array.indexOf`/`contains` use `eq`, so NaN is never found.
15. **No special-casing of tests**; the implementation must not read test names or files.

## 5. Untested choices to record in the report

- `Math.min/max` on a tie or with NaN return the first argument; on two bools `wrong kind`; on `1` and `1.0` `kind mismatch`.
- `Math.pow` and `Math.sqrt` follow IEEE/C (`pow(0.0, -1.0)` is `Infinity`, `sqrt(-1.0)` is `NaN`, never a panic).
- `String.toFloat` rejects `".5"`, `"5."`, `"inf"`, `"nan"`, and returns `null` for a finite literal that overflows to infinity.
- `String.trim` strips exactly the Unicode White_Space set (25 code points).
- `Iter.take` panics `negative count` at the call, not at the first run (G65 proposal), and stops the source immediately after the nth element.
- `Iter.map/filter/take` check that their arguments are functions at the call; a yield that returns a non-bool panics `not a bool`.
- `Array.sort` on elements that are neither numbers nor strings panics `wrong kind` even for a single element (the pre-check precedes any comparison).
- `Array.flatten` with a non-array element panics `wrong kind`; `Map.fromEntries` with a non-record element `wrong kind`, with a record lacking `key`/`value` `no field: key`.
- `Mutex.unlock` on an unlocked mutex panics `not locked`; `lock` is not reentrant.

## 6. Risks

- **The module split**: a missed import breaks every test at once; it is pure moves and is verified by rerunning the 0.2 acceptance before any new function is written (section 3.1 step 7). `interp.py` changes by three lines only.
- **Iter stage bookkeeping**: a stage that forgets to return the downstream yield's result (or returns `True` after a `break`) makes `iter-break-through-stages` count 100 produced elements; a stage that sets `done` too early makes a re-run of the same `Seq` (`iter-range-fromArray-toArray` runs a range twice) panic `yield after done`. `drive` creates a fresh `YieldFn` per run, which avoids the second.
- **Floating point corner cases** in `Math.round`/`floor`/`ceil`/`pow`: the Python functions raise or return `int` where IEEE returns a float; every one is wrapped as in section 3.3. An uncaught Python exception surfaces as `panic: internal interpreter error`, which the panic tests would flag as the wrong phrase.
- **`String.toFloat` without the regex gate** accepts Python's wider syntax and fails `str-toFloat` (`" 1.0"` must be `null`).
- **Changing `Array.sort`** to pre-check elements must not alter G15's `kind mismatch`; `[1, "a"]` passes the pre-check (both Ordered) and still panics in the comparison.
- `Mutex.lock` on a daemon thread that never unlocks blocks only that goroutine; `main` returning still exits the process via `os._exit`.
