# Plan for step 2.4 "rt-modules"

Row: "module sections, init order, imports, `main`, `Os.args/env/exit`". Directories: `runtime/`, `src/antc/`,
`Makefile`, `bootstrap/antpy/system.py` (`Os.exec` only; nothing to do there), plus `work/2.4/`.

Acceptance (verbatim, run from the repository root; about 3 minutes under antpy for these 40 tests; run it in
the background with output to `work/2.4/test.log` and wait):

    python3 -I src/tests/run.py --timeout 600 --run "python3 -I bootstrap/antpy.py run src/antc/main.ant run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run src/antc/main.ant run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --tier programs --filter /smoke/ --filter /modules/ --filter programs/ --exclude /concurrency/ --exclude race-memory-safe --exclude parallel-sum --exclude producer-consumer

## 0. Where the tree stands (measured, `work/2.4/baseline.log`)

The acceptance command on the committed tree (HEAD `f000d3d`, "2.3 rt-stdlib") ends with
`41 checks, 0 failures, 0 warnings (implementation mode, 40 tests)`, exit 0. The 40 tests are the five
`smoke` files, `run/modules/mod-namespace-value`, `panic/modules/mod-exit-status-panic` and
`mod-var-read-before-init-via-call`, `unchecked/modules/mod-assign-namespace-rt` and `mod-private-readable-rt`,
and the 30 `programs/` directories that are not the three concurrent ones.

Step 2.1 built everything this row names, and `work/2.1/report.md` describes it:

