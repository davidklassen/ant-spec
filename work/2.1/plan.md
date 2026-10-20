# Plan for step 2.1 "rt-core" (re-plan after four red rounds)

Acceptance (verbatim): `RUN A0 RPU --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /functions/
--filter /expressions/` (125 tests, 126 checks). The technical design of the previous plan is kept: it is
`work/2.1/plan-v1.md` sections 1-3 and 5 (runtime API, emitter, spec rules), implemented as `work/2.1/report.md`
describes. This file replaces plan-v1 sections 0 and 4: it is the procedure for the remaining rounds.

## 0. What the re-plan found (do not re-investigate)

State of the tree, verified by the planner on 2026-10-21:
- `runtime/` (ant.h + 18 C files, ~3.6k lines), `src/antc/codegen/{literals,modules,emit,build}.ant`,
  `src/antc/main.ant` (`build`, `run`), `Makefile` (`runtime`, `clean`) implement the whole deliverable of the
  roadmap row following `docs/runtime-design.md`.
- The acceptance command exits 0 with `126 checks, 0 failures, 0 warnings (implementation mode, 125 tests)`
  (`work/2.1/test.log`); `--consistency` is clean too (`work/2.1/consistency.log`); the planner re-ran the
  `/smoke/` subset: green. Every `ant_call` in `runtime/*.c` is followed by an unwind check (design §6.1).
- The only red item in four rounds was the scope rule: `bootstrap/antpy/system.py` carries a 15-line
  uncommitted patch adding `Os.exec` (saved as `work/2.1/os-exec.patch`), outside `runtime/`, `src/antc/`,
  `Makefile`, `work/2.1/`. The tester must list every such path from `git status --short`, and the driver in
  `.claude/workflows/antc-roadmap.js` turns any listed path into a red round whatever the justification
  (`if (!test.pass || test.scopeViolations.length) ... continue`).

Why no in-scope design removes the patch (the evidence, so that nobody spends a round on it again):
- The acceptance runs `antc` inside `python3 -I bootstrap/antpy.py` (`src/tests/run.py` runs one fixed `sh -c`
  template per test, `cwd` = repo root). The program's output must come from that process tree, so the Python
  process must start clang and the built binary itself, or execute the program itself.
- The committed bootstrap exposes to Ant code only the stdlib namespaces: `File.{read,readBytes,write,
  writeBytes,exists,remove,list,open,create,readLine,writeLine,pipe,close}`, `Os.{args,env,exit}`, `Time`,
  `Json`, threads for `go`. No native creates a process (grep over `bootstrap/` for subprocess, popen,
  os.system, spawn, exec, fork, ctypes, importlib, eval finds nothing outside the patch). Under `-I` the
  interpreter imports only from `bootstrap/`, `tools/` and the standard library, so nothing placed under
  `runtime/`, `src/antc/` or in the `Makefile` can ever be loaded by it. Ant has no dynamic import and no
  reflection, so `antc` cannot even load the test module into the running antpy.
- Rejected alternatives: (a) an interpreter written in Ant under `src/antc/` so that `antc run` under antpy
  runs programs without clang: thousands of lines of throwaway code, an interpreter under an interpreter
  (timeouts), and the step's real deliverable would go unexercised by its acceptance, which the reviewer is told
  to flag as cheating; (b) a compile server started by `make` and reached through FIFOs: a process the acceptance
  cannot start, and antc executing something other than clang; (c) hiding the change from git
  (`assume-unchanged`, `.git/info/exclude`, stash): forbidden or deceptive, and the committed tree would stay
  broken for anyone else.
- Conclusion: the design is right and complete; the patch stays applied; the step turns green the moment the
  patch is in HEAD or the step's allowed paths include it. Only the user can do either. Reverting the patch is
  strictly worse: the round stays red (125 failures, `panic: no field: exec` from `Os.exec` under the committed
  bootstrap) and the user then has two things to do instead of one.

## 1. Files

