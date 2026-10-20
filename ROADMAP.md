# antc roadmap

Goal: `antc`, a compiler for Ant written in Ant, producing Apple Silicon macOS binaries, passing every test in `src/tests` (the executable spec: 543 tests in six tiers, runner `src/tests/run.py`, contract in `src/tests/README.md`, open questions and the readings the tests assume in `src/tests/GAPS.md`).

Strategy, decided with the user:
- **M0** a Python bootstrap interpreter, `antpy`, with no checker. Types are erased and the spec says a program may run unchecked, so the bootstrap only has to implement section 12 over the reference parser's AST. It exists to run `antc` once and to shake out wrong test expectations cheaply.
- **M1** the frontend of `antc` in Ant (lexer, parser), validated against the reference parser.
- **M2** a C runtime plus a dynamic (tagged) code generator in Ant; `antc build` emits C and calls clang; Boehm GC from Homebrew; then `antc` compiles itself (fixpoint) and the bootstrap is retired from the loop.
- **M3** the checker in Ant, developed and tested with the native `antc`.
- **M4** polish and final verification.

Each milestone is one run of the workflow in `.claude/workflows/antc-roadmap.js`; each step ends in a commit on branch `antc`. The design for M2 is `docs/runtime-design.md`.

## Prerequisites (verified on the target machine)

- macOS on Apple Silicon, Xcode command-line tools (`clang`), Python 3.11+, git.
- `brew install bdw-gc` (headers `/opt/homebrew/include/gc.h`, library `/opt/homebrew/lib/libgc.dylib`).
- Repo checked out on branch `antc`; run every command from the repo root.

## Layout

```
ROADMAP.md                   this file
docs/runtime-design.md       M2 design: value representation, unwinding, scheduler, codegen shapes
.claude/workflows/antc-roadmap.js   the workflow
bootstrap/antpy.py           M0 CLI; bootstrap/antpy/*.py the interpreter (imports tools/antparse.py)
src/antc/*.ant               M1-M3: lexer.ant, parser.ant, ast.ant, codegen/*.ant, check/*.ant, main.ant
runtime/ant.h, runtime/*.c   M2 C runtime, built to runtime/libantrt.a
tools/compare_frontend.py    M1 oracle comparison against tools/antparse.py
Makefile                     runtime / stage1 / stage2 / fixpoint / test
bin/                         built compilers (git-ignored)
work/<step-id>/plan.md, report.md, BLOCKED.md; work/TEST-CHANGES.md
```

## Rules for every workflow agent

- Read-only, never modified: `ant-lang.md`, `stdlib.md`, `grammar.ebnf`, `tools/antparse.py`, `src/tests/run.py`, `src/tests/oracle.py`, `src/tests/README.md`, `src/tests/lib/`, `ROADMAP.md`, `docs/`, `.claude/`.
- A test under `src/tests` may be changed only when its expectation contradicts the spec. Change it minimally, cite the section, and append `file, change, citation` to `work/TEST-CHANGES.md`. Never delete, skip, or weaken a test. The reviewer verifies every such change.
- Never special-case test files, names, or outputs in the implementation. `antc` and the runtime may execute nothing but clang and the C toolchain. No network.
- Write only inside the step's directories plus `work/<step-id>/`. Do not commit; a separate agent commits each green step on branch `antc`.
- The acceptance command is the definition of done: it must exit 0 and end with `0 failures`.

## Command-line contracts

