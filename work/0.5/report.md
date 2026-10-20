# Report for step 0.5 "antpy-modules"

## What was built

The module machinery of section 10 was already in place from 0.1 (module
records sharing the scope dict, live destructured imports through
`Scope.imports`, hoisting with `UNINIT` vars, depth-first init once keyed by
normalized absolute path, import paths relative to the importing file, tag
identity under aliases, exit 0 when `main` returns). The baseline run
(`work/0.5/baseline.log`) had exactly one failure, `programs/smoke-args`
(`unbound name: Os`). This step adds:

1. `bootstrap/antpy/system.py` (new): the `Os` namespace of stdlib.md.
   - `Os.args()`: a ctx native returning `list(interp.args)`, a fresh array
     per call, holding only the arguments after the entry file (G39).
   - `Os.env(name)`: `os.environ.get` after `need_string`; `None` is `null`.
   - `Os.exit(status)`: `need_int`, flush stdout and stderr, `os._exit(status & 0xFF)`.
     Immediate from any goroutine, no deferred blocks, no panic line (G39).
   Wired into `prelude.NAMESPACES` as `"Os": OS`.
2. Import cycle detection in `Interp.load_module` (`bootstrap/antpy/interp.py`):
   a `self.loading` stack of `[path, import line]` pairs, checked before the
   `self.modules` lookup (a loading module is already registered there). A
   cycle is a `LoadError` on the import line of the first module of the cycle,
   message `cycle: a.ant -> b.ant -> a.ant` (basenames), exit 1, nothing on
   stdout. This is where the check-tier tests `mod-import-cycle` and
   `mod-self-import` put their markers (verified by hand, see probes).
3. `main` diagnostics in a new `Interp.check_main`, called from `run_main`:
   a missing `main` (or a `var main`) is reported at the entry module's last
   declaration (G53) as `main: missing or not a function`; a `main` with any
   parameter is refused with `main: takes no parameters` at its line. Both are
   load errors (exit 1, before anything runs, G32). `self.decls` keeps each
   module's lowered declarations for this. The return type is erased by the
   lowering, so `func main(): int` is left to the checker (M3).
4. Docstrings: `bootstrap/antpy/__init__.py` lists `system`; `bootstrap/antpy.py`
   mentions `Os.exit(n)`; `prelude.py` names `system.py`.

Files changed: `bootstrap/antpy/system.py` (new), `bootstrap/antpy/prelude.py`,
`bootstrap/antpy/interp.py`, `bootstrap/antpy/__init__.py`, `bootstrap/antpy.py`.
Probes under `work/0.5/probes/`. No file outside `bootstrap/` and `work/0.5/` was touched.

## Acceptance

Command (verbatim from the roadmap row), log in `work/0.5/test.log`:

    python3 -I src/tests/run.py --run "python3 -I bootstrap/antpy.py run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --tier programs --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /functions/ --filter /expressions/ --filter /types/ --filter /nullability/ --filter /errors/ --filter /stdlib/ --filter /concurrency/ --filter race-memory-safe --filter parallel-sum --filter producer-consumer --filter /modules/ --filter programs/

Last line (exit 0):

    414 checks, 0 failures, 0 warnings (implementation mode, 413 tests)

The same command with `--consistency` (`work/0.5/consistency.log`) gives the
same line, exit 0. `ruff check --select F401,F821,F841,F811 bootstrap/`:
`All checks passed!` (`work/0.5/ruff.log`).

## Test changes

None. `work/TEST-CHANGES.md` is unchanged.

## Decisions and deviations from the plan

- The `try/finally` around the loading stack wraps only the import loop, not
  the hoisting and initializer loops (the plan sketched it around all three).
  Only imports can extend a cycle; initializers never load modules, so the
  stack is complete once the import loop ends. This keeps the hoisting and
  initializer code untouched, which the plan's risk section asked for.
- `check_main` returns the closure so `run_main` reads as one line; the error
  path uses the entry module's absolute path (0.1 used the path as typed).
- Writes through a destructured import (unchecked) go to the exporter's
  binding, as 0.1 chose (G31's field-assignment rule applied to the module
  record). Probe `assign-import`: `count = 5; count += 1` then `C.count` and
  `count` both print `6`. No test relies on either reading; checked, it is a
  compile error.
- A module record renders as `Module` (probe `namespace-render`); G31 leaves
  it open and no test asserts it.
- Two modules exporting only equal-valued `var`s compare `==` (structural
  equality of two `Module` records); unspecified, not asserted, left as is.
- `Os.exit` masks the status to eight bits (what the kernel does to `exit(300)`
  anyway, and it keeps `os._exit` from raising on an `int64` outside C `int`);
  `Os.exit(-1)` is 255 like C. `Os.exit` inside `Mutex.withLock`/`Once.do`
  releases nothing because the process is gone.
- `Os.args` is reached through the `ctx=True` native convention (`interp.args`),
  not a module-level global, so `system.py` imports nothing from `interp.py`
  and the import graph stays acyclic (`values`, `library` <- `system` <- `prelude` <- `interp`).

## Probe results (`work/0.5/probes/`, run by hand)

