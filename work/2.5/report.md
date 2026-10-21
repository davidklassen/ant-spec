# Report for step 2.5 "self-host"

Status: **green**. The acceptance command exits 0; its last line is

    396 checks, 0 failures, 0 warnings (implementation mode, 395 tests)

(`work/2.5/test.log`; 237.8 s wall, 58.0 s user, 18.8 s sys for `make fixpoint` plus the suite: run 244,
panic 54, unchecked 64, programs 30, including the `dynamic` section that no earlier step had run.)

## What was built

Only `Makefile` changed (53 insertions, 8 deletions). `runtime/`, `src/antc/` and `bootstrap/antpy/system.py`
are untouched: the committed emitter and runtime already reach the fixpoint (section "Evidence" below), so the
step is the stage chain the roadmap asks for.

The Makefile now has, besides the committed `runtime` rules:

| Target | Rule |
|---|---|
| `make runtime` | `runtime/libantrt.a` from `runtime/obj/*.o`, and `runtime/libantrt.flags` |
| `make stage1` | `bin/antc-stage1`: `python3 -I bootstrap/antpy.py run src/antc/main.ant build src/antc/main.ant -o bin/antc-stage1`; depends on every `src/antc/**/*.ant`, the bootstrap sources, the archive and the flags file |
| `make stage2` | `bin/antc-stage2`: `bin/antc-stage1 build src/antc/main.ant -o bin/antc-stage2` |
| `make fixpoint` | `bin/antc-stage3` by stage2, `bin/antc-stage2.c` and `bin/antc-stage3.c` by `build --emit-c` of stage2 and stage3, `cmp` of the two C files, then `cp bin/antc-stage2 bin/antc` |
| `make clean` | removes `runtime/obj`, the archive, the flags file and `bin/` |

Details that matter:

- The stage rules depend on the archive and flags **files**, not on the phony `runtime` target, so a second
  `make fixpoint` is a no-op (a phony prerequisite would rebuild the stages on every run, and every later
  step's acceptance starts with `make fixpoint`).
- `runtime/libantrt.flags` is rewritten only when its content changes (`FORCE` + `cmp -s`): a changed
  `GC_PREFIX` still reaches `antc build`, an unchanged one does not touch the file's mtime.
- One variable `ANTC_ENTRY = src/antc/main.ant` names the entry in every recipe: the emitted C embeds module
  paths exactly as the entry was typed (`AntFuncInfo.file`), and the fixpoint compares bytes.
- `bin/antc` is stage2 (ROADMAP: "copies stage2 to `bin/antc`"), copied only after `cmp` succeeded.
- `export ANT_CC = $(CC)`: the stages link with the compiler the runtime was built with. Under the test runner
  (no `ANT_CC`) `antc` defaults to `clang`, which on this machine is the same binary
  (`/opt/homebrew/opt/llvm/bin/clang` on `PATH`).
- `PYTHON ?= python3`, run with `-I` as the command-line contract spells it; `bin` is an order-only
  prerequisite.

## Evidence for the fixpoint

Measured on this tree (`bin/` built by `make fixpoint` from the repository root):

| Step | Wall time | Notes |
|---|---|---|
| antc under antpy emits the C for `src/antc/main.ant` | 3.6 s | 1,038,056 bytes |
| stage1 = that C through clang (`-O1`) | 5.7 s total | binary 736,472 bytes |
| stage1 builds stage2 | 3.0 s | 0.06 s of antc, the rest clang on the 1 MB C file |
| stage2 builds stage3 | 3.0 s | |
| cold `make fixpoint`, runtime already built | 11.9 s | `work/2.5/fixpoint-cold.log` |
| `make fixpoint` from a clean checkout (runtime from scratch, 19 objects) | 13.5 s | done in an isolated copy made with `git archive`, not in the repository |

- The C emitted for `src/antc/main.ant` by antc under antpy, by stage1, by stage2 and by stage3 is
  **byte-identical** (four `cmp`s, 1,038,056 bytes each). `cmp bin/antc-stage2.c bin/antc-stage3.c` is the
  `make fixpoint` check.
- The *binaries* `bin/antc-stage2` and `bin/antc-stage3` differ (`cmp`: char 1369, line 6, inside the Mach-O
  load commands, where the linker writes a per-link UUID). The roadmap defines the fixpoint over the emitted C,
  never over the binaries, so this is irrelevant and is not "fixed".

Makefile behaviour verified (in the repository where harmless, otherwise in the isolated copy):

- second `make fixpoint`: no output, exit 0; `make stage1` / `make stage2` afterwards: no output;
- `rm bin/antc-stage2; make fixpoint` rebuilds exactly stage2, stage2.c, stage3, stage3.c and `bin/antc`;
- `touch runtime/value.c; make fixpoint` recompiles the object, re-archives and rebuilds the whole stage chain;
- `make -n fixpoint GC_PREFIX=/opt/other` would rewrite the flags file and rebuild from stage1;
- a simulated mismatch (a line appended to `bin/antc-stage3.c`) makes `make fixpoint` fail at `cmp` with
  exit 2 and **no** `bin/antc` left behind; removing the drifted file repairs it;
- `bin/antc run src/tests/run/smoke/print-hello.ant` prints `hello`, exit 0.

## Decisions and deviations from the plan

The Makefile is the plan's section 2 with two small additions, both safety nets:

1. `.DELETE_ON_ERROR:` so a recipe that fails (clang killed, disk full) leaves no half-written target that a
   later `make` would take as up to date. Supported by GNU Make 3.81, the system `make`.
2. `rm -f $@` before the `cmp` in the `bin/antc` rule. The plan says a mismatch "leaves no `bin/antc`
   behind"; that was only true when none existed before. With the `rm -f`, a stale compiler from an earlier
   successful run cannot survive a later mismatch, so `bin/antc` exists exactly when the fixpoint held for the
   current sources.

Noted, not changed: `make -n fixpoint` prints the whole chain even when a real run is a no-op. Dry-run mode
"runs" the `FORCE`d flags recipe and then assumes the flags file changed. This is a known artefact of the
move-if-changed idiom; the real run is a no-op, which is what the acceptance commands care about.

Not done on purpose: `make test` is step 4.1's deliverable (ROADMAP layout table); no runtime or emitter
"optimisation" for the compiler's own benefit (the self-compile is 0.06 s of antc plus clang).

## Consistency run (not required by this row)

The 2.G gate runs the suite with `--consistency` (run and panic tests must behave the same when run with
`--unchecked`). Result on this tree, `work/2.5/consistency.log`: `396 checks, 0 failures, 0 warnings (implementation mode, 395 tests)`,
exit 0, 423.0 s wall (each run and panic test is executed twice, checked and `--unchecked`).

## Test changes

None. `work/TEST-CHANGES.md` is unchanged.

## Known limitations (carried forward)

- Goroutines run synchronously and any sync call that would block panics `deadlock`: step 2.6 (the
  concurrency section and the three concurrent programs are excluded from this row's acceptance).
- `File` handles, `Json` and the rest of `Time`: step 2.7.
- `antc run` spawns the built binary and waits instead of `execv` (`docs/runtime-design.md` section 4
  wording); the exit status is relayed, 128 + signal when the program was killed.
- Stack traces show the enclosing function's frame for code in a `for` body; `ant_call` reports
  `missing argument` before any default evaluates (2.1 decisions).
- Every Makefile recipe uses paths relative to the repository root, and the emitted C embeds the entry path as
  typed, so `make fixpoint` must run from the repository root (as ROADMAP prescribes for every command).
- The bootstrap-side items listed in the 2.2 to 2.4 reports (`float32` near `FLT_MAX`, `map key too deep`,
  load-time artefacts) are for step 4.1; `bootstrap/` was not open to this step beyond `Os.exec`, which
  needed no change.

## Refactor

Done after the step was green; only `Makefile` changed, and the acceptance command was rerun at the end:
`396 checks, 0 failures, 0 warnings (implementation mode, 395 tests)`, exit 0, 240.3 s wall (`work/2.5/test.log`).
Subsets run after each change were green too (`/smoke/ /modules/ /dynamic/ programs/`: 54 checks;
`/types/ /errors/ /stdlib/`: 205 checks).

1. Simplify pass (single pass, no agent fan-out; reuse, simplification, efficiency, altitude), no behaviour change:
   - `bin/antc-stage2` depends on `bin/antc-stage1` only. The sources, archive and flags file it also listed are
     all prerequisites of stage1, so stage1 is newer whenever any of them changes; the list was derivable and
     made stage2 asymmetric with stage3.
   - The stage2 and stage3 recipes use `$<`, and one pattern rule `bin/%.c: bin/%` replaces the two
     copy-paste `--emit-c` rules, matching the file's existing `runtime/obj/%.o` style.
   - Verified with real runs (a dry run is uninformative here, see below): second `make fixpoint` is a no-op;
     `rm bin/antc-stage2; make fixpoint` rebuilds exactly stage2, stage2.c, stage3, stage3.c, `bin/antc`;
     `touch src/antc/codegen/emit.ant; make fixpoint` rebuilds from stage1; `make -j4 fixpoint` from a removed
     stage2 reaches the fixpoint.
2. Reviewer suggestion: `ANTC_SRC = $(wildcard src/antc/*.ant src/antc/*/*.ant)`, so the M3 checker in
   `src/antc/check/*.ant` (a directory M3 cannot add to the Makefile itself) rebuilds the stages. Verified with
   a scratch `src/antc/check/x.ant` and `make -n -o runtime/libantrt.flags fixpoint` (`-o` stops the `FORCE`d
   flags rule from making every dry run print the whole chain): the new wildcard schedules stage1 onward, the
   old one passed on the command line reports nothing to do. Scratch file and directory removed afterwards.
3. Reviewer note on `make -j`: no rule changed; one comment above the stage rules records that `--emit-c`
   writes only its output and that `build.ant`'s `tempPath` keys on the clocks and the entry, never the
   output path, for whoever later adds a target that builds the same entry concurrently.
