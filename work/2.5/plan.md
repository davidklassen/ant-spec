# Plan for step 2.5 "self-host"

Row: `make stage1`, `make stage2`, `make fixpoint`; `bin/antc` native. Allowed: `runtime/`, `src/antc/`,
`Makefile`, `bootstrap/antpy/system.py` (`Os.exec` only), `work/2.5/`.

Acceptance (verbatim):

    make fixpoint && python3 -I src/tests/run.py --run "bin/antc run {file} {args}" --run-unchecked "bin/antc run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --tier programs --exclude /concurrency/ --exclude race-memory-safe --exclude parallel-sum --exclude producer-consumer

It selects 395 tests (396 checks): run 244 (all sections but concurrency), panic 54, unchecked 64, programs 30.

## 0. What the planner verified on the committed tree (nothing in the repo was changed)

Everything below ran from the repository root with binaries placed in the scratchpad, not in `bin/`.

1. `make runtime` is clean. `python3 -I bootstrap/antpy.py run src/antc/main.ant build src/antc/main.ant -o <stage1>`
   takes 5.7 s wall (antpy) plus clang; the binary is 736 KB.
2. The C that antpy's antc emits for `src/antc/main.ant` (`build --emit-c`, 1,038,056 bytes) is byte-identical to the
   C the native stage1 emits for the same path. Stage1 emits it in 0.1 s.
3. Stage1 builds stage2 (`<stage1> build src/antc/main.ant -o <stage2>`) in 2.4 s wall, almost all of it clang on
   the 1 MB C file. Stage2 builds stage3 the same way, and **`stage2 --emit-c` and `stage3 --emit-c` are
   byte-identical**: the fixpoint already holds on the committed emitter and runtime. The two *binaries* differ
   (clang/ld metadata such as the Mach-O UUID): the roadmap compares the emitted C, never the binaries.
4. The emitted C embeds every module path exactly as the entry path was typed (`AntFuncInfo.file`, e.g.
   `"src/antc/lexer.ant"` vs `"/Users/.../src/antc/lexer.ant"`). Both emissions of the fixpoint check must
   therefore pass the identical path string; the Makefile uses one variable for it.
5. A prototype Makefile with the rules of section 2 (run with `make -f`, output directory overridden) does a cold
   `fixpoint` in 11.5 s with the runtime already built, is a no-op on a second run, and after deleting stage2
   rebuilds exactly stage2, stage2.c, stage3, stage3.c and `antc`. The system `make` is GNU Make 3.81.
6. Stage1 runs `run/smoke/print-hello.ant` (0.5 s wall, mostly clang + link) and `programs/prog-wordcount`
   correctly and parses its own `parser.ant`.
7. The full acceptance suite (the exact tiers and excludes above) driven by the native stage2 binary in place of
   `bin/antc`: `396 checks, 0 failures, 0 warnings (implementation mode, 395 tests)`, exit 0, 3 min 56 s wall.
   The `dynamic` section, which no earlier step ran, is among them.
8. `src/tests/run.py` runs every command with `cwd` = the repository root and the inherited environment plus
   the test's `env:` pairs, so `bin/antc` (a relative path) finds `runtime/libantrt.a`, `runtime/libantrt.flags`,
   `clang` on `PATH` and `TMPDIR` the same way it does by hand.

Consequence: the step is the Makefile plus verification. No change to `runtime/`, `src/antc/` or
`bootstrap/antpy/system.py` is expected; section 4 says what to do if the suite disagrees.

## 1. Files

| File | Change |
|---|---|
| `Makefile` | add `stage1`, `stage2`, `fixpoint`, the `bin/` rules, a content-stable `runtime/libantrt.flags` rule; extend `clean` |
| `work/2.5/test.log` | the acceptance command's output (run in the background, see section 3) |
| `work/2.5/consistency.log` | the same command with `--consistency` (the 2.G gate needs it; cheap to know now) |
| `work/2.5/report.md` | status, timings, the fixpoint evidence, decisions |

