# Report for step 2.4 "rt-modules"

## Status

Acceptance (`work/2.4/test.log`, exit 0), last line of the runner's output:

    41 checks, 0 failures, 0 warnings (implementation mode, 40 tests)

Self-check with `--consistency` (`work/2.4/consistency.log`, exit 0; gate 2.G requires it), last line:

    41 checks, 0 failures, 0 warnings (implementation mode, 40 tests)

(`test.log` wraps the runner's last line in `START`/`EXIT_STATUS`/`END` lines, `consistency.log` appends
`exit=0`; both were rerun after the refactor pass.) The committed tree already ended `41 checks, 0 failures`
(`work/2.4/baseline.log`). The verification sweep changed nothing under `src/antc/`, `runtime/`, `Makefile`
or `bootstrap/`; the refactor pass (section "Refactor" below) changed one function in
`src/antc/codegen/emit.ant`, so the diff against HEAD is that file plus `work/2.4/`.

## What was built

Step 2.1 built the whole row (`work/2.4/plan.md` section 0 names where each piece lives): module sections
with one record per module and an `init_k` per module (imports first, depth first in source order, once;
tags and function closures stored before any initializer; `var` initializers in order; a top-level `return`
or stray `break`/`continue` turned into the G24 panics), live import bindings with the `K_UNINIT` and
`K_UNBOUND` checks, `main` lookup and the load-error diagnostics, and `Os.args`/`Os.env`/`Os.exit` in
`runtime/io.c`. This step is, like 2.2 and 2.3, the verification sweep of the row against the bootstrap
and the spec. It found no defect: 63 of 69 probes agree, and the 6 divergences are all settled for the
runtime by section 10 and G32 (section C).

### A. The probe runner (`work/2.4/probes/run.sh`)

`work/2.3/probes/run.sh` extended as the plan asks: a probe is a file `NN-name.ant` or a directory
`NN-name/` whose entry is `main.ant`; both commands get the same absolute path (the bootstrap prints
absolute paths in load errors, antc prints the path as given, so only equal inputs compare); every probe
runs with the arguments `alpha "b c" ""` and `ANT_PROBE=1` in the environment. The comparison is the one
of 2.3: stdout, `exit=<status>`, and the first stderr line. `sh work/2.4/probes/run.sh [NN ...]` from the
repository root; `*.out` and `*.err` are git-ignored next to the probes.

### B. The probe set (`work/2.4/probes/`, log `work/2.4/probes.log`)

69 programs in the 16 groups of the plan; panicking cases are separate `NN-pK-*` programs, and the flat
panic probes that need helper modules import them from the sibling probe directory of their group
(`05-p1-for-module.ant` imports `./05-namespace-values/m.ant`) instead of carrying copies. Beyond the
plan's list: `03-p6-self-init` (`var a = a`), an empty module and a module with only a `type` as namespace
values, `Record.same` on module records, `07-p4` (`return 7` from `main`, exit 0), `08-p7` (`continue` in
an initializer), `11-p5` (`Os.exit("3")`), `Array.same(Os.args(), Os.args())` (`false`: fresh arrays),
`12-j` and `12-k` (two load-order cases, section C), and `16-p0` (re-export through a destructuring import).

Everything the plan lists is `SAME`:

- Init order (`01`): the diamond, `d` imported again directly and twice in one file, one file reached as
  `./lib/x.ant`, `../lib/x.ant`, `./../lib/x.ant` and `../lib/../lib/x.ant`: each name printed once, in the
  order `d b c x app main`; the four namespace bindings of one file are `==` and `Record.same`.
- Hoisting (`02`): a function and a type used above their lines, mutual recursion, a hoisted body reading
  a later `var` once init is over (G61), a top-level closure assigning a top-level var, a top-level
  destructuring, and the `defer`s of `if`/`while`/`for` blocks inside an initializer running at each
  block's exit before the next initializer (`while-defer 1`, `while-defer 2`, `for-body 1`, `for-defer 1`,
  ..., `if-body-end`, `if-defer`, `after blocks`).
- `not initialized` (`03`): through a call, directly, through a destructuring initializer, `var a = a`;
  `unbound name: nope` for an import the exporter lacks, after `loaded` (G3); `no field: nope` through a
  namespace.
- Live imports (`04`): every write (`count = 5`, `Counter.count = 6`, `count += 1`, `Counter.count += 1`)
  is seen by the destructured name, the namespace field, the exporter's own function and a second
  importer; a copy keeps its value; functions taken from either binding mutate the same field.
