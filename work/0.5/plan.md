# Plan for step 0.5 "antpy-modules"

Deliverable (ROADMAP.md row 0.5): imports (destructure, alias, namespace), live read-only bindings, depth-first init once, hoisting, `main`, exit status, `Os.args/env/exit`. Allowed directories: `bootstrap/`, `work/0.5/`.

Acceptance (verbatim):

    python3 -I src/tests/run.py --run "python3 -I bootstrap/antpy.py run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --tier programs --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /functions/ --filter /expressions/ --filter /types/ --filter /nullability/ --filter /errors/ --filter /stdlib/ --filter /concurrency/ --filter race-memory-safe --filter parallel-sum --filter producer-consumer --filter /modules/ --filter programs/

## 1. Starting point (measured)

The acceptance command on the current tree (`work/0.5/baseline.log`):

    414 checks, 1 failures, 0 warnings (implementation mode, 413 tests)

The one failure is `programs/smoke-args/`: `panic: unbound name: Os`. Every other test the step selects already passes, because `Interp.load_module` (0.1) implements the module model of section 10: a module is a `Record(MODULE_TAG, vars)` whose `fields` dict *is* the module scope's `vars` dict; namespace imports bind that record; destructured imports are live lookups through `scope.imports` (local name -> (exporter vars, exported name)); `func`/`type` are hoisted, `var` names start as `UNINIT` (`not initialized` when read through a call); modules are keyed by normalized absolute path in `self.modules` (initialized once, depth first in source order); a missing file is a `LoadError` (`path:line:col: not found: ...`, exit 1).

The tests this step selects beyond 0.4 (all read and accounted for):

| Test | Pins | Status |
|---|---|---|
| `programs/smoke-args` | `Os.args()` is only the arguments (G39) | **fails** |
| `run/modules/mod-namespace-value` | `Geometry == Geometry` (G31) | passes |
| `panic/modules/mod-exit-status-panic` | panic in an imported function, exit non-zero, stdout before it | passes |
| `panic/modules/mod-var-read-before-init-via-call` | `not initialized` through a hoisted function called from an earlier initializer | passes |
| `unchecked/modules/mod-assign-namespace-rt` | `Geometry.counter = 5` mutates the exporter's binding (G31) | passes |
| `unchecked/modules/mod-private-readable-rt` | every module field readable at run time | passes |
| `programs/mod-*` (11 dirs) | destructure, alias, namespace, live var, diamond init order (`D B C A`), var order, imported var in initializer, relative paths from the importing file, tag identity under aliases, imported `main` ignored, exit 0 on return | pass |
| `programs/prog-*`, `ex-accounts*`, `race-memory-safe` | whole programs | pass |

So the implementation work is: the `Os` namespace, plus the two module-loading gaps that 0.1's report explicitly deferred to 0.5 (import cycle detection; `main` diagnostics), plus probes that confirm the deliverable's semantics rather than trusting one test each.

## 2. Files

| File | Action | Content |
|---|---|---|
| `bootstrap/antpy/system.py` | **create** | the `Os` namespace: `OS` registry with `args`, `env`, `exit` (section 3) |
| `bootstrap/antpy/prelude.py` | change | `from antpy.system import OS`; add `"Os": OS` to `NAMESPACES` after `"Time": TIME`; docstring sentence mentions `system.py` |
| `bootstrap/antpy/__init__.py` | change | add `system   the Os namespace of stdlib.md (0.6 adds File, Json, Time.now/monotonic)` to the docstring's module list |
| `bootstrap/antpy/interp.py` | change | import cycle detection in `load_module` (section 4); `main` diagnostics in `run_main` (section 5) |
| `bootstrap/antpy.py` | change (docstring only) | add "`Os.exit(n)` exits n" to the exit-status sentence |
| `work/0.5/probes/*.ant` | create | the probes of section 7 (not tests; never under `src/tests`) |
| `work/0.5/report.md`, `work/0.5/test.log` | create | the step report and the acceptance log |

Import graph stays acyclic: `values`, `library` <- `system` <- `prelude` <- `interp`. `system.py` needs only `need_int`/`need_string` from `library.py`; `Os.args` reaches the interpreter through the `ctx=True` native convention (`fn(interp, frame, *args)`), not through a module-level global.

No test under `src/tests` needs a change; `work/TEST-CHANGES.md` stays as it is.

## 3. `bootstrap/antpy/system.py`

Write it literally like this (registry shape `(param names, fn[, True])` is what `prelude.namespace` expects):

