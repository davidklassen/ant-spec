# Report for step 0.2 "antpy-types-errors"

## What was built

The deliverable's semantics (tags, `is` with kind names, structural `==` with cycles, spread, `?.` `??`, `try`, `defer`, unwinding, replacing panics, the prelude record tags, conversions) were built in 0.1 and already passed every selected `types`, `nullability`, and `errors` test; the 15 failures were all `panic: no field: <name>`, a library native missing from `prelude.py`. As the plan says, the evaluator was not touched. The only code change is `bootstrap/antpy/prelude.py` (415 to 554 lines):

| Native | Semantics |
|---|---|
| `Array.sum(xs)` | `null` on an empty array; every element must be a number (`wrong kind`); then a fold with the `+` operator (`arith.binary`), so an overflow panics `integer overflow` and elements of different numeric kinds `kind mismatch`; the result keeps the elements' kind (`int8` in, `int8` out; `float32` rounds per step) |
| `Map.isEmpty(m)`, `Map.remove(m, k)`, `Map.same(m, n)` | size test; `AntMap.remove` with the internal `MISSING` turned into `null` (a NaN key is never found, G13; a stored `null` comes back as `null`); identity |
| `String.isEmpty(s)` | `s == ""` |
| `String.toUpper(s)`, `String.toLower(s)` | one helper `case_map(s, fn)`: a code point is replaced only when `str.upper`/`str.lower` gives exactly one code point, otherwise kept (G36: `"ß"` stays, `"é"` becomes `"É"`, `"ﬁ"` and `"İ"` stay). `toLower` is not in this step's failures but is the twin of the helper and run/stdlib/str-case tests the pair |
| `String.fromBytes(bytes)` | every element must be a `UInt8` (`wrong kind` for a plain `int`); strict UTF-8 decode, `null` on `UnicodeDecodeError` (never a replacement character); an empty array decodes to `""` |
| `String.toInt(s)` | `re.fullmatch(r"[+-]?[0-9]+")` (ASCII digits only, no surrounding whitespace, no `_`, no radix prefix, no trailing newline), then `int()`, then `null` when outside the `int64` range |
| `Result.map(r, f)`, `Result.mapErr(r, f)` | `f` runs on exactly one branch; both branches return a fresh `Ok`/`Err` record (section 11: a non-mutating function returns a new value), as `try` does |
| `Result.unwrapOr(r, fallback)` | `value` of an `Ok`, else `fallback` |
| `Mutex.withLock(m, f)` | acquire `m.lock`, call `f` with no argument (its `x` is `void`, G23), release in `finally`; a panic in `f` propagates after the release |

Two small helpers: `need_number(v)` (the `wrong kind` check for numeric kinds, used by `Array.sum`) and `result_payload(r, name)`, which reads `value`/`error` with a `no field: <name>` panic instead of a Python `KeyError` when an unchecked program builds an `Ok{}` without a payload; `Result.unwrap` now uses it too. The registry parameter names are section 11's (`xs`, `m`, `k`, `n`, `s`, `bytes`, `r`, `f`, `fallback`) so named arguments resolve (`Result.unwrapOr(r, fallback: 0)` verified).

## Acceptance

Command (verbatim from the roadmap):

    python3 -I src/tests/run.py --run "python3 -I bootstrap/antpy.py run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /functions/ --filter /expressions/ --filter /types/ --filter /nullability/ --filter /errors/

Exit status 0; full output in `work/0.2/test.log`; last line:

    264 checks, 0 failures, 0 warnings (implementation mode, 263 tests)

The same command with `--consistency` added also ends in `264 checks, 0 failures, 0 warnings (implementation mode, 263 tests)`.

The 0.3 tests that pin these natives (plan step 4.3: `--tier run --tier panic --filter arr-sum --filter str-toInt --filter str-case --filter str-fromBytes --filter str-bytes-roundtrip --filter map-set-remove --filter map-same --filter map-size-isEmpty --filter str-length-isEmpty`): `12 checks, 0 failures, 0 warnings (implementation mode, 11 tests)`, including both `arr-sum-overflow` panic tests and all six invalid UTF-8 cases.

`ruff check --select F401,F821,F841 bootstrap/` reports nothing.

## Test changes

None. `work/TEST-CHANGES.md` is unchanged.

## Decisions and deviations from the plan

1. **`Array.sum` checks every element, not only the first.** The plan gated on the first element's kind and let the operator report anything later. I check all elements with `need_number` before folding, so a non-number anywhere is `wrong kind` (section 12, library function: "an argument has a kind the function does not take"; an `Array[T: Number]` holding a string is such an argument) and only numbers of different kinds reach the operator's `kind mismatch`. This makes the split between the two phrases principled instead of positional. A consequence: in `[int8(100), int8(100), "a"]` the `wrong kind` wins over the overflow, since the check precedes the fold. Both phrases are untested for `Array.sum`.
2. The Python parameter of `string_from_bytes` is `bs` (the registry name is `bytes`, as section 11 prints it) to avoid shadowing the builtin the function calls.
3. Nothing else deviates: no change to `interp.py`, `lower.py`, `values.py`, `equal.py`, `arith.py`, or `render.py`, because every section 3 probe passed against the final code (below).