| Probe | Result |
|---|---|
| `os-args-fresh.ant a b` | pushed copy has length 3, `Os.args()` still 2, `Os.args()[1]` is `b`; `Os.args(1)` panics `too many arguments`, exit 2 |
| `ANT_PROBE=1 os-env.ant` | `1`, `null` for an unset name, `Os.env(1)` panics `wrong kind: expected a string` |
| `os-exit-codes.ant` with `3`, `0`, `300`, `neg`, `float`, `bool` | shell status 3, 0, 44, 255; `Os.exit(1.0)` and `Os.exit(true)` panic `wrong kind: expected an int`, exit 2; `before` printed each time, `after` never |
| `os-exit-defers.ant` | stdout is `in goroutine` only (no `main defer`, no `after wait`), status 0, wall time 0.02 s (no 1 s grace) |
| `os-exit-buffered.ant \| wc -l` | 2000 lines, status 3 |
| `cycle/a.ant` | `<abs>/cycle/a.ant:1:1: cycle: a.ant -> b.ant -> a.ant`, exit 1, empty stdout |
| `self-import.ant` | `<abs>/self-import.ant:1:1: cycle: self-import.ant -> self-import.ant`, exit 1 |
| `main-missing.ant` (last declaration on line 3) | `...:3:1: main: missing or not a function`, exit 1 |
| `main-params.ant` (`func main(x: int = 1)` on line 3) | `...:3:1: main: takes no parameters`, exit 1 |
| `main-lambda.ant` (`var main = { x + 1 }`) | `...:1:1: main: missing or not a function`, exit 1 |
| `import-undeclared/` | `loaded`, then `panic: unbound name: nope`, exit 2 (G3) |
| `assign-import/` | `6` and `6` (write-through to the exporter) |
| `two-paths/` (`./lib/x.ant` and `./lib/../lib/x.ant`) | `init x` printed once, `A == B` is `true`, `A.v` is `1` |
| `namespace-render.ant` | `Module` |

The check-tier module tests run by hand under antpy (they are not part of
this step's acceptance, which has no `--check`): `mod-import-cycle` reports
`main.ant:3:1: cycle: main.ant -> ping.ant -> main.ant` (marker on line 3),
`mod-self-import` reports `:3:1: cycle: ...` (marker on line 3),
`mod-main-missing` reports `:4:1: main: missing or not a function` (the last
declaration, line 4), `mod-main-params` reports `:3:1: main: takes no
parameters` (marker on line 3), all exit 1; `mod-main-return` (`func main(): int`)
runs and exits 0, since the return type is the checker's.

## Known limitations

- `func main(): int { 1 }` is not refused: the lowering erases types, so the
  return-type rule waits for the checker (M3).
- `need_int` is `type(v) is int`, so a status computed as a sized integer
  (`int8` etc.) panics `wrong kind`; that is the declared signature
  (`status: int`), and the checker's job otherwise.
- `self.decls` keeps every module's lowered declarations alive for the run;
  negligible for the suite and for `antc`'s own sources.
- The cycle message lists basenames; two modules with the same basename in
  different directories would read ambiguously, but the error's path and
  line identify the start of the cycle exactly.

## Refactor

Applied after the review; the tree is green afterwards (`work/0.5/test.log`:
`414 checks, 0 failures, 0 warnings (implementation mode, 413 tests)`, exit 0;
`ruff check --select F401,F821,F841,F811 bootstrap/` clean).

1. `simplify` pass over the step's diff (single inline pass; the Agent tool
   was not available, so no 4-agent fan-out): the only findings were the two
   points below, which the reviewer had already specified. `system.py` is
   clean (reuses `need_int`/`need_string`, follows the `(names, fn, ctx)`
   native convention of `sync.py`; there is no shared flush helper to reuse).
2. `bootstrap/antpy/interp.py`: `main` is now diagnosed before anything runs
   (G32). `load_module` calls `check_main(path, decls)` on the entry module's
   lowered declarations right after `lower_file`, before its imports load and
   before any initializer runs; `check_main` finds the `Func` declaration
   named `main` (missing: `main: missing or not a function` at the last
   declaration, G53; `decl[3]` non-empty: `main: takes no parameters` at its
   line). `run_main` is `load_module(abspath, None)` then
   `call(module.fields["main"], ...)`. Deleted: `self.decls` (and the
   per-module store, so the "keeps every module's declarations alive"
   limitation above no longer applies) and the `type(main) is not Closure or
   main.is_lambda` checks: a `Func` declaration always yields a non-lambda
   closure; a `var main` redeclaring it is a checker error and, unchecked,
   panics `not a function` at the call (probe
   `work/0.5/probes/init-before-main/var-main.ant`, exit 2). This supersedes
   item 3 of "What was built". Probe `init-before-main/no-main.ant` (`var x
   = side()` with no `main`) no longer prints `initializer ran` before the
   load error.
3. `bootstrap/antpy/interp.py`: the loading stack holds immutable
   `(importer path, import line)` pairs, pushed around each recursive
   `load_module` call (`try/finally` per import) instead of a mutable
   `[path, 0]` cell whose line was overwritten inside the loop; the cycle
   check reads `start, start_line` straight from the entry. Chains and lines
   are unchanged: `mod-self-import :3:1`, `mod-import-cycle main.ant:3:1:
   cycle: main.ant -> ping.ant -> main.ant`, probe `cycle/a.ant :1:1`.
   `two-paths` still prints `init x` once.

Hand checks after each change: `mod-main-missing :4:1`, `mod-main-params
:3:1`, `mod-main-return` exit 0, `mod-self-import :3:1`, `mod-import-cycle
:3:1` (markers did not move); the `--filter /modules/ --filter programs/`
subset after each change: `39 checks, 0 failures` (`work/0.5/refactor-subset.log`).