- `src/antc/codegen/modules.ant`: the module graph loaded depth first in import order (post-order, entry
  last; one `Loaded` per resolved path, so two spellings of one file are one module), `resolve` folding `.`
  and `..` against the importing file's directory, `not found: <path>` at the import line, `cycle: a.ant ->
  b.ant -> a.ant` at the first import of the cycle, the top-level name table (`names`/`nameIndex`: every
  `var` target name, `func`, `type`, in declaration order, deduplicated), the import bindings (`namespace`, or
  `name` with the exporter's field index, -1 when absent), `checkMain` (`main: missing or not a function` at
  the last declaration, G53; `main: takes no parameters`).
- `src/antc/codegen/emit.ant`: one C file with a section per module: `static AntValue M_k` and
  `M_k_state`, `F_k_<name>` per declared function, `init_k` (`emitInit`: return if running or done; `init_dep()`
  for every import in source order with an unwind check after each; `ant_module_new(shape)` with every field
  `K_UNINIT`; tags and function closures stored (hoisting); then the `var` initializers in order as the body
  of a frame named `module`; `ant_finish_init` turns a top-level `return` into `return outside a function`,
  a stray `break`/`continue` into `break or continue outside a loop`). Name resolution (`emitIdent` /
  `storeName`): local → the module's own top-level name (`ant_module_get(M_k, idx)`, which panics
  `not initialized: <name>` on `K_UNINIT`) → import binding (namespace: `M_dep`; name: `ant_module_get(M_dep,
  field)`, live; a name the exporter lacks: `unbound name` at the use site) → prelude global
  (`ant_global`, `unbound name` when absent). Writes go through `ant_module_set` for own vars and destructured
  imports (G31). The generated `main`: `ant_runtime_init(argc, argv)` (stores `argv[1..]` for `Os.args`, G39),
  symbols, prelude, constants, then `ant_run_main(init_entry, &M_entry, <field index of main>)`.
- `runtime/record.c`: `ant_module_new/get/set`, the module tag (renders as `Module`), `ant_field_get`
  also panics `not initialized` on a `K_UNINIT` field (namespace reads). `runtime/unwind.c`:
  `ant_run_main` runs the entry init and `main` on the 512 MB thread, reports a pending panic
  (`panic: <message>` + trace, stdout flushed first) and exits 2, else 0; `ant_exit` = `fflush` both streams +
  `_exit`. `runtime/io.c`: `Os.args` (a fresh array per call), `Os.env` (`getenv`, `null` when unset),
  `Os.exit` (`ant_need_int`: `wrong kind` unless `int`; status masked to 8 bits), `Os.exec`.
- `src/antc/codegen/build.ant` / `src/antc/main.ant`: `antc run` builds a temporary binary, runs it through
  `Os.exec` with the program arguments (environment inherited) and exits with its status; load errors are
  `path:line:col: message` on stderr, exit 1, before anything runs (G32).

The planner ran 14 hand probes under both the bootstrap and antc (init order, re-export through a namespace,
empty modules, a module with only a `type`, an import of a name the exporter lacks, `Math = 1`, a top-level
`return`, `Os.args` with `"a b"` and `""`, `Os.env` set/unset, a 2-cycle, a missing import under a `..` path,
a parse error in an imported module, `var main`, `A == B` for two modules with equal exports, `Os.exit` from a
lambda under `Array.map`, `Os.exit(int8(1))` with pending defers, `Os.exit(9)` with pending defers). All agree
except two unchecked cases that no test pins and the spec settles for the runtime (section 2.C below):
`A.B` (a namespace import reachable as a field of the importer's record under the bootstrap) and `Math = 1`
(the bootstrap rebinds a prelude or namespace name; the runtime panics `unbound name`).

So no defect is known. This step is, like 2.2 and 2.3, a verification sweep of the row against the
bootstrap and the spec, fixing whatever it finds. Nothing under `src/antc/` or `runtime/` is expected to
change unless a probe finds a divergence the spec settles against antc.

## 1. Files

| File | Change |
|---|---|
| `work/2.4/probes/run.sh` | the probe runner of section 2.A (step 2.3's `work/2.3/probes/run.sh` extended for directory probes, absolute paths, fixed arguments and one environment variable) |
| `work/2.4/probes/.gitignore` | `*.out` and `*.err` (as in `work/2.3/probes/.gitignore`) |
| `work/2.4/probes/NN-name.ant`, `work/2.4/probes/NN-name/main.ant` (+ helper modules) | the probe set of section 2.B |
| `work/2.4/baseline.log` (exists), `work/2.4/test.log`, `work/2.4/consistency.log`, `work/2.4/probes.log` | run logs (git-ignored by `work/**/*.log`) |
| `work/2.4/report.md` | the report, in the shape of `work/2.3/report.md` |
| `runtime/*.c`, `runtime/ant.h`, `src/antc/codegen/modules.ant`, `src/antc/codegen/emit.ant`, `src/antc/codegen/build.ant`, `src/antc/main.ant` | only for a divergence found in section 2.C that the spec settles against the runtime or antc; nothing planned |
| `Makefile`, `bootstrap/antpy/system.py` | nothing |

`make runtime` must have been run (it has: `runtime/libantrt.a` is present and the baseline passed); rerun it
after any C change, since `antc build` links the archive and never rebuilds it.

## 2. Approach and order

### A. The probe runner (`work/2.4/probes/run.sh`)

Copy `work/2.3/probes/run.sh` and change:

1. `D=work/2.4/probes`.
2. Probes are files `NN-name.ant` or directories `NN-name/` with a `main.ant` (multi-module cases):
   `for f in $D/[0-9][0-9]-*; do case "$f" in *.ant) src=$f;; *) src=$f/main.ant;; esac; n=$(basename "$f" .ant); ...`.
3. Pass the source as an absolute path (`"$PWD/$src"`, the script already `cd`s to the repository root) to
   both runs: the bootstrap prints absolute paths in load errors (`os.path.abspath`), antc prints the path as
   given, so diagnostics only compare when both get the same absolute path (the runner passes absolute paths
   too).
4. Run every probe with the fixed arguments `alpha "b c" ""` after the file (both commands: `... run "$abs"
   alpha "b c" ""`) and with `ANT_PROBE=1` in the environment (`ANT_PROBE=1 python3 -I ...`), so the `Os`
   probes have something to read; probes that ignore them are unaffected.
5. Keep the comparison as it is: stdout, `exit=<status>`, and the first stderr line (a load error is one
   line; a panic's first line is `panic: <message>`).

Usage from the repository root: `sh work/2.4/probes/run.sh [NN ...]`; each probe prints `SAME` or `DIFF`
with the diff. Each program runs twice under antpy (the antc run builds with clang, about 6 s), so the full
set takes a few minutes; run it in the background to `work/2.4/probes.log`.

Writing rules for probes: plain programs with `func main()` and `print`; a statement starting with `{` is a
lambda, so a nested block is `if true { ... }`; panicking cases are separate programs named `NN-pK-name`
(one panic each, since a panic ends the program); multi-module probes keep their helper modules inside the
probe's directory.

### B. The probe set (expectations from `ant-lang.md` section 10/12, `stdlib.md` Os, G3, G24, G31, G32, G39, G53, G61)

1. `01-init-order/`: a diamond (`main` imports `b` and `c`, both import `d`), `d` imported again directly by
   `main` after `b` and `c`, one module imported twice in one file (destructured and as a namespace), the
   same file reached as `./lib/x.ant` from `main` and as `../lib/x.ant` and `./lib/../lib/x.ant` from a module
   in `app/`; every module prints its name in a top-level initializer. Expected: each name once, depth first in
   source order (`d`, `b`, `c`, ... then `main`), and `X == Y` true for two namespace bindings of one file.
2. `02-hoisting/`: an initializer calling a function declared below it that reads an earlier var; a type
   used in an initializer before its `type` line (`var p = P{ a: 1 }` above `type P = { a: int }`); mutual
   recursion between top-level functions; a function declared above a var that reads it, called from `main`
   after init (G61: works); a top-level `var inc = func() { count += 1 }` assigning a top-level var, called
   from `main` three times (`count` is 3); an initializer with `if`/`while`/`for` blocks whose `defer`s print at
   block exit, in reverse order, before the next initializer runs; a top-level destructuring
   `var { a, b: c } = P{ a: 1, b: 2 }` (two fields of the module record).
3. `03-p*-not-initialized` (separate programs): `var early = f()` where `f` reads the later `late`
   (`not initialized: late`, after `f`'s print); `var a = b` with `b` declared later (unchecked;
   `not initialized: b`); `var { x } = later()` where `later` reads a later var; a hoisted function reading a
   later var *through a namespace from the entry module's initializer* is impossible without a cycle, so do
   not try to build one. Also `03-p4-unbound`: `import "./m.ant" { nope }` then `print("loaded")` and
   `print(nope)` in `main` (`loaded`, then `unbound name: nope`, exit 2, G3); `03-p5-no-field`: `M.nope`
   (`no field: nope`).
4. `04-live-imports/`: the shape of `programs/mod-live-var` plus writes: `count = 5` through the destructured
   name, then `Counter.count` and `count` and the exporter's `read()` all give 5 (G31); `Counter.count = 6`
   likewise; `count += 1` through the import; a second importer module sees the same value; a copy taken
   earlier keeps its value.
5. `05-namespace-values/`: `print(M)` (`Module`), `print([M])` (`[Module]`), `M == M`, `M != M`, `M == N` for
   two modules with the same export names and values (both `false`: functions compare by identity), `M is Map`
   (`false`), `M.Point` as a value (`Point`), `M.Point{ x: 1 }`, `v is M.Point`, `M.f` stored in a var and
   called, `String.from(M)`; `05-p1-for-module`: `for x in M {}` (`not iterable`); `05-p2-is-nontag`:
   `v is M.counter` (`not a tag`); `05-p3-lit-nontag`: `M.counter{ }` (`not a tag`).
6. `06-tag-identity/`: `programs/mod-tag-identity` plus `Shapes.Circle == Disc` (`true`, one tag under two
   names), a tag of the same name declared in two modules compared with `==` (`false`) and in `is` (`false`),
   `Record.same` on records built through different aliases of one tag.
7. `07-main/`: `export func main()` in the entry; a `main` in an imported module that would panic if run; `main`
   leaving through `return` inside a `while` (exit 0); `main` with a `defer` that prints after the body;
   `07-p1-main-panics-after-output` (`before` on stdout, `panic: boom`, exit 2); `07-p2-defer-panics`
   (a panic in `main`'s deferred block, exit 2); `07-p3-main-value`: `func main() { 1 }` unchecked (value
   dropped, exit 0; this is a `run`-style check, not a panic).
8. `08-p*-init-failures` (separate programs, each with `print("before")` where it applies): a `panic("x")` in
   an imported module's initializer (entry's initializers and `main` never run, `before` from earlier
   initializers kept, exit 2); the same in the entry module's initializer; `Os.exit(3)` from an initializer
   (status 3, nothing after); `var x = return 5` (`return outside a function`); `var y = if true { break } else
   { 0 }` (`break or continue outside a loop`); a `return` inside a `for` body in an initializer (`return
   outside a function`).
9. `09-os-args.ant`: `Os.args()` is `["alpha", "b c", ""]` (length 3, each element in brackets); pushing onto
   the result does not change the next call's length (fresh array, section 11); `Os.args() == Os.args()`;
   `09-p1-args-extra`: `Os.args(1)` (`too many arguments`); `09-p2-args-named`: `Os.args(x: 1)`
   (`unknown parameter`).
10. `10-os-env.ant`: `Os.env("ANT_PROBE")` is `1`, `Os.env("ANT_PROBE_UNSET_X")` is `null`, `Os.env("")` is
    `null`, `Os.env("PATH") != null`, `Os.env(name: "ANT_PROBE")`; `10-p1-env-kind`: `Os.env(1)` (`wrong kind`);
    `10-p2-env-missing`: `Os.env()` (`missing argument`).
11. `11-*-os-exit` (separate programs): `Os.exit(3)` after a print (`exit=3`, the print on stdout);
    `Os.exit(0)`; `Os.exit(300)` (`exit=44`); `Os.exit(-1)` (`exit=255`); `Os.exit(9)` with a pending `defer` in
    `main` and one in an enclosing `if true { }` block (nothing deferred prints, G39); `Os.exit(5)` from a
    lambda under `Array.map`, from a `for` body over a user sequence, from inside `Mutex.withLock`, from a
    deferred block, from inside an `Iter.map` stage, from `go { Os.exit(6) }` (synchronous in both until 2.6;
    exit 6); 2000 printed lines then `Os.exit(3)` (all 2000 lines on stdout: the runtime's 64 KB stdout buffer
    must be flushed); `Os.exit(1.0)`, `Os.exit(true)`, `Os.exit(int8(1))` (`wrong kind`, exit 2, and the
    pending defers *do* run since this is a panic); `Os.exit()` (`missing argument`).
12. `12-*-diagnostics` (load errors: exit 1, one `path:line:col: message` line on stderr, empty stdout, and
    nothing of the program runs, G32): a missing import (`not found: <resolved absolute path>` at the
    importing file, the import's line, col 1); `import "./geometry"` without the extension (`not found`); a
    2-cycle, a 3-cycle (`cycle: a.ant -> b.ant -> c.ant -> a.ant` at the first import of the cycle, basenames),
    a self-import; no `main` (reported at the last declaration's line); `main(x: int)`; `var main = { x }`
    without a `func main` (`main: missing or not a function`); a parse error in an imported module (that file's
    path and position); a file whose initializer would print, with a missing `main` (nothing printed: the check
    precedes initialization). The entry file missing cannot be a probe (the runner needs a file); check by hand
    that both print `<path>:0:1: not found: <path>` or record the difference.
13. `13-privacy/`: a private `var` and a private `func` read and called through a namespace and through
    destructuring (section 12: every field is readable at run time).
14. `14-shadowing/`: `var Math = 1` at the top level of a module (inside it `Math` is 1 and `Math.PI` is
    `not a record`, separate panic program; another module still sees the prelude `Math`); a local `var print =
    1` inside a function (the local wins; `print` outside it still works); `import "./a.ant" { f: print }` (the
    import wins over the prelude inside that module); a module that declares `var x` and also imports `{ x }`
    (unchecked: the module's own `x` wins in both); a parameter named like a top-level var.
15. `15-stdout-order.ant` / `15-p1-order-panic`: prints from three modules' initializers interleaved with
    `main`'s prints, then a panic: stdout holds every line in order, exit 2.

Expected `DIFF`s, known before the sweep (keep the runtime, see C): a probe reading an importer's namespace
import through its own namespace (`A.B.v`) and a probe assigning to a namespace import or a prelude name.
Put both in explicitly (`16-reexport-through-namespace/`, `16-p1-assign-namespace-name`, `16-p2-assign-prelude-name`)
so that the log records them.

Run everything, save `work/2.4/probes.log`, and list every `DIFF` in the report.

### C. Settle every DIFF by the spec

For each `DIFF`, find the sentence in `ant-lang.md` section 10/12, `stdlib.md`, or the GAPS entry that decides
it. When the spec supports the bootstrap, fix the runtime or the emitter (the smallest change, in the file
section 0 names for that behaviour) and rerun the probe and the affected test subset. When the spec supports
the runtime, or is silent and the bootstrap's behaviour is an artefact, keep the runtime and record the probe,
the reading and the reason in the report. `bootstrap/` is closed to this step; never change a test (none of
the selected tests contradicts the spec).

The two known cases, already decided:

1. Re-export through a namespace (`A.B` where `a.ant` has `import "./b.ant" B`): the bootstrap stores a
   namespace import in the module's own `vars`, which are also the module record's fields, so `A.B` is a field
   read; the runtime's module record has one field per `var`/`func`/`type` (`modules.ant` `collectNames`;
   `docs/runtime-design.md` section 1: "a record whose shape lists every top-level name (var, func, type)"),
   so `A.B` panics `no field: B`. Section 10: "Its top-level scope is a record whose fields are its
   declarations" and "There is no re-export". Keep the runtime; do not add import names to the shape (it would
   also change what `M == N` compares and what `print(M)`'s shape holds).
2. Assignment to a namespace import name or a prelude name (`Geometry = 1`, `Math = 1`, `print = 1`) when
   unchecked: the bootstrap rebinds (for a prelude name, in the shared prelude scope); the runtime panics
   `unbound name: <name>` (`emit.ant` `storeName`). Section 10 makes both a compile error and says the
   prelude names "count as imports"; the unchecked outcome is unspecified and untested. Keep the runtime's
   panic; record it. Do not spend time on a `cannot assign` phrase here.

### D. Acceptance, consistency, report

1. `make runtime` (clean under `-Wall -Wextra`) if any C file changed; then the acceptance command in the
   background into `work/2.4/test.log`; it must exit 0 and end with `0 failures`.
2. The same command with `--consistency` into `work/2.4/consistency.log` (gate 2.G requires it; the
   run/panic tiers must behave the same unchecked, which is trivially so while `--unchecked` is ignored). The
   two runs may overlap: `antc build` names its temporary files by clock and hash, and the runner builds tests
   side by side.
3. `work/2.4/report.md`: status with the last line of each log and the baseline; what the row consists of and
   where it lives (section 0, briefly); the probe set with its `SAME`/`DIFF` tally, every kept `DIFF` with its
   reading, and every fix with the probe and the spec sentence that forced it; decisions and deviations from
   this plan; "Test changes: none" (unless a test had to change, in which case cite the section and append
   the line to `work/TEST-CHANGES.md`); known limitations carried forward (below).
4. The refactor pass of the workflow follows; keep the probes and logs out of it.

Known limitations to carry forward in the report (not this step's work): `antc run` spawns the binary and
waits instead of `execv` (the design's section 4 wording; under antpy there is no exec, and the exit status
is relayed, 128 + signal when killed); the paths antc prints in load errors are the entry path as typed plus
resolved imports (the bootstrap absolutizes the entry), which the runner never sees since it passes absolute
paths; `Os.args` elements and `Os.env` values are taken as UTF-8 bytes without validation; goroutines run
synchronously and `Os.exit` from one is therefore trivially immediate until 2.6; the `for`-body frames and
`missing argument` ordering of 2.1.

## 3. Spec rules of this row that are easy to get wrong (each pinned by a test or a probe above)

- Init order (section 10): imports resolve depth first in source order; each module initializes once, after
  everything it imports; the entry module last; a module imported under several path spellings or by several
  importers is one module with one record (`mod-init-order-diamond`, `mod-relative-paths`, probe 01). Within a
  module `func` and `type` are hoisted (bound before any initializer runs); `var` initializers run in source
  order (`mod-init-var-order`).
- Reading a top-level `var` before its initializer ran panics `not initialized` (section 10, 12; through a
  call, through a destructured import, or through a namespace field); a name with no binding anywhere panics
  `unbound name` at the use site after the preceding statements ran (G3, `programs/`-style probe 03-p4). The
  checks are in `ant_module_get` and `ant_field_get` for `K_UNINIT`, `ant_global` for `K_UNBOUND`; neither
  sentinel may ever be stored into a value.
- Imports are live (section 10): a destructured name reads the exporter's field each time; a copy is a
  plain value; unchecked writes through a destructured name or a namespace field mutate the exporter's binding
  (G31, `mod-assign-namespace-rt`, `mod-live-var`).
- `export` is a checker rule: every field of a module record is readable at run time (section 12,
  `mod-private-readable-rt`), and a `type` export is its tag, identical under any local name, so `is` and
  record literals through aliases and namespaces agree (`mod-tag-identity`).
- Lookup order unchecked: block scopes outward → the module's own top-level names → import bindings → prelude
  (section 12 "Lookup walks outward ... to the module scope and then to the prelude"); the bootstrap agrees on
  every case probe 14 lists.
- `main` (section 10, G32, G53): execution starts at `func main()` of the entry module only (an imported
  module's `main` is an ordinary private function); a missing `main` (or a `var main`) is a load error at the
  entry module's last declaration, a `main` with parameters at its line; both before anything runs, exit 1,
  `path:line:col: message`. The return type is the checker's (M3). Exit status: 0 when `main` returns (even
  through `return` inside a loop), 2 after a panic (`panic: <message>` and the trace on stderr, stdout
  flushed first), `n` for `Os.exit(n)` masked to 8 bits.
- A top-level `return` panics `return outside a function`, a stray `break`/`continue` panics `break or
  continue outside a loop` (G24; the bootstrap's `init_vars` phrases).
- `Os.args()` holds only the arguments after the entry file (no program name, G39) and is a fresh array per
  call; `Os.env` is `null` when unset and `wrong kind` for a non-string; `Os.exit` takes an `int` only
  (`wrong kind` for a float, bool or sized int, and that panic unwinds normally), ends the process at once
  from any goroutine without running deferred blocks (G39), and flushes stdout first (the runtime buffers
  64 KB).
- Load errors (G32): `not found: <resolved path>` at the importing file and import line (col 1), including an
  import without the extension; `cycle: a.ant -> b.ant -> a.ant` (basenames) at the first import of the cycle
  in the first module of the cycle; a parse error at its own file and position; all exit 1 with nothing on
  stdout.
- A statement beginning with `{` is a lambda, not a block; a `for` body is a closure whose `return` targets the
  enclosing function (or the module initializer, which then panics); `defer` inside a block of an initializer
  runs at that block's exit.

## 4. Risks

- The sweep is the step: a probe that silently exercises the wrong thing (a `{` block that is a lambda, a
  panic that ends the probe before later lines, a relative path in a diagnostic) gives a false `SAME` or a
  spurious `DIFF`. Read every `.py.out` once; split panicking lines into separate programs.
- Diagnostics include absolute paths and the probe directory's path; the runner must pass the same absolute
  path to both commands, and the compared first stderr line must be the diagnostic itself (nothing precedes
  it).
- A fix in `emit.ant` or `modules.ant` changes every program's C; after any such change rerun the whole
  acceptance, not only `/modules/` (and `make runtime` after any C change: `antc build` links the archive
  it finds).
- Do not add import names to the module shape, re-export, or absolutize paths to mimic the bootstrap; the
  spec and the design settle those for the runtime (section 2.C). Do not touch the sync objects, goroutines
  (2.6), `Json`/file handles (2.7), or the stage targets (2.5).
- The acceptance takes about 3 minutes under antpy and the probe sweep a few more; run both in the background
  with logs and wait, never in the foreground.
- Scope: no test-name or output special case anywhere; `bootstrap/antpy/system.py` only for `Os.exec`, which
  needs nothing; no change to any test (none contradicts the spec).