- Unchanged in a blocked round: everything under `runtime/`, `src/antc/`, the `Makefile`, and the patched
  working copy of `bootstrap/antpy/system.py` (identical to `work/2.1/os-exec.patch`).
- Updated each round: `work/2.1/report.md` (the status section at its top, section 5 below), `work/2.1/test.log`
  when the full acceptance is run.
- Reference only: `work/2.1/plan-v1.md` (the previous plan; the reviewer compares the implementation with its
  sections 1-3 and 5 plus `report.md`'s "Decisions and deviations").

## 2. Every round starts here (literal)

A. Classify the state with two commands and one look at your task header:
```
git status --short
git show HEAD:bootstrap/antpy/system.py | grep -c '"exec"'
```
- GREEN-READY: the count is 1 or more and `git status` lists no `bootstrap/` path (the user committed the
  patch); or the "Allowed directories" line of your task names `bootstrap/` or `bootstrap/antpy/system.py` (the
  user widened the step; make sure the patch is applied, `grep -c '"exec"' bootstrap/antpy/system.py`). Go to
  section 3.
- BLOCKED (the state at the time of this plan): the count is 0 and `git status` shows
  `M bootstrap/antpy/system.py`. Go to section 4.
- REVERTED: the count is 0 and no `bootstrap/` path is listed. Re-apply with `git apply work/2.1/os-exec.patch`
  (if it no longer applies, re-create the 15 lines by hand from the patch: the `subprocess` import, the widened
  `antpy.values` import, `os_exec`, the `"exec"` entry of `OS`; nothing else under `bootstrap/`). Then go to
  section 4 and say in your summary that the patch was re-applied.

B. In every state run `make runtime` (a no-op when up to date). `runtime/libantrt.a` and `runtime/obj/` are
git-ignored and the tester's run depends on the archive being present: never leave the tree after `make clean`.

## 3. GREEN-READY round

1. Run the full acceptance in the background with output to `work/2.1/test.log`; wait for it.
2. Green: rewrite the top of `report.md` (section 5) to status green with the exact last line; drop the
   user-action text; keep the "Deviation: Os.exec in the bootstrap" section, the reviewer needs it. Return
   status `green`.
3. Not green (new: a toolchain change, a stale archive): fix inside the allowed directories only, following
   `plan-v1.md` and the design; no refactors (the refactor phase follows the review).
4. The review comes next. Points a reviewer may raise, and the answer already recorded in `report.md`:
   - `Os.exec` is not in `stdlib.md`: design §4 makes `antc` spawn clang and run the binary, so the runtime must
     offer the primitive to Ant code; the C native (`io.c`: `posix_spawnp` + `waitpid`, stdio inherited, stdout and
     stderr flushed first, `Ok(status)` / `Err`) and the bootstrap native have the same semantics. Note for
     step 3.1: the checker's prelude table must list it.
   - Stubs by design (design §5 order): `sched.c` runs `go` synchronously, `sync.c` panics `deadlock` where a
     wait would block forever, `ANT_SAFEPOINT()` is empty (it is already emitted at function entry and loop
     back-edges, so 2.6 only defines the macro).
   - `missing argument` is raised by `ant_call` before defaults run: this is design §4's wording ("fills a
     nparams vector with K_ABSENT, panics missing argument for an absent parameter without default, then
     invokes code").
   - clang `-w`: generated code has unused labels and temps by construction (`CHK` cascade, ANF temps).
   - `SY_<name>` symbol constants and `ant_closure(...)` instead of plan-v1's `S_`/`ant_func_new`: naming
     deviations, recorded.

## 4. BLOCKED round

Goal: cost nothing, change nothing, keep the one needed action in front of the user.
1. Do not edit `runtime/`, `src/antc/` or the `Makefile`. The tree is green; an edit now only risks the state
   the user resumes from, and the refactor phase handles cleanups once the step is green.
2. Run only the smoke subset (append `--filter /smoke/` alone to the acceptance's other options; about a
   minute) to confirm the toolchain still works. Do not rerun the full acceptance: the tester does, and
   `work/2.1/test.log` holds the last full run.
3. Make sure `report.md` starts with the status section of section 5 (edit only if it is out of date).
4. Return status `stuck` with this summary, verbatim: "Acceptance green (126 checks, 0 failures); blocked only by
   the uncommitted 15-line Os.exec patch in bootstrap/antpy/system.py, outside the step's directories; no agent
   can clear it. User: `git add bootstrap/antpy/system.py && git commit -m 'bootstrap: Os.exec'` (patch also at
   work/2.1/os-exec.patch), or add that file to step 2.1's dirs in .claude/workflows/antc-roadmap.js; then
   resume the workflow."

## 5. The status section of `report.md` (first section, at most 20 lines)

- Status line: acceptance green (the last line of `test.log`) / blocked by the scope rule, or green and
  unblocked.
- The user's options, in order of preference:
  1. `git add bootstrap/antpy/system.py && git commit -m "bootstrap: Os.exec"` (restore first with
     `git apply work/2.1/os-exec.patch` if the working copy was reverted); resume the workflow.
  2. Add `bootstrap/antpy/system.py` to step 2.1's `dirs` in the M2 table of `.claude/workflows/antc-roadmap.js`;
     the step's own commit (`git add -A`) then carries the patch.
  3. (Not recommended) change the contract instead: make `A0` in the workflow and ROADMAP a `build --emit-c`
     + clang + run pipeline (the runner executes its template through `sh -c`), which removes process spawning
     from antpy. It changes the `antc run` contract and the 2.2-2.4 rows; options 1 and 2 keep the roadmap as is.
- One sentence on why antpy cannot run clang otherwise (section 0, first two bullets of the evidence).
Then the existing sections (what was built, deviations, self-hosting signal, limitations) unchanged.

## 6. Spec rules the reviewer will check (all green today; `plan-v1.md` §5 has the full list)

- Operator check order: `kind mismatch` before `not a number`; `!`, `&&`, `||`, `if`, `while` operands are
  `not a bool`; integer ops overflow-checked at the operand width, `INT_MIN / -1` panics, `INT_MIN % -1` is 0.
- Evaluation order: callee, then arguments left to right (named ones in written order); pipe `a | f(b)` is `a`,
  `f`, `b`; assignment target sub-expressions once and before the right side; `not a function` before arguments.
- Scoping: `var x = x + 1` sees the outer `x`; nested bodies and `defer` see the hoisted binding; per-iteration
  `for` and `while`-body freshness through boxes allocated at block entry.
- `for` over a sequence: `break` makes `yield` return `false`, `continue` `true`, `return v` unwinds through the
  sequence running its defers; `yield after done` for a stashed yield; sequences re-runnable.
- `return` takes the whole expression to its right; `??`, `&&`, `||` never evaluate a skipped right side.
- Defaults evaluate per call, in parameter order, in the callee's scope; a lambda with no argument has `x == void`.
- `==` structural (records by tag and fields, arrays, maps), functions/tags/sync by identity.

## 7. Risks

- No agent action turns a round green while the patch is uncommitted; after `maxIter` rounds the workflow
  writes `work/2.1/BLOCKED.md` from the tester's feedback (which quotes `report.md`'s justification). Keep the
  status section accurate: it is what the user reads.
- A user who reverts the patch without widening the step or committing it gets 125 failures with
  `panic: no field: exec`; section 2's REVERTED state re-applies the patch and says so.
- The reviewer may question `Os.exec` as a stdlib extension; section 3.4 gives the design-based answer.
- `runtime/libantrt.a` is git-ignored: the tester's run needs it present and current (`make runtime` every
  round; never end a round with `make clean`).
- Step 2.5 note: `make stage1` should use `antc build --emit-c` plus clang from the Makefile, so the stage chain
  does not depend on antpy's `Os.exec` (only the 2.1-2.4 acceptance commands do).
