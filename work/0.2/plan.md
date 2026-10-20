# Plan for step 0.2 "antpy-types-errors"

Deliverable (ROADMAP.md): tags, `is` (kind names incl. `int`/`byte`), structural `==` with cycle handling, spread, `?.` `??`, `try`, `defer`, unwinding, replacing panics, prelude `Ok`/`Err`/`Error`/`Entry`/`Indexed`, conversions. Allowed directories: `bootstrap/`, `work/0.2/`.

Acceptance (verbatim):

    python3 -I src/tests/run.py --run "python3 -I bootstrap/antpy.py run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /functions/ --filter /expressions/ --filter /types/ --filter /nullability/ --filter /errors/

## 0. Where things stand

The acceptance command today: `264 checks, 15 failures` (263 tests). Every failure is `panic: no field: <name>` on a library namespace record: a native that `prelude.py` does not register yet. With `--consistency` added the result is the same 15 and nothing else. The failing tests and what each needs:

| Test | Needs |
|---|---|
| run/errors/defer-try-early-return, result-compose-flat, result-narrow-value-error | `String.isEmpty`, `String.toInt` |
| run/errors/result-map, result-maperr, result-maperr-widens, result-unwrapor-isok | `Result.map`, `Result.mapErr`, `Result.unwrapOr` |
| run/nullability/narrow-param | `String.toUpper` |
| run/types/conv-as-value | `String.fromBytes` |
| run/types/infer-lambda-solves-U, tdecl-alias-interchangeable | `Array.sum` |
| run/types/infer-lambda-zero-params | `Mutex.withLock` |
| run/types/map-literal-forms, map-nan-key, map-eq-order-independent | `Map.isEmpty`, `Map.remove`, `Map.same` |

Everything the deliverable names was built in 0.1 (`work/0.1/report.md`, `work/0.1/plan.md` sections 4, 6, 10) and passes every selected `run`, `panic`, and `unchecked` test in `types`, `nullability`, and `errors`. Probe programs (section 3) confirmed the spec readings the tests do not cover. **Do not rewrite the evaluator.** This step is: add twelve natives with the exact semantics of section 11, verify the deliverable against the checklist in section 3, and report.

## 1. Files

| File | Change |
|---|---|
| `bootstrap/antpy/prelude.py` | the only code change: `import re`; two small helpers; the natives of section 2; one registry line per native in `NAMESPACES` |
| `work/0.2/test.log` | output of the acceptance command |
| `work/0.2/report.md` | what was built, the untested choices (section 5), acceptance output |

No change to `interp.py`, `lower.py`, `values.py`, `equal.py`, `arith.py`, `render.py` unless a probe in section 3 fails. No test change is expected; `work/TEST-CHANGES.md` stays as it is (the `output:` block sweep is done repo-wide).

## 2. The natives to add

Conventions (from 0.1, keep them): a native is a Python function `fn(*args)`; one that calls back into Ant is registered with a third tuple element `True` and receives `(interp, frame, *args)`; argument kinds are checked with the `need_*` helpers, which panic `wrong kind`; the parameter names in the registry are the ones section 11 prints, because named arguments resolve against them (`Result.unwrapOr(r, fallback: 0)` must work). Never return the `MISSING` sentinel to Ant code.

Add, under the existing section comments of `prelude.py`:

| Native | Params | Behavior |
|---|---|---|
| `Array.sum` | `xs` | `need_array`. Empty: `None`. Otherwise the first element's kind must be in `NUMBER_KINDS`, else `wrong kind` (strings must not concatenate, bools must not add). Fold with `arith.binary("+", acc, x)`: integer overflow panics `integer overflow` (panic/stdlib/arr-sum-overflow-*), a mixed-kind array panics `kind mismatch` (untested; see section 5), floats follow IEEE and `float32` rounds per step. Returns the folded value: `int8` in gives `int8` out (run/stdlib/arr-sum). |
| `Map.isEmpty` | `m` | `need_map(m).size == 0` |
| `Map.remove` | `m`, `k` | `need_map`; `AntMap.remove(k)`; `MISSING` becomes `None`. A NaN key is never found: `None`, size unchanged (map-nan-key). |
| `Map.same` | `m`, `n` | both `need_map`; identity (`is`) |
| `String.isEmpty` | `s` | `need_string(s) == ""` |
| `String.toUpper` | `s` | per code point: `c.upper()` when that result is exactly one code point, else `c` unchanged. Python's `"ß".upper()` is `"SS"`, so a whole-string `.upper()` is wrong (G36: `"ß"` unchanged; `"é"` becomes `"É"`). Write the helper as `case_map(s, fn)` so `String.toLower` is the same helper with `str.lower` (register it too: one line, and run/stdlib/str-case tests the pair; `"İ".lower()` is two code points and stays unchanged by the same rule). |
| `String.fromBytes` | `bytes` | `need_array`; every element must be a `UInt8` (`type(b) is UInt8`; a plain `int` is `wrong kind`); `bytes(list).decode("utf-8")` in strict mode; `UnicodeDecodeError` gives `None`. Verified: the strict decoder rejects all six cases of run/stdlib/str-fromBytes-invalid (0xFF, lone continuation, overlong, encoded surrogate, truncated, beyond U+10FFFF) and an empty array decodes to `""`. |
| `String.toInt` | `s` | `need_string`; `re.fullmatch(r"[+-]?[0-9]+", s)` else `None`; then `int(s)`; outside `INT_RANGE["int64"]` gives `None`; otherwise the plain Python `int` (the `int64` kind). Never call `int(s)` on an unmatched string: Python accepts surrounding whitespace, underscores, and non-ASCII digits, all of which must be `None` (run/stdlib/str-toInt). `fullmatch`, not `$`, so a trailing newline fails. `"-0"` is `0`, `"007"` is `7`. |
| `Result.map` | `r`, `f` (ctx) | `need_result`, `need_func`. `Ok`: `Record(OK, {"value": interp.call(f, [value], (), frame)})`. `Err`: a fresh `Err` with the same `error`, `f` not called (run/errors/result-map checks the marks). |
| `Result.mapErr` | `r`, `f` (ctx) | the mirror image: `Err` maps the error, `Ok` passes through as a fresh `Ok`. |
| `Result.unwrapOr` | `r`, `fallback` | `need_result`; `Ok` gives `value`, `Err` gives `fallback`. |
| `Mutex.withLock` | `m`, `f` (ctx) | `need(m, Mutex, "a Mutex")`, `need_func`; `m.lock.acquire()`; `try: return interp.call(f, [], (), frame) finally: m.lock.release()`. The lambda is called with zero arguments, so its `x` is `void` (0.1, G23); the call's value is the body's value (`{ counter += 1 }` gives `void`, `{ counter * 10 }` gives the number). A panic in `f` must propagate after the release, never be caught: run/concurrency/mutex-withlock-returns-and-releases (0.4) depends on both. |

Section 11 says every non-`mutates` function "returns a new value": `Result.map`/`mapErr` build fresh records on both branches, like `try` does in `ev_Try`.

Payload reads: `result_unwrap` reads `r.fields["value"]`, which is a `KeyError` (reported as an internal interpreter error) on an unchecked `Ok{}`. Add a helper `result_payload(r, name)` that panics `no field: <name>` when the field is absent and use it in `unwrap`, `unwrapOr`, `map`, `mapErr`.

## 3. Verify the deliverable (expect no code change)

Each item names where the behavior lives and the reading the tests and GAPS fix. Probes run while planning passed; re-run any that the implementation touches.