- `python3 -I bootstrap/antpy.py run [--unchecked] FILE [ARGS...]`: runs the module at FILE as the entry; `--unchecked` is accepted and identical. Exit 0 when `main` returns; a panic prints `panic: <message>` and a trace to stderr and exits 2; `Os.exit(n)` exits n.
- `antc lex FILE` (token dump), `antc parse [--dump] FILE` (exit 0/1; `--dump` prints the S-expression AST), `antc check FILE` (M3: reports `path:line:col: message`, exit 1 on errors), `antc build [--unchecked] [--emit-c] FILE -o OUT`, `antc run [--unchecked] FILE [ARGS...]`. Before M3, `check` is absent and `build`/`run` never check.
- `tools/compare_frontend.py lex|parse (--sample | --all | PATHS...)`: dumps the reference lexer or parser output for each path in a canonical text form and diffs it against `antc lex`/`antc parse --dump` run under antpy; files under `tests/err` must be rejected by both. `--sample` is `tests/ok/*`, `tests/err/*`, and every 14th file of the sorted suite; `--all` is every `.ant` in the repo. Exit 0 iff everything agrees.
  AST dump form: tuples as `(Kind child ...)`, lists as `[a b ...]`, strings quoted with escapes, `None` as `nil`, booleans as `true`/`false`, numbers as their literal text.