Do not touch `bin/` by hand (git-ignored; the Makefile owns it), `bootstrap/` (`Os.exec` already exists in
`system.py` and in `runtime/io.c`), or any test. `make test` is step 4.1's deliverable; do not add it here.

## 2. The Makefile

Replace the whole file with the following (the runtime part is the committed one; only the flags rule changes).
Keep every recipe's path relative to the repository root: `antc build` resolves `runtime/` against the current
directory (or `$ANT_RUNTIME`).

```make
# antc build (ROADMAP.md): `make runtime` builds the C runtime archive that `antc build` links
# and writes the flags it links with next to it; `make stage1` builds antc under the bootstrap,
# `make stage2` rebuilds it with stage1, `make fixpoint` checks that stage2 and stage3 emit the
# same C for antc's own source and installs stage2 as bin/antc. GC_PREFIX is where Homebrew put bdw-gc.
ifeq ($(origin CC),default)
CC = clang
endif
GC_PREFIX ?= /opt/homebrew
PYTHON ?= python3
CFLAGS = -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I$(GC_PREFIX)/include
RT_LDFLAGS = -L$(GC_PREFIX)/lib -lgc -lpthread

RT_SRC = $(wildcard runtime/*.c)
RT_OBJ = $(patsubst runtime/%.c,runtime/obj/%.o,$(RT_SRC))
ANTC_SRC = $(wildcard src/antc/*.ant src/antc/codegen/*.ant)
BOOTSTRAP_SRC = bootstrap/antpy.py $(wildcard bootstrap/antpy/*.py) tools/antparse.py
# One spelling for the entry: the emitted C embeds module paths as typed, and the fixpoint compares bytes.
ANTC_ENTRY = src/antc/main.ant
export ANT_CC = $(CC)

.PHONY: runtime stage1 stage2 fixpoint clean FORCE

runtime: runtime/libantrt.a runtime/libantrt.flags

runtime/libantrt.a: $(RT_OBJ)
	ar rcs $@ $^

runtime/obj/%.o: runtime/%.c runtime/ant.h | runtime/obj
	$(CC) $(CFLAGS) -c $< -o $@

runtime/obj:
	mkdir -p $@

# Rewritten only when its content changes: a changed GC_PREFIX reaches `antc build`,
# and an unchanged one does not rebuild the stages.
runtime/libantrt.flags: FORCE
	@echo "$(RT_LDFLAGS)" | cmp -s - $@ 2>/dev/null || echo "$(RT_LDFLAGS)" > $@

FORCE:

stage1: bin/antc-stage1
stage2: bin/antc-stage2
fixpoint: bin/antc

bin:
	mkdir -p $@

# Stage 1: antc under the bootstrap interpreter compiles antc.
bin/antc-stage1: $(ANTC_SRC) $(BOOTSTRAP_SRC) runtime/libantrt.a runtime/libantrt.flags | bin
	$(PYTHON) -I bootstrap/antpy.py run $(ANTC_ENTRY) build $(ANTC_ENTRY) -o $@

# Stage 2: the native stage1 compiles antc.
bin/antc-stage2: bin/antc-stage1 $(ANTC_SRC) runtime/libantrt.a runtime/libantrt.flags
	bin/antc-stage1 build $(ANTC_ENTRY) -o $@

# Stage 3 and the fixpoint: stage2 compiles antc; the C stage2 and stage3 emit for antc must be identical.
bin/antc-stage3: bin/antc-stage2
	bin/antc-stage2 build $(ANTC_ENTRY) -o $@

bin/antc-stage2.c: bin/antc-stage2
	bin/antc-stage2 build --emit-c $(ANTC_ENTRY) -o $@

bin/antc-stage3.c: bin/antc-stage3
	bin/antc-stage3 build --emit-c $(ANTC_ENTRY) -o $@

bin/antc: bin/antc-stage2 bin/antc-stage2.c bin/antc-stage3.c
	cmp bin/antc-stage2.c bin/antc-stage3.c
	cp bin/antc-stage2 $@

clean:
	rm -rf runtime/obj runtime/libantrt.a runtime/libantrt.flags bin
```