```python
"""The Os namespace of stdlib.md (Os.args, Os.env, Os.exit). The other I/O
namespaces (File, Json, Time.now/monotonic) arrive in roadmap step 0.6."""
import os
import sys

from antpy.library import need_int, need_string


def os_args(interp, frame):
    """A fresh array each call (section 11: a function returns a new value),
    holding only the arguments after the entry file: no program name (G39)."""
    return list(interp.args)


def os_env(name):
    return os.environ.get(need_string(name))      # None is Ant's null


def os_exit(status):
    """Ends the process at once, from any goroutine, without running deferred
    blocks (G39). Flushes first: print writes to a block-buffered pipe."""
    need_int(status)
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(status & 0xFF)


OS = {
    "args": ([], os_args, True),
    "env": (["name"], os_env),
    "exit": (["status"], os_exit),
}
```

Why each line is the way it is:

- `Os.args`: `Interp.args` is already `argv` after `FILE` (`antpy.py` strips `run` and an optional `--unchecked`). `smoke-args` expects `2` then `one` for `args: one two`; the io tier's runner prepends the temporary directory itself. `list(...)` so `Array.push(Os.args(), ...)` cannot change the next call's result. With `[]` params, `Os.args(1)` panics `too many arguments` through `call_native`.
- `Os.env`: `need_string` first (`wrong kind` for a non-string); `os.environ.get` gives `str` or `None`, which are Ant `string` and `null`. `python3 -I` ignores `PYTHON*` variables only; the runner merges `env:` pairs into the subprocess environment, so nothing else is needed.
- `Os.exit`: `need_int` is `type(v) is int`, so `true` and `int8(1)` are `wrong kind` (signature `status: int`). It must be `os._exit`, not `sys.exit` and not an exception: the program runs on a worker thread whose `except BaseException` would report `SystemExit` as an internal error, and `sys.exit` on a goroutine thread would only end that thread. `os._exit` skips Python's buffer flush, hence the explicit flushes (otherwise `io/os-exit-status` loses its `before`). The mask `& 0xFF` is what the kernel does to `exit(300)` anyway (44) and keeps `os._exit` from raising `OverflowError` on an `int64` outside C `int`; `Os.exit(-1)` gives 255 like C.

## 4. Import cycle detection (`Interp.load_module`)

Today `self.modules[path] = module` is registered *before* the module's imports load, so a cycle silently returns a half-built record (no functions hoisted yet), which shows up later as `unbound name`/`no field`. Section 10: "Import cycles are a compile error." The bootstrap reports it as a load error before running (the same phase as `not found`, G32), exit 1, nothing printed to stdout.

Data structure: `self.loading = []` in `Interp.__init__`, the depth-first stack of modules whose initialization is in progress, as `[path, line]` pairs where `line` is the import line that module is currently resolving.

Shape (keep everything else in `load_module` as it is):

```python
def load_module(self, path, importer, line=0):
    for i, (p, _) in enumerate(self.loading):          # before the self.modules check:
        if p == path:                                  # a module on the stack is already registered there
            chain = [q for q, _ in self.loading[i:]] + [path]
            start, start_line = self.loading[i]
            raise LoadError(start, start_line, 1,
                            "cycle: " + " -> ".join(os.path.basename(q) for q in chain))
    module = self.modules.get(path)
    if module is not None:
        return module
    ... read / parse / lower / scope / register in self.modules (unchanged) ...
    self.loading.append([path, 0])
    try:
        for _, line, target, pattern in imports:
            self.loading[-1][1] = line                 # set before the recursive call
            dep = self.load_module(...)
            ... bind namespace or live names (unchanged) ...
        ... hoisting and initializers (unchanged) ...
    finally:
        self.loading.pop()
    return module
```

The error is placed on the import line of the module that *starts* the cycle (the first module of the cycle on the stack), which is where the check-tier tests put their marker: `check/modules/mod-import-cycle` marks `main.ant`'s `import "./ping.ant"`, and `mod-self-import` marks the self-import line. The message text after `cycle:` is free (the phrase is the only thing a checker test matches); basenames keep it readable.

Keep the early registration in `self.modules`: with the stack check first it is harmless, and the diamond test (`mod-init-order-diamond`) still initializes `d.ant` once.

## 5. `main` diagnostics (`Interp.run_main`), small and optional