- Namespace values (`05`): `Module`, `[Module]`, templates and `String.from`, `M == M`, `M == N` `false`
  (two modules with equal export names: functions and tags compare by identity), `E1 == E2` `true` (two
  modules holding only `var x = 1`), `M is Map` `false`, tags and functions read through the namespace,
  a namespace captured by a lambda, `not iterable` / `not a tag` for `for x in M`, `v is M.counter`,
  `M.counter{ }`.
- Tag identity (`06`): one tag under four names (namespace, destructured, aliased, through a function
  returning it), same-named tags of two modules unequal under `==` and `is`, `Record.same` and `==` on
  records built through different aliases, a union alias binding its own tag (G17).
- `main` (`07`): `export func main()`, an imported module's `main` ignored, `return` inside `while`
  (exit 0, `main`'s `defer` runs), a panic after output (`before`, `panic: boom`, exit 2), a panic in
  `main`'s deferred block, a dropped block value and a `return 7` (exit 0).
- Initializer failures (`08`): a panic in a dependency's initializer (`a`, `before`, exit 2, the entry's
  initializers and `main` never run), in the entry's initializer, `Os.exit(3)` with a pending block `defer`
  (`first`, exit 3, nothing deferred), `return` (directly and inside a `for` body) → `return outside a
  function`, `break` / `continue` → `break or continue outside a loop`.
- `Os.args` (`09`): `["alpha", "b c", ""]`, a fresh array per call, `==` across calls, `too many
  arguments`, `unknown parameter: x`.
- `Os.env` (`10`): the set variable, an unset one and `""` (`null`), `PATH`, the named argument,
  `wrong kind: expected a string`, `missing argument: name`.
