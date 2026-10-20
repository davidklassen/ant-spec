# Report for step 0.1 "antpy-core"

## What was built

`bootstrap/antpy.py` (the CLI) and the package `bootstrap/antpy/`:

| File | Lines | Content |
|---|---|---|
| `antpy.py` | 38 | `run [--unchecked] FILE [ARGS...]`; puts `bootstrap/` and `tools/` on `sys.path` (needed under `python3 -I`); exits through `os._exit` with the interpreter's status |
| `antpy/values.py` | 239 | value classes (`Int8`..`UInt64`, `Float32`, `Record`, `Tag`, `Closure`, `Native`, `YieldFn`, `Mutex`, `WaitGroup`, `VOID`, sentinels), `Scope`, `Frame`, `AntPanic`, the `KIND` table and the integer range/width tables |
| `antpy/equal.py` | 138 | structural `==` with the pair set (no identity shortcut, G11), the structural hash, and `AntMap` (buckets by structural hash, keys compared with `eq`) |
| `antpy/arith.py` | 205 | `+ - * / %`, comparisons, unary `-`/`!`, the ten conversions, `Bits` |
| `antpy/render.py` | 117 | `String.from`: bare/quoted strings, JavaScript float layout with shortest round-trip digits (G33, G55), cycles as `...` |
| `antpy/lower.py` | 346 | `LineParser` (the reference parser with `("At", line, node)` wrappers on statements, declarations and imports) and the lowering pass: decoded literals, split templates (reusing `Lexer.embedded` to skip `${...}`), `Chain` wrappers for optional chains, pre-split assignment targets, `IsKind`/`IsTag`, blocks marked `has_defer`, statements as `(line, node)` pairs |
| `antpy/interp.py` | 765 | scopes and lookup (`UNINIT` → `not initialized`, miss → `unbound name`), frames and traces, `run_stmts`/`run_stmts_with_defers`, calls (positional, named, defaults in parameter order in the callee's scope, lambdas with `x`), natives with named-argument resolution, `YieldFn`, all expression and control-flow handlers, `for` over arrays (length re-read), maps (snapshot of `Entry` records), strings (code points) and sequences, module loading with namespace and live name imports, hoisting, `go` as a daemon thread, the panic report and exit statuses |
| `antpy/prelude.py` | 415 | prelude tags, conversions, `print`, `panic`, and the namespaces the 0.1 tests need: `Array` (17 functions), `Map` (4), `String` (5), `Iter` (`range`, `toArray`), `Bits` (10), `Result` (4), `Record.same`, `Mutex.new`, `WaitGroup` (4); the registry is a dictionary, one line per function |

The design follows `work/0.1/plan.md`: scopes are shared dicts with no block-level hoisting (G3 and G29 fall out), `ReturnSignal` carries the frame of the function it leaves so a `return` in a sequence-driven `for` body passes through the sequence function and runs its deferred blocks, defer lists exist only on blocks the lowering pass marked, and a panic in a deferred block replaces the pending one while the remaining deferred blocks still run (G59).

## Acceptance

Command (verbatim from the roadmap):

    python3 -I src/tests/run.py --run "python3 -I bootstrap/antpy.py run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /functions/ --filter /expressions/

Last line of its output (exit status 0):

    126 checks, 0 failures, 0 warnings (implementation mode, 125 tests)

The same filters with `--consistency` added also report 0 failures.

## Test changes

Every `output:` block in the suite (106 files, 241 lines) writes its lines as `//` followed by three spaces, as does the README's own example. `run.py` parses an output line as `ln[2:]` and, when that starts with two spaces, takes `body[2:]`, so each expected line keeps one leading space: `print-hello` is expected to print `" hello"`. That contradicts section 11 (`print` writes `String.from(value)` and a newline) and the README's rule ("two spaces after the slashes"); the oracle never noticed because it echoes the parsed block. `run.py` and `README.md` are read-only, so the 13 affected tests in this step's selection were fixed minimally: one space removed from each output line, nothing else. They are listed in `work/TEST-CHANGES.md` with the citation.

The remaining 93 files with `output:` blocks (in `types`, `nullability`, `errors`, `concurrency`, `modules`, `stdlib`, `dynamic`, `programs`, `io`) have the same issue and will fail the same way in later steps. A decision is needed: either normalize them all the same way, or change `run.py` to strip the indent after an optional leading space (then the 13 fixed files still parse, since the README says two spaces). I verified that the seven `run/stdlib/print-*` tests pass when their blocks are read with three spaces, so the rendering is already right.

## Decisions and deviations from the plan

- `AntMap` lives in `equal.py`, not `values.py`: its lookup needs `eq` and `hash_value`, and `values.py` cannot import `equal.py` without a cycle. `equal.py` registers `KIND[AntMap] = "map"`. `Scope` and `Frame` live in `values.py` so that `prelude.py` can build the prelude scope without importing the interpreter.
- Statement lists are `[(line, node)]` pairs instead of an `At` node per statement: the block runner sets `frame.line` while iterating, which costs no extra dispatch. Statements the base parser produces without a wrapper (lambdas inside templates, which `antparse` parses with a fresh base `Parser`) take the enclosing line.
- Natives are plain Python functions `fn(*args)`; those that call back into Ant are flagged `ctx=True` and receive `(interp, frame, *args)`. Named arguments resolve against the native's parameter list.
- Lowering pre-splits assignment targets (`AssignVar`/`AssignField`/`AssignIndex`/`AssignBad`), `==`/`!=` (`Eq`/`Ne`) and `is` (`IsKind` with a tuple of Python types, `IsTag` with a path), so the hot path does no string matching on operators or target shapes.
- A `break` or `continue` that escapes a function body (only possible unchecked, G24) panics `break or continue outside a loop` at the call boundary instead of escaping as a Python exception.
- `float32` arithmetic is computed in double and rounded per operation; a finite result that rounds past the float32 range becomes an infinity (IEEE), while the `float32(x)` conversion panics `conversion out of range` in that case (G7). `float32` rendering uses the shortest `%g` digits (1 to 9) that round-trip through float32.
- The recursion limit is 5,000,000 (the plan said 1,000,000): an Ant call costs about ten Python frames, so the plan's limit allowed only about 90,000 Ant frames. Frames live on the heap (measured: 300,000 Ant frames in 1.2 s and 790 MB), the 512 MB thread stack covers C-level re-entry. A `RecursionError` is reported as `panic: stack overflow`.
- Traces: `panic: <message>` then one `  at <name> (<path>:<line>)` per frame, innermost first; frames are named by the declared function, `func`, `lambda`, `defer`, `goroutine`, or `module`. Panic messages carry detail after a colon where useful (`unbound name: missing`, `no field: nope`); the runner checks substrings.
- Load errors (`literal out of range`, `invalid escape`, parse errors, a missing import, a missing or non-function `main`) print `path:line:col: message` and exit 1. The column is always 1 except for parse errors, since the lowering pass knows statement lines only.
- Module records share the module scope's dict as their fields, so a namespace import sees later assignments (`Expect.log = []`), and assigning to a destructured import writes through to the exporter (G31; 0.5 may revisit).

## Known limitations (for later steps)

- Only the library functions listed above exist; `Array.concat`, `Map.keys`, `String.split`, `Math`, `Iter.map`, `Result.map`, the other sync objects, `Os`, `File`, `Json`, `Time` arrive in 0.3 to 0.6.
- Goroutines are daemon threads without preemption control beyond `sys.setswitchinterval(0.005)`; `Mutex.lock/unlock/withLock` are not implemented yet (only `Mutex.new`, which the tests use for identity). 0.4 completes this.
- Modules: no import cycle detection, no `Os.args`; the entry path is only checked for existence. 0.5.
- A `defer` in a top-level initializer registers on the module scope and never runs (it is a compile error by G24).
- Map keys that are cyclic containers panic `map key too deep (cyclic?)` from the hash's depth guard rather than crashing.
- Performance has not been tuned beyond the dictionary dispatch and lowering-time literal decoding; the acceptance run (125 processes) takes a few seconds.

## Refactor

Reviewer suggestions applied in the refactor stage, one at a time, each followed by a run of the acceptance command or its `/functions/` + `/expressions/` subset.

1. **Checks in section 12 order** (`antpy/interp.py`). `ev_Call`, `ev_OptCall` and `ev_Pipe` panic `not a function` right after the callee evaluates, before the arguments ("`f` evaluates first and must be a function. Arguments evaluate in order."). `ev_AssignField` and `ev_AssignIndex` validate the target right after its sub-expressions, through `check_field_target` (a record that already has the field) and `check_index_target` (an array with an `int` index in range, or a map), shared by the plain and compound forms; before, the compound forms panicked before the right side through `field`/`index` and the plain forms after it. The array store keeps a one-line range guard for a right side that shrinks the array (`xs[2] = f(xs)` where `f` pops), which still panics `index out of range` instead of raising a Python `IndexError`. Probes `n(mark("arg"))` with `n = 1`, `q.a = mark("rhs")` with `q = 1`, and `xs[5] = mark("rhs")` now panic without printing the mark, in both forms.
2. **No exit path ends in a bare traceback** (`antpy/interp.py`). A `return`, `break` or `continue` that escapes a top-level initializer (a compile error by G24, reachable unchecked) is converted in `load_module` to `panic: return outside a function` / `panic: break or continue outside a loop` with the module trace and exit 2; before, it killed the worker thread with threading's default traceback and the process exited with the placeholder status 1. `report_internal` is now a module-level function, and the worker thread has a final `except BaseException` that reports an internal interpreter error and sets status 2, so the placeholder can no longer stand in for an unreported crash (verified by driving `run_program` with a `run_main` that raises a `BaseException` subclass).
3. **Unused imports removed**: `KIND` in `antpy/interp.py`; `Record`, `Tag`, `Mutex`, `WaitGroup` and the `# noqa: F401` in `antpy/lower.py`; and, found by `ruff check --select F401,F821,F841 bootstrap/` while confirming the above, `Float32` in `antpy/render.py` (only `round_f32` is used there). Ruff reports nothing on the package.

`antpy/interp.py` is now 794 lines. Acceptance after the changes (exit 0, log in `work/0.1/test.log`), and the same line with `--consistency` added:

    126 checks, 0 failures, 0 warnings (implementation mode, 125 tests)