Do this after the acceptance is green; skip it if anything about it misbehaves. Section 10: "`main` takes no parameters and returns `void`"; G32/G53: a missing `main` is reported before running, at the entry module's last declaration. Today a missing `main` is reported at line 1 and `func main(x: int)` runs and panics `missing argument` (exit 2) instead of being refused (exit 1).

- In `load_module`, after `lower_file`: `self.decls[path] = decls` (`self.decls = {}` in `__init__`). The lowered decls are `("Var", line, target, expr)`, `("Func", line, name, params, body)`, `("Type", line, name)`.
- In `run_main`, after `load_module`:

```python
decls = self.decls[abs_path]
main = module.fields.get("main")
decl = next((d for d in decls if d[0] == "Func" and d[2] == "main"), None)
if decl is None or type(main) is not Closure or main.is_lambda:
    raise LoadError(abs_path, decls[-1][1] if decls else 1, 1, "main: missing or not a function")
if main.params:
    raise LoadError(abs_path, decl[1], 1, "main: takes no parameters")
```

The return type is erased by the lowering (`params(sig)` keeps names and defaults only), so `func main(): int` stays the checker's job (M3). Any parameter, with or without a default, is refused: the signature is fixed.

## 6. Order of work

1. `system.py`, the `prelude.py` wiring, the `__init__.py` docstring. Run `... --tier programs --filter smoke-args` (expect `1 checks, 0 failures`), then the full acceptance command (expect `414 checks, 0 failures`). This alone makes the step green.
2. Probes of section 7 for `Os` (args fresh, env set/unset/wrong kind, exit codes, exit from a goroutine with pending defers, exit with buffered output).
3. Cycle detection (section 4). Run `... --filter /modules/ --filter programs/` (44 tests) and the cycle probes.
4. `main` diagnostics (section 5), same subset plus the `main` probes.
5. Full acceptance into `work/0.5/test.log` (exit 0, last line `0 failures`); the same command with `--consistency` added; `ruff check --select F401,F821,F841,F811 bootstrap/`.
6. `work/0.5/report.md`: what was built, the acceptance line, decisions, probe results, known limitations (sections 8 and 9 below list the decisions to record).