1. **Tags.** `load_module` binds every `type` declaration, alias or not, to a fresh `Tag` (section 4; G17). A record literal head goes through `ev_path` and must be a `Tag`: `not a tag` (unchecked/types/rec-head-not-tag-rt). Prelude type names `Seq`, `Integer`, `Number`, `Ordered`, `Result`, `bool`, `string`, `never` are tags too, so `x is Integer` is `false` and `bool(1)` is `not a function` (G8).
2. **`is`.** `lower.py` `KIND_TESTS` decides kind names at lowering time: `int` is `int64`, `byte` is `uint8` (G48), `func` covers `Closure`, `Native`, `YieldFn`, plus `Array`, `Map`, `null`, `bool`, `string`, the ten numeric names. Any other path evaluates (`IsTag`) and must be a tag: `not a tag` (unchecked/types/tdecl-is-union-alias-rt). `is void` is a parse error; tests use `== void` (G17). Probe: `1 is int`, `1 is int64`, `byte(1) is byte`, `byte(1) is uint8` all `true`; `1 is byte` `false`; `print is func`, `[] is Array`, `Map{} is Map` `true`.
3. **Structural `==`.** `equal.eq` with the pair set and no identity shortcut (G11): `p == p` is `false` with a NaN field; cyclic values compare in finite time (run/types/rec-eq-cyclic). Different kinds are unequal: `int8(1) == 1`, `1 == 1.0`, `null == void` are `false`. Maps compare by key set with structural hashing; `-0.0` and `0.0` are one key.
4. **Spread.** `ev_RecordLit`: `fields.update(src.fields)` keeps a replaced field's first position and a later entry wins; `Vec{ y: 9.0, ...p }` renders `Vec{ y: 2.0, x: 1.0 }` and `Point{ ...p, x: 0.0 }` renders `Point{ x: 0.0, y: 2.0 }` (G52 creation order). The tag comes from the head; a spread operand that is not a record panics `not a record`; an array spread of a non-array `not an array`. The G10 leak (undeclared fields travel with the spread) is tested as written (run/types/rec-spread-leak).
5. **`?.`.** `OptField`/`OptIndex`/`OptCall` raise `OptNull`, caught by the `Chain` wrapper that `lower.spine` puts around the whole postfix chain, so `a?.b.c` skips the rest of the chain; arguments and indices get chains of their own. On a non-null left side the plain rule applies: `(1)?.x` panics `not a record` (G21, unchecked/nullability/optchain-nonnull-non-record). `f?.(x)` checks `f` for `null` before evaluating the arguments.
6. **`??`.** Only `null` triggers the right side; the right side may be `return`, `panic`, `break` (run/nullability/nullish-return-panic). `null ?? null` is `null`.
7. **`try`.** `ev_Try` raises `ReturnSignal(scope.frame, ...)`, the lexically enclosing function's frame: `null` returns `null`, an `Err` returns a fresh `Err` with the same `error`, an `Ok` yields `value`, anything else is yielded unchanged (G28, unchecked/errors/try-non-result-operand). `try` binds tighter than `|` (run/errors/try-precedence).
8. **`defer`, unwinding, replacing panics.** Blocks that contain a `defer` directly are marked at lowering and get a defer list; `run_stmts_with_defers` runs them in reverse on every exit, in the registration scope; a `return` inside a deferred block leaves only that block (`run_deferred`); a panic in a deferred block replaces the pending panic or return and the remaining deferred blocks still run (G59). Probe: `inner` with `defer { print("inner defer") }`, `defer { panic("from defer") }`, `panic("original")` prints `inner defer`, then `main defer`, then `panic: from defer` with the trace `defer`, `inner`, `main`, exit 2.
9. **Prelude records.** `Ok`, `Err`, `Error`, `Entry`, `Indexed` are tags in `make_prelude`; `Result.ok`/`err` build records with exactly those tags, so `Ok{ value: 1 } == Result.ok(1)` (run/errors/result-ctor-eq); `for` over a map yields `Entry{ key, value }`, `Array.entries` yields `Indexed{ index, value }`.
10. **Conversions.** `arith.convert`: floats truncate toward zero (`int(1.5)` is `1`, `int(-1.5)` is `-1`, G38), out of range, NaN and infinity panic `conversion out of range` for integer targets, `float32` of a value beyond its range panics (G7) while in-range values round (`float32(16777217)` is `16777216.0`), `float64(float32(0.1))` is `0.10000000149011612`, a non-number argument is `wrong kind`. Each conversion is a `Native`, so `Array.map(xs, byte)` and `var f: func(int): float64 = float64` work.

## 4. Order of work