- `Os.exit` (`11`): statuses 3, 0, 300 → 44, -1 → 255; no deferred block runs (G39) from `main`, a block,
  `Array.map`, a user sequence (the sequence's `defer` neither), `Mutex.withLock`, a deferred block, an
  `Iter.map` stage, a `go` block (exit 6); 2000 printed lines all reach stdout; `1.0`, `true`, `int8(1)`,
  `"3"` → `wrong kind: expected an int` with the pending `defer` running (a panic), `Os.exit()` →
  `missing argument: status`.
- Load diagnostics (`12`): `not found: <resolved path>` at the importing file and line, col 1, for a
  missing file and for a path without the extension; `cycle: a.ant -> b.ant -> a.ant`, the 3-cycle, the
  self-import `cycle: main.ant -> main.ant`, each at the first import of the cycle; `main: missing or not
  a function` at the last declaration (a multi-line `func` reports its first line; `var main = { x }` too);
  `main: takes no parameters` at its line; a parse error in an imported module at that file and position
  (`unexpected '='`); all exit 1 with empty stdout. The missing entry file, checked by hand: both print
  `<path>:0:1: not found: <path>` and exit 1 (with the path as given in both).
- Privacy (`13`), shadowing (`14`: `var Math = 1` at a module's top level wins inside it and `Math.PI`
  there is `not a record` while another module keeps the prelude; `import { f: print }` wins over the
  prelude; a local `var print`; the module's own `var x` over an imported `{ x }`; a parameter over a
  top-level var), stdout order across three initializing modules, `main` and a final panic (`15`).

### C. The six `DIFF`s, all kept (the spec sides with the runtime)

1. `12-j-late-missing-after-init`: `main.ant` imports `./a.ant` (whose initializer prints `a`) and then
   `./nope.ant`. The bootstrap prints `a` and then the `not found` diagnostic; antc prints the same
   diagnostic with nothing on stdout. The bootstrap initializes each module as soon as it is loaded, so
   an earlier import runs before a later one fails to resolve. G32 reads a missing import as "reported
   before running" and section 10 makes it a compile error; antc loads the whole graph before anything
   runs. Kept.
2. `12-k-no-main-bad-import`: an entry without `main` that imports a missing file. The bootstrap reports
   `main: missing or not a function` (line 3), antc `not found` (line 1): both exit 1 before anything
   runs, and differ only in which of two load errors comes first. The spec orders nothing here; the
   bootstrap checks `main` before loading imports because of its interleaved loading (section C.1), antc
   reports the first error the graph meets. Kept.
3. `16-reexport-through-namespace` (`A.B.w`) and `16-p0-reexport-destructure` (`import "./a.ant" { B }`
   where `B` is `a.ant`'s own namespace import): the bootstrap yields `2`, the runtime panics `no field:
   B` / `unbound name: B`. Section 10: "Its top-level scope is a record whose fields are its declarations"
   and "There is no re-export"; `docs/runtime-design.md` section 1: the shape lists every top-level
   `var`, `func`, `type`. The bootstrap keeps namespace imports in the same dictionary as the module's
   fields, an artefact. Kept, as the plan decided.
4. `16-p1-assign-namespace-name` (`A = 1`) and `16-p2-assign-prelude-name` (`Math = 1`): the bootstrap
   rebinds (the prelude one in the shared prelude scope), the runtime panics `cannot assign` (the
   namespace is bound but no target; before the refactor pass it said `unbound name: A`) /
   `unbound name: Math` (at emit time a prelude name cannot be told from an unbound one without a
   runtime `ant_global` probe). Section 10 makes both a compile error ("assigning to an imported name ... is a
   compile error"; the prelude names "count as imports"); the unchecked outcome is unspecified and no test
   pins it. Kept, as the plan decided.

## Decisions and deviations

- The sweep changed no code. The plan expected none, and the sweep confirmed it: every divergence is one
  the spec settles for the runtime, so `runtime/`, `src/antc/`, `Makefile` and `bootstrap/antpy/system.py`
  were untouched by it (`make runtime` was a no-op on the committed archive). The refactor pass then
  changed the panic one divergence produces (section "Refactor").
- The flat panic probes share the helper modules of their group's directory rather than carrying their
  own copies (plan 2.A said helpers stay inside the probe's directory); the imports still resolve
  relative to the importing file, which is the rule under test.
- In `02-hoisting` the `while` and `for` with their `defer`s sit inside an `if` that is the initializer
  expression, since a loop is a statement, not an initializer; the `defer`s still run inside the module
  initializer's frame, which is what the probe checks.
- Procedure: the acceptance and the `--consistency` run were started while the sweep was still running
  (as in 2.2 and 2.3; `antc build` names its temporary files by clock and hash). Since no code changed
  afterwards, those runs are the final ones.
- The refactor pass is recorded in the section "Refactor" at the end.

## Test changes

None. No test of the selected tiers contradicts the spec; `work/TEST-CHANGES.md` is unchanged.

## Known limitations (carried forward)

- `antc run` spawns the binary and waits instead of `execv` (`docs/runtime-design.md` section 4 wording);
  under antpy there is no exec, and the exit status is relayed (128 + signal when the program was killed).
- Load errors print the entry path as typed plus the resolved import paths; the bootstrap absolutizes the
  entry. The runner passes absolute paths, so the suite never sees the difference.
- `Os.args` elements and `Os.env` values are taken as UTF-8 bytes without validation.
- Goroutines run synchronously until step 2.6, so `Os.exit` from a `go` block is trivially immediate.
- The `for`-body frames in traces and the `missing argument` ordering recorded in `work/2.1/report.md`.
- The bootstrap's load-time artefacts of section C.1 and C.2 (initializers running before a later load
  error; `main` checked before imports) and its unchecked rebinding of imports and prelude names
  (C.3, C.4): `bootstrap/` is closed to this step; for step 4.1 if they are to be aligned.

## Refactor

- `simplify` pass (single-pass, inline; the Agent fan-out was unavailable): the step's uncommitted tree
  held only `work/2.4/` (probes, logs, the runner, this report) and no implementation code, so there was
  nothing to reuse, shorten or move. `work/2.4/probes/run.sh` keeps the per-step copy convention of 2.2
  and 2.3 and was left alone.
- Reviewer 1, `src/antc/codegen/emit.ant` `storeName`: an assignment to a namespace import (`A = 1`,
  binding kind `namespace`) fell through to `unbound()` and panicked `unbound name: A`, but README defines
  `unbound name` as a name with no binding (G3) and `A` is bound. It now emits `ant_panic("cannot assign");
  goto <exit>`, the phrase the emitter already used for non-target expressions in `emitAssign`. That line
  moved into a helper `cannotAssign(fn)` next to `unbound`, and both sites call it. The no-binding path
  stays `unbound name`: at emit time a prelude name cannot be told from an unbound one without a runtime
  `ant_global` probe, so `Math = 1` still panics `unbound name: Math`. No test pins either outcome
  (`unchecked/modules/mod-assign-namespace-rt` writes a namespace *field*, the `Ast.Field` path). Probe
  group 16 was rerun: `16-p1` now diverges with `panic: cannot assign` (`probes.log`, section C.4 and the
  probe's comment updated); the other three `16-*` diffs are unchanged.
- Reviewer 2, this report: the probe tally said 68; `probes.log` has 69 `SAME`/`DIFF` lines and the
  directory 69 sources (138 `.out` files): 63 SAME + 6 DIFF = 69, corrected in two places.
- Reruns after the emitter change: the `/smoke/` + `/modules/` subset (`11 checks, 0 failures`), then the
  full acceptance (`work/2.4/test.log`: `41 checks, 0 failures, 0 warnings`, exit 0) and the
  `--consistency` run (`work/2.4/consistency.log`: same line, exit 0).