- `Makefile`: `make runtime` (libantrt.a), `make stage1` (antc under antpy builds `bin/antc-stage1`), `make stage2` (stage1 builds `bin/antc-stage2`), `make fixpoint` (stage2 builds stage3; the C emitted for antc's own source by stage2 and stage3 must be byte-identical; copies stage2 to `bin/antc`), `make test` (full suite with `bin/antc`). From M3 on, stage builds are checked builds.

Runner shorthands used below: `RUN` = `python3 -I src/tests/run.py`; `PY` = `--run "python3 -I bootstrap/antpy.py run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run --unchecked {file} {args}"`; `A0` = `--timeout 600 --run "python3 -I bootstrap/antpy.py run src/antc/main.ant run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run src/antc/main.ant run --unchecked {file} {args}"`; `AN` = `--run "bin/antc run {file} {args}" --run-unchecked "bin/antc run --unchecked {file} {args}"`; `CN` = `--check "bin/antc check {file}" ` + `AN`; `RPU` = `--tier run --tier panic --tier unchecked`. The workflow script carries each acceptance command in full.

## M0 Bootstrap interpreter (Python)

Design: tree-walking interpreter over the tuple AST from `tools/antparse.py` (`parse(src)`; node shapes like `("Func", name, tparams, sig, body)`, `("RecordLit", path, entries)`). Values carry their kind (ten numeric kinds with width-checked arithmetic, bool, string, null, void, array, map, record with tag, func, tag, sync). Scopes are dictionaries with hoisted bindings and an "unbound" sentinel (GAPS G3, G29). `return`/`break`/`continue` and panics are Python exceptions so they unwind sequence functions and run deferred blocks; a panic inside a deferred block replaces the pending one (G59). Goroutines are `threading.Thread`s; the GIL gives memory safety under races; `sys.setswitchinterval(0.005)`; exit when `main` returns via `os._exit`. Rendering per G33/G55. Deep recursion: raise the recursion limit and thread stack size.

| Step | Deliverable | Directories | Acceptance |
|---|---|---|---|
| 0.1 antpy-core | CLI, scopes, values, literals, operators with §12 kind checks and overflow panics, calls (positional, named, defaults), closures, lambdas, blocks, if/while/for over arrays, maps, strings, sequences, destructuring, return/break/continue, record/array/map literals, field and index access, templates, `String.from`, `print`, `panic` with trace and exit 2 | `bootstrap/` | `RUN PY RPU --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /functions/ --filter /expressions/` |
| 0.2 antpy-types-errors | tags, `is` (kind names incl. `int`/`byte`), structural `==` with cycle handling, spread, `?.` `??`, `try`, `defer`, unwinding, replacing panics, prelude `Ok`/`Err`/`Error`/`Entry`/`Indexed`, conversions | `bootstrap/` | 0.1 filters + `--filter /types/ --filter /nullability/ --filter /errors/` |
| 0.3 antpy-stdlib | Array, Map, String (code points), Iter, Math, Bits, Result, Record; rendering of every kind | `bootstrap/` | 0.2 filters + `--filter /stdlib/` |
| 0.4 antpy-concurrency | goroutines as threads, Mutex, RwMutex, WaitGroup, Once, Cond, Atomic, `Time.sleep`, goroutine panic isolation, preemption, exit when main returns | `bootstrap/` | 0.3 filters + `--tier programs --filter /concurrency/ --filter race-memory-safe --filter parallel-sum --filter producer-consumer` |
| 0.5 antpy-modules | imports (destructure, alias, namespace), live read-only bindings, depth-first init once, hoisting, `main`, exit status, `Os.args/env/exit` | `bootstrap/` | 0.4 tiers and filters + `--filter /modules/ --filter programs/` |
| 0.6 antpy-io | File, Json, Time | `bootstrap/` | 0.5 + `--tier io --filter io/` |
| 0.G gate | everything except the check tier, with `--consistency`; a refactor pass; `bootstrap/README.md` | `bootstrap/` | `RUN PY --consistency --tier run --tier panic --tier unchecked --tier programs --tier io` |

## M1 Frontend in Ant (runs under antpy)

| Step | Deliverable | Directories | Acceptance |
|---|---|---|---|
| 1.1 antc-lexer | `src/antc/lexer.ant` (tokens, newline rule, templates), `src/antc/main.ant` with `lex`; `tools/compare_frontend.py` with the `lex` mode | `src/antc/`, `tools/compare_frontend.py` | `python3 -I tools/compare_frontend.py lex --sample` |
| 1.2 antc-parser | `src/antc/ast.ant` (tagged unions mirroring the reference node kinds), `src/antc/parser.ant`, `antc parse --dump`; `compare_frontend.py parse` | same | `python3 -I tools/compare_frontend.py parse --sample` |
| 1.G gate | full agreement; antc parses its own sources | same | `python3 -I tools/compare_frontend.py lex --all && python3 -I tools/compare_frontend.py parse --all` |

## M2 C runtime and code generator

Follow `docs/runtime-design.md`. Steps 2.1 to 2.4 run `antc` under antpy, which is slow (`--timeout 600`), so their acceptance covers only their own sections plus smoke; 2.5 makes `antc` native and the gate covers everything.

`antc build` runs clang through `Os.exec(args: Array[string]): Result[int, Error]`, an antc-internal native that `stdlib.md` does not define: it runs `args[0]` (looked up on `PATH`) with the remaining elements as arguments, stdio inherited, waits, and returns `Ok(exit status)` (128 + signal when the child was killed) or `Err` when the program cannot start; a non-array or a non-string element panics `wrong kind`. It exists in the bootstrap (`bootstrap/antpy/system.py`) and in the C runtime, so that `antc` under antpy and the native `antc` both build programs. M2 steps may change `bootstrap/antpy/system.py` for this native only; nothing but `antc` uses it, and it is not part of the language.

| Step | Deliverable | Directories | Acceptance |
|---|---|---|---|
| 2.1 rt-core | `runtime/`: values, Boehm init, symbols, strings, numbers with overflow checks, closures, calls with named args and defaults, `ant_for_each` with yield objects, unwinding (frames, panic, set_return, defers, traces), print; `src/antc/codegen/`: ANF lowering, functions, blocks with defer lists, control flow, lambdas, `antc build`/`run`; `Makefile` `runtime` target | `runtime/`, `src/antc/`, `Makefile`, `bootstrap/antpy/system.py` (`Os.exec` only) | `RUN A0 RPU --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /functions/ --filter /expressions/` |
| 2.2 rt-types-errors | records with tags and shapes, arrays, maps, `is`, structural equality and rendering with cycles, spread, `?.` `??`, `try`, conversions, replacing panics | same | `RUN A0 RPU --filter /smoke/ --filter /types/ --filter /nullability/ --filter /errors/` |
| 2.3 rt-stdlib | the core library natives, float formatting, stable sort | same | `RUN A0 RPU --filter /smoke/ --filter /stdlib/` |
| 2.4 rt-modules | module sections, init order, imports, `main`, `Os.args/env/exit` | same | `RUN A0 RPU --tier programs --filter /smoke/ --filter /modules/ --filter programs/ --exclude race-memory-safe --exclude parallel-sum --exclude producer-consumer` |
| 2.5 self-host | `make stage1`, `make stage2`, `make fixpoint`; `bin/antc` native | same | `make fixpoint && RUN AN RPU --tier programs --exclude /concurrency/ --exclude race-memory-safe --exclude parallel-sum --exclude producer-consumer` |
| 2.6 rt-concurrency | GIL scheduler, goroutines, sync objects, `Time.sleep` | same | `make fixpoint && RUN AN RPU --tier programs --filter /concurrency/ --filter race-memory-safe --filter parallel-sum --filter producer-consumer` |
| 2.7 rt-io | File, Json, Time | same | `make fixpoint && RUN AN --tier io` |
| 2.G gate | fixpoint and the whole suite except the check tier, with `--consistency` | same | `make fixpoint && RUN AN --consistency --tier run --tier panic --tier unchecked --tier programs --tier io` |

## M3 Checker in Ant (native antc)

| Step | Deliverable | Directories | Acceptance |
|---|---|---|---|
| 3.1 check-core | `src/antc/check/`: names and declarations, type representation, assignability, records, arrays, maps, unions, `is`, literals; `antc check` with `path:line:col: message` using the README phrases; `antc run` checks before running unless `--unchecked` | `src/antc/` | `make fixpoint && RUN CN --tier check --tier run --tier panic --tier unchecked --tier programs --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /types/ --filter programs/smoke` |
| 3.2 check-inference | bidirectional checking, generics and bounds, lambdas, function types, pipes, calls | same | 3.1 + `--filter /functions/ --filter /expressions/ --filter /stdlib/` |
| 3.3 check-flow | narrowing with complement, exhaustiveness through `never`, try/defer/go/loop placement rules | same | 3.2 + `--filter /nullability/ --filter /errors/ --filter /concurrency/` |
| 3.4 check-modules | export tables, prelude collisions, `main`, cycles, missing files; antc checks its own sources | same | `make fixpoint && RUN CN --tier check --tier run --tier panic --tier unchecked --tier programs --tier io && bin/antc check src/antc/main.ant` |
| 3.G gate | all tiers with `--consistency`; stage builds are checked builds | same | `make fixpoint && RUN CN --consistency` |

## M4 Final

| Step | Deliverable | Directories | Acceptance |
|---|---|---|---|
| 4.1 final-polish | `antc --help`, `make test`, `src/antc/README.md` (build, bootstrap, layout), `work/TEST-CHANGES.md` reconciled against `src/tests/GAPS.md`, dead code and refactor pass across `bootstrap/`, `runtime/`, `src/antc/` | all implementation dirs | `make fixpoint && RUN CN --consistency && python3 -I tests/run_tests.py` |

## Running the workflow

```
SCRIPT = "<repo>/.claude/workflows/antc-roadmap.js"
Workflow({ scriptPath: SCRIPT, args: { milestone: "M0" } })                 # one milestone per run
Workflow({ scriptPath: SCRIPT, args: { milestone: "M2", steps: ["2.3"] } })  # a single step
Workflow({ scriptPath: SCRIPT, args: { milestone: "M0", dryRun: true } })    # list the steps, spawn nothing
Workflow({ scriptPath: SCRIPT, resumeFromRunId: "wf_..." })                  # resume after a blocked step
```

Per step: plan → implement → independent test → independent review → refactor (the `simplify` skill on the step's diff, then the reviewer's suggestions, then a retest) → commit, up to `maxIter` (default 4) implement/test/review rounds. A step that stays red after `maxIter` rounds is re-planned once (the planner gets the failure history and writes a new plan; `args.replans` sets how many times) and given `maxIter` more rounds; only then does it write `work/<id>/BLOCKED.md` and stop the run; fix by hand or adjust the plan, then resume. Gates skip the plan phase and, once green, run a milestone-wide simplify pass instead of the per-step refactor: four reviewers in parallel (reuse, simplification, efficiency, altitude) over everything the milestone changed since its base commit, one applier, the gate rerun, then the commit.

## Future work (outside this roadmap)

- A native ARM64 backend replacing the C emitter, reusing the runtime.
- Type-directed unboxing and inlined array loops once the checker is trusted.
- Packages and versioning (spec section 14).
- `Http` namespace.