## Untested choices (plan section 5), recorded

- `Array.sum` on an array with a non-number element: `wrong kind`; on numbers of mixed kinds: `kind mismatch` (through the operator). Verified by probes: `["a", "b"]`, `[1, "a"]`, `[true, true]` give `wrong kind: expected a number`; `[1, int8(1)]` gives `kind mismatch`.
- `String.toUpper`/`toLower` leave a code point whose mapping has several code points unchanged (`"ß"`, `"ﬁ"`, `"İ"`), which is G36's one-to-one reading, not Unicode's full case mapping: `String.toUpper("straße")` is `"STRAßE"`.
- `String.toInt` is `int64` only: `"-9223372036854775808"` parses, `"9223372036854775808"` is `null`; Arabic-Indic digits and `"42\n"` are `null`.
- `Map.remove` of a key whose stored value is `null` returns `null` and removes it (size goes down); only `Map.has` tells the cases apart.
- `Result.map`/`mapErr` return records that are not `Record.same` as the input, on both branches.
- `Result.unwrapOr(Ok{}, 5)` and `Result.map(Ok{}, ...)` (unchecked, no payload) panic `no field: value`.
- `Mutex.withLock` on a non-`Mutex` is `wrong kind: expected a Mutex`; its lambda sees `x == void`; a panic inside it (in a goroutine) prints `panic: inside` and a later `withLock` on the same mutex succeeds, so the lock was released.

## Section 3 checklist against the final code

Every item was re-run as a probe after the change (one program each, under `python3 -I bootstrap/antpy.py run`):

1. Tags, 2. `is`: `1 is int`, `1 is int64`, `byte(1) is byte`, `byte(1) is uint8` are `true`; `1 is byte` is `false`; `print is func`, `[] is Array`, `Map{} is Map` are `true`.
3. `==`: a record with a NaN field is not equal to itself; `int8(1) == 1`, `1 == 1.0`, `null == void` are `false`; the cyclic and map-order tests in the selection pass.
4. Spread: `Vec{ y: 9.0, ...p }` renders `Vec{ y: 2.0, x: 1.0 }`, `Point{ ...p, x: 0.0 }` renders `Point{ x: 0.0, y: 2.0 }`.
5. `?.`, 6. `??`: `null ?? null` is `null`, `null ?? 3` is `3`; the `nullability` tests (`optchain-*`, `nullish-return-panic`) pass.
7. `try`: the `errors` tests including `try-precedence` and `unchecked/errors/try-non-result-operand` pass.
8. Defer and replacing panics: `inner` with `defer { print("inner defer") }`, `defer { panic("from defer") }`, `panic("original")` prints `inner defer`, `main defer`, then `panic: from defer` with the trace `defer`, `inner`, `main`, exit 2.
9. Prelude records: `run/errors/result-ctor-eq` passes; `Result.map`/`mapErr` build records with the same `OK`/`ERR` tags, so `try` and these functions agree on the shape.
10. Conversions: `int(1.5)` is `1`, `int(-1.5)` is `-1`, `float32(16777217)` is `16777216.0`, `float64(float32(0.1))` is `0.10000000149011612`; `run/types/conv-as-value` (conversions as values) passes.

## Known limitations (for 0.3 and 0.4)

- `Mutex.lock`/`Mutex.unlock` are still absent: `run/concurrency/mutex-withlock-returns-and-releases` now gets through both `withLock` calls and the goroutine panic (`panic: inside withLock` on stderr, lock released) and fails only at `Mutex.lock` with `no field: lock`. 0.4 must implement `lock`/`unlock` (`not locked`) on the same `m.lock` that `withLock` holds.
- `withLock` uses a non-reentrant `threading.Lock`: a nested `withLock` on the same mutex blocks forever, like Go's `sync.Mutex`.
- `String.toFloat`, `String.split`, `String.trim`, `Array.concat`, `Map.keys`, `Math`, `Iter.map`, and the rest of section 11 arrive in 0.3; `RwMutex`, `Once`, `Cond`, `Atomic` in 0.4.

## Refactor

Reviewer suggestion 1, applied: `ev_Try` in `bootstrap/antpy/interp.py` now reads the payload of its `Ok`/`Err` operand through `prelude.result_payload(v, "value")` / `prelude.result_payload(v, "error")` instead of `v.fields.get(...)`. Before, an unchecked `try Err{}` yielded `Err{ error: null }` and `try Ok{}` yielded `null`, while `Result.map`, `Result.unwrap`, and `Result.unwrapOr` on the same payload-less records panicked `no field: ...`. Section 12 reads a missing field as `no field`, and section 8 says `try` returns a new `Err` with the same `error` field or yields the `Ok`'s `value` field, so the panic is the consistent reading and `try` and the `Result` functions now agree. Probes after the change: `try Err{}` and `try Ok{}` panic `no field: error` / `no field: value`, exit 2; `try Ok{ value: 1 }` still yields `1`, `try Err{ error: Error{ message: "boom" } }` still returns the `Err` unchanged, and `try 7` is `7`. Unchecked-only, no test covers it; two lines changed, nothing else.

Acceptance after the refactor: `264 checks, 0 failures, 0 warnings (implementation mode, 263 tests)`, exit 0 (`work/0.2/test.log`).