1. Add the natives of section 2 in this order, running the named tests after each (`--filter <name>`): `String.isEmpty`, `String.toInt` (defer-try-early-return, result-compose-flat, result-narrow-value-error); `Result.map`, `Result.mapErr`, `Result.unwrapOr` (result-map, result-maperr, result-maperr-widens, result-unwrapor-isok); `String.toUpper` and `toLower` (narrow-param); `String.fromBytes` (conv-as-value); `Array.sum` (infer-lambda-solves-U, tdecl-alias-interchangeable); `Mutex.withLock` (infer-lambda-zero-params); `Map.isEmpty`, `Map.remove`, `Map.same` (map-literal-forms, map-nan-key, map-eq-order-independent).
2. Run the acceptance command, output to `work/0.2/test.log`: exit 0, last line `0 failures`. Then the same command with `--consistency` as a sanity check (not required).
3. Run the 0.3 tests that pin these functions so 0.3 does not have to touch them again: `--tier run --tier panic --filter arr-sum --filter str-toInt --filter str-case --filter str-fromBytes --filter str-bytes-roundtrip --filter map-set-remove --filter map-same --filter map-size-isEmpty`. All of these use only functions that exist after this step; they should pass. Do not chase a failure that needs a function outside section 2; note it in the report.
4. `ruff check --select F401,F821,F841 bootstrap/` reports nothing (the 0.1 standard).
5. Walk the checklist of section 3 once more against the final code; write `work/0.2/report.md` with the acceptance output, the untested choices of section 5, and anything 0.3 or 0.4 must know (e.g. `Mutex.lock`/`unlock` are still absent; `withLock` holds `Mutex.lock` directly).

## 5. Spec rules that are easy to get wrong

1. `Array.sum([])` is `null`, not `0`: the interpreter cannot produce a zero of the right kind (section 11). Tests write `Array.sum(xs) ?? 0`.
2. `Array.sum` must refuse strings with `wrong kind` before folding; `arith.binary("+")` would concatenate them. Mixed numeric kinds panic through the operator (`kind mismatch`); that phrase is untested either way. Say so in the report.
3. `String.toInt` accepts an optional sign and ASCII decimal digits only (G36); whitespace, `_`, `0x`, fractions, Unicode digits, and overflow all give `null`. Python's `int()` is more liberal on every one of those.
4. `String.toUpper`/`toLower` map one code point to one code point or leave it alone (G36): whole-string `str.upper()` expands `ß`.
5. `String.fromBytes` takes an `Array[byte]`: elements are `UInt8` values, a plain `int` element is `wrong kind` (section 12: "an argument has a kind the function does not take"). Strict UTF-8: `null` on any invalid sequence, never a replacement character.
6. `Map.remove` of a missing key is `null`; a NaN key is never found (G13); the returned value may itself be `null` when the stored value is `null` (only `Map.has` tells them apart).
7. `Result.map` and `mapErr` call `f` on exactly one branch and return fresh records; `try` and these functions agree on the shape `Err{ error }`.
8. `Mutex.withLock` releases in a `finally` and lets the panic through; the lambda is called with no argument (`x` is `void`), and the call's value is returned.
9. Natives read a `Result` payload with a `no field` panic when it is absent, never a Python `KeyError`.
10. Keep `type(v) is ...` checks: Python `bool` is an `int`, and `True` must be `wrong kind` wherever an `int` is required.
11. Never return `MISSING` or `UNINIT` from a native.
12. Parameter names in the registry are the spec's (`xs`, `m`, `k`, `n`, `s`, `bytes`, `r`, `f`, `fallback`): named arguments and pipes with named arguments resolve against them.

## 6. Risks

- Scope: the deliverable's semantics already pass; the temptation is to refactor the evaluator. Do not. Only `prelude.py` changes unless a section 3 probe fails, and the report must say which probe.
- `Array.sum` on a mixed-kind array: `kind mismatch` (through the operator) versus `wrong kind` (library argument) is an untested choice. The plan picks the operator's phrase because the fold is the operator; record it.
- `Mutex.withLock` uses a non-reentrant `threading.Lock`: a nested `withLock` on the same mutex blocks forever, like Go's `sync.Mutex`. 0.4 owns `lock`/`unlock` (`not locked`) and must keep `withLock` consistent with them (same `m.lock`).
- `String.toUpper` on characters whose uppercase is several code points (`ß`, ligatures) leaves them unchanged; `toLower` on `İ` likewise. This is G36's reading, not Unicode's full case mapping; the report should state it.
- Running the 0.3 stdlib tests early (step 4.3) is a check, not acceptance; a failure there that needs a function outside section 2 is 0.3's work.