Sections of the test suite in the order to look at if something regresses: `/modules/` and `programs/mod-*` (touched code path), then `programs/` as a whole, then the 0.4 concurrency subset (`Os.exit` is the only new process-exit path and it does not touch `run_program`'s grace loop), then everything else.

## 7. Probes (`work/0.5/probes/`, run by hand, results go in the report)

Each is one program under `python3 -I bootstrap/antpy.py run <file> [args]`; multi-file ones are a subdirectory.

| Probe | Expect |
|---|---|
| `os-args-fresh.ant` with args `a b`: push onto `Os.args()`, then `Array.length(Os.args())` | still 2; `Os.args(1)` panics `too many arguments` |
| `os-env.ant` run as `ANT_PROBE=1 python3 ...` | `Os.env("ANT_PROBE")` is `"1"`, an unset name is `null`, `Os.env(1)` is `wrong kind` |
| `os-exit-codes.ant` (one program per value, or an argument selecting it): `Os.exit(3)`, `Os.exit(0)`, `Os.exit(300)`, `Os.exit(-1)`, `Os.exit(1.0)`, `Os.exit(true)` | shell status 3, 0, 44, 255; the last two panic `wrong kind` (exit 2) |
| `os-exit-defers.ant`: `defer { print("d") }` in main, a goroutine whose `defer` would release a `WaitGroup` calls `Os.exit(0)` | stdout is the lines before the exit only, status 0, returns at once (no 1 s grace) |
| `os-exit-buffered.ant`: print 2000 lines then `Os.exit(3)`, run with stdout piped to `wc -l` | 2000 lines, status 3 |
| `cycle/` (`a.ant` imports `b.ant`, `b.ant` imports `a.ant`, `a.ant` is the entry) and `self-import.ant` | `<abs path of a.ant>:<its import line>:1: cycle: a.ant -> b.ant -> a.ant`, exit 1, empty stdout; the self-import names the file twice |
| `main-missing.ant`, `main-params.ant` (section 5) | exit 1, message starts `main`, line of the last declaration / of `main` |
| `import-undeclared/`: `import "./a.ant" { nope }` and read `nope` inside `main` | loads fine; the read panics `unbound name: nope` (G3: import failures unchecked are `unbound name`), exit 2 |
| `assign-import/`: `import "./c.ant" { count }` then `count = 5` and `count += 1` in main, and `C.count` read through a second namespace import | `6` (writes go through to the exporter: 0.1's reading of G31, kept) |
| `two-paths/`: `import "./lib/x.ant" A` and `import "./lib/../lib/x.ant" B` where `x.ant` prints in an initializer | printed once; `A == B` is `true` |
| `namespace-render.ant`: `print(Geometry)` | `Module` (G31 leaves rendering open; record what it is) |

## 8. Spec rules that are easy to get wrong

1. `Os.args()` holds only the arguments: no interpreter name, no entry file (G39). `antpy.py` already strips `run` and `--unchecked`; do not strip anything else, an argument may itself look like a flag.
2. `Os.exit` runs no deferred blocks, ends every goroutine, works from any goroutine, and must flush stdout before `os._exit` (section 3). It is not a panic: no `panic:` line, no trace.
3. Imports are live: a destructured name is looked up in the exporter's `vars` on every read (`Scope.imports`), never copied at import time; a namespace is the exporter's record whose `fields` *is* its scope dict. `mod-live-var` reads `count` after `bump()` through both routes. Do not "optimize" either into a copy.
4. Depth first, source order, once: a module's imports finish initializing before its own hoisting and initializers run; a module reached twice (the diamond) initializes once; the entry module is initialized last (`D B C A`).
5. Hoisting: `func` and `type` are bound before any initializer runs (an initializer may call a function declared below it: `mod-init-var-order`); a `var` is `UNINIT` until its initializer finished, read through a call before that is `not initialized` (`mod-var-read-before-init-via-call` prints `calling` first, then panics). A direct forward reference is a compile error (checker), which the bootstrap also reports as `not initialized`: fine, it is unchecked behaviour.
6. `main` of an imported module is an ordinary private function; only the entry module's `main` runs (`mod-main-in-imported-ignored`).
7. Import paths are relative to the importing file, extension included: `os.path.join(dirname(importer), target)`, normalized. No search path, no extension guessing, no resolution against the current directory. A missing file is a load error (`not found`), exit 1, before anything runs (G32).
8. A `type` export is its tag object; `is` under any alias in any module compares tag identity (`mod-tag-identity`). Never mint a new `Tag` for an import or an alias.
9. Unchecked writes: `Geometry.counter = 5` must change what `Geometry.read()` returns (G31); this follows from the shared dict and `check_field_target` (the field must already exist, so `Geometry.nope = 1` is `no field`).
10. Exit statuses: 0 when `main` returns (even by an early `return`: `mod-exit-status-return`), 2 on a panic (`panic: <message>` and a trace on stderr; any non-zero satisfies the runner), 1 on a load error (`path:line:col: message` on stderr), `n & 0xFF` for `Os.exit(n)`.
11. The cycle check must run before the `self.modules` lookup, since a module on the loading stack is already registered there.

## 9. Decisions to record in the report

- Writes through a destructured import (unchecked) go to the exporter's binding, as 0.1 chose (G31's "field-assignment rule" applied to the module record). No test in the suite relies on either reading; it is a compile error when checked.
- Two modules that export only equal-valued `var`s compare `==` (structural equality of two `Module` records); unspecified by G31, not asserted anywhere, left as is.
- A module record renders as `Module`; G31 leaves it open.
- `Os.exit` masks to eight bits; `Os.exit` from inside `Mutex.withLock`/`Once.do` releases nothing because the process is gone.

## 10. Risks

- Forgetting the flushes in `Os.exit` loses buffered stdout under the runner (pipe, block buffering): `io/os-exit-status` (0.6) would fail while the step's own selection stays green. The buffered probe catches it now.
- A cycle check placed after the `self.modules` lookup never fires; a cycle check that compares non-normalized paths misses cycles through `..` segments. All paths in `load_module` are already normalized absolute paths; compare those.
- Changing `load_module`'s control flow (the `try/finally` around imports and initializers) can regress all 44 module/program tests at once; run the subset after each edit, and keep the hoisting/initializer code untouched inside the `try`.
- `self.decls` keeps every module's lowered declarations alive for the run; negligible for the suite, and `antc`'s own sources in M1 are a few thousand lines.
- `Os.exit` from a goroutine while main sits in `WaitGroup.wait` or a `Mutex.lock`: `os._exit` does not wait for threads, so the exit is immediate; nothing to handle, but confirm with the goroutine probe that no "internal interpreter error" appears and that the process does not linger for the 1 s grace period.
- `need_int` rejects sized integers, so a program computing its status as `int8` panics `wrong kind`; that is the declared signature (`status: int`) and the checker's job otherwise.