Why each decision:

- The stage rules depend on the archive and flags **files**, never on the phony `runtime` target: a phony
  prerequisite is always out of date and would rebuild the stages (about 12 s) on every `make fixpoint`, which
  the acceptance command and every later step run first.
- `runtime/libantrt.flags` uses the move-if-changed idiom (`FORCE` + `cmp`) so that it keeps the committed
  property "a changed `GC_PREFIX` reaches `antc build`" without touching the file's mtime when nothing changed.
  Verified on GNU Make 3.81 (second `make fixpoint` is a no-op).
- `bin/antc` is a copy of **stage2** (ROADMAP: "copies stage2 to `bin/antc`"), made only after `cmp` succeeded;
  a mismatch leaves no `bin/antc` behind and fails the acceptance at `make`.
- `bin/antc-stage2` lists `$(ANTC_SRC)` and the runtime files besides stage1 so that make's intent is explicit;
  stage3 and the two `.c` files follow from stage2 alone.
- `export ANT_CC = $(CC)` makes the stages' `antc build` use the compiler the runtime was built with. Under the
  runner (not via make) `antc` falls back to `clang`, which is the same compiler.
- `PYTHON ?= python3` with `-I`, as the command-line contract spells it.
- `bin` is an order-only prerequisite (`| bin`): its mtime changes whenever a file lands in it.

## 3. Order of work

1. Write the Makefile. `rm -rf bin` (and nothing else). `make fixpoint` from the repository root: expect
   `mkdir -p bin`, the stage1 recipe (about 8 s), stage2 (2.4 s), stage2.c, stage3, stage3.c, `cmp`, `cp`;
   about 15 s if the runtime is already built, about 20 s after `make clean`. Then `make fixpoint` again: no
   output. `bin/antc run src/tests/run/smoke/print-hello.ant` prints `hello`.
2. Run the acceptance command with its output redirected to `work/2.5/test.log`, in the background (395 tests
   at roughly 0.6 s each: 4 to 5 minutes; the runner prints its report at the end, so the log stays empty until
   then). It must exit 0 and end with `... 0 failures ...`.
3. Run the same command with `--consistency` to `work/2.5/consistency.log` (not required by this row; 2.G
   needs it and it costs one more run). If it fails, record it in the report; do not block the step on it.
4. Only if step 2 shows failures: section 4. After any change under `runtime/` or `src/antc/`, run
   `make fixpoint` again (the chain rebuilds from the changed file) and rerun the suite.
5. `work/2.5/report.md`: the acceptance's last line, the timings of the three stage builds, the byte sizes and
   the `cmp` of `bin/antc-stage2.c`/`bin/antc-stage3.c`, that the binaries differ and why that is irrelevant,
   decisions (section 2's list), and the limitations carried forward from the 2.1 to 2.4 reports that still
   hold (synchronous goroutines, `deadlock` panics from blocking sync calls, `antc run` spawns and waits instead
   of `execv`, `for`-body frames in traces, `missing argument` before defaults).

## 4. If the suite is not green under the native compiler

Baseline: the planner ran the acceptance suite with a native stage2 built from the committed tree and it was
green (`396 checks, 0 failures`, section 0 item 7), so the implementer should see the same once `bin/antc` is
in place. The notes below exist for the case where a change made during this step, or a difference in the
environment, breaks it; they also tell a reviewer where to look.

Where a native-only failure could come from, given that the native antc's emitter is the same Ant code that
passed sections 2.1 to 2.4 under antpy and emits byte-identical C:

- **The `dynamic` section** (run 3, panic 3, unchecked 7) was in no earlier step's filter, so no antc, native
  or under antpy, has run it before. Its tests exercise features that other sections pin: a local `var print`
  shadowing the prelude inside an `if` block, outward lookup through two closures, kinds surviving calls,
  fields, arrays and `int8 + int8`, reference semantics of arrays, maps and records, `Iter.range` with `*=`
  overflow after the first print, `not initialized` through a call before `b`'s initializer, `not a record` for
  `.length` on an array, a string, a function and `Point.x` (G54: a tag is not a record), `not an array` for
  `[...Map{}]`, `not a tag` for `Math{}`, and the never-row program (`1 ?? 2` is `1`, `try 1` is `1`,
  `1 == "a"` false, `int8(1) == 1` false, `null != void` true). A failure here is a runtime or emitter bug to fix
  in `runtime/` or `src/antc/codegen/emit.ant`, never a test change.
- **Timing**: the default per-test timeout is 60 s; a native compile-and-run of a test is about 0.5 s, so a
  timeout means a hang (look for a blocking sync call, which the 2.1 stubs turn into `deadlock` panics, or an
  infinite loop in the program under a semantic difference).
- **Temporary files**: `build.ant`'s `tempPath` names files by `Time.now()`, `Time.monotonic()` and a hash of
  the entry; the runner is sequential, so collisions cannot occur. Nothing in `/tmp` needs cleaning.
- **Panics in the compiler itself** (the compiler is now a dynamic Ant program on the C runtime): a message
  `panic: ...` with a trace naming `src/antc/...` files, exit status 2 from `bin/antc` rather than from the test
  program. Reproduce with `bin/antc build --emit-c <test> -o /dev/stdout`; compare with
  `python3 -I bootstrap/antpy.py run src/antc/main.ant build --emit-c <test> -o /dev/stdout` to tell a runtime
  defect (only the native compiler fails) from an emitter defect (both fail).

## 5. Rules that are easy to get wrong

- The fixpoint compares the **C**, byte for byte; the two binaries always differ (Mach-O UUID). Do not "fix"
  that, and do not compare them.
- Both `--emit-c` invocations must receive the **same path string** (`$(ANTC_ENTRY)`, relative, from the
  repository root); an absolute path changes every `AntFuncInfo` file string.
- `bin/antc` is stage2, not stage3 (ROADMAP's `fixpoint` definition). `bin/antc` must not exist when `cmp`
  fails (the recipe order above guarantees it).
- `make fixpoint` must work on a clean checkout: `bin/` created by make, the runtime built by the dependency
  on `runtime/libantrt.a`, the flags file by its rule. `bin/` is git-ignored (`.gitignore` has `bin/`); keep it
  so. Do not commit.
- The stage1 recipe runs `python3 -I bootstrap/antpy.py run src/antc/main.ant build ...`: `run` takes the
  entry (antc's `main.ant`) and the rest are antc's arguments.
- The acceptance runner invokes `bin/antc run <abs file> <args>` from the repository root with the inherited
  environment: no `ANT_CC`, no `ANT_RUNTIME`; the defaults (`clang`, `runtime`) must keep working. The
  Makefile's `export ANT_CC` only affects recipes.
- The runtime and the emitter are not to be "optimized" for the compiler's benefit in this step: the
  self-compile takes 0.1 s of antc plus 2.2 s of clang; the stated goal of 2.5 is the stage chain.
- A test is changed only if its expectation contradicts the spec, with a citation and a line in
  `work/TEST-CHANGES.md`; nothing in the planner's run suggests one.

## 6. Risks

1. The native suite run turns up a runtime or emitter defect no earlier section pinned (the `dynamic` section
   is the one place with no history). Mitigation: section 4; the fix belongs in `runtime/` or `src/antc/`.
2. Build-system drift: a later step touching `runtime/*.c` must trigger stage rebuilds. The chain does this
   through `runtime/libantrt.a`; a future `runtime/*.h` beyond `ant.h` would need adding to the object rule.
3. `GNU Make 3.81` idioms only: the Makefile uses `$(wildcard)`, `$(patsubst)`, order-only prerequisites and
   `export VAR = value`, all supported there; no `.SECONDEXPANSION`, no `$(file ...)`.
4. The flags rule runs `cmp` on every `make`; harmless, and it prints nothing.
5. The step's acceptance takes 4 to 5 minutes plus `make fixpoint`; run it in the background with the log in
   `work/2.5/test.log` as the task statement requires.
