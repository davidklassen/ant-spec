# Step 2.1 "rt-core": report

## Status: accepted (green)

- The full acceptance command ends with `126 checks, 0 failures, 0 warnings (implementation mode, 125 tests)`
  (`work/2.1/test.log`), and `--consistency` is clean (`work/2.1/consistency.log`).
- `Os.exec` in `bootstrap/antpy/system.py` is an accepted antc-internal native: the M2 introduction of
  ROADMAP.md specifies it (`Os.exec(args: Array[string]): Result[int, Error]`, in the bootstrap and in the C
  runtime, "M2 steps may change `bootstrap/antpy/system.py` for this native only"), and the step's row lists
  `bootstrap/antpy/system.py` (`Os.exec` only) among its directories. It is part of the step's change set; there
  is no `work/2.1/os-exec.patch` any more.
- Why antpy needs it: the acceptance runs `antc` inside `python3 -I bootstrap/antpy.py`, whose other natives
  (`File`, `Os.{args,env,exit}`, `Time`, `Json`, threads) spawn no process, and which under `-I` imports only
  from `bootstrap/`, `tools/` and the standard library, so nothing under `runtime/` or `src/antc/` could run
  clang. The roadmap presumes it: M2 says "`antc build` emits C and calls clang" and "Steps 2.1 to 2.4 run
  `antc` under antpy".

## Os.exec in the bootstrap

`antc run` must emit C, run clang and run the built binary, and the acceptance command runs `antc` under
antpy. `stdlib.md`'s `Os` has only `args`, `env` and `exit`, and the committed bootstrap had no process-spawning
native of any kind (`git grep` of `HEAD -- bootstrap/` for subprocess/popen/system/spawn/exec finds nothing;
`File` only reads, writes, lists and copies files), so no Ant code under antpy could execute clang. As the plan
(section 0) and ROADMAP.md's M2 introduction direct, one native was added to `bootstrap/antpy/system.py`, the
only file touched outside `runtime/`, `src/antc/`, `Makefile` and `work/2.1/`: `Os.exec(args: Array[string]):
Result[int, Error]` runs `args[0]` (looked up on `PATH`) with the remaining elements as arguments, stdio
inherited, flushes this process's stdout and stderr first, waits, and returns `Ok(status)` (`128 + signal` when
the child was killed by a signal) or `Err{ error: Error{ message } }` when it cannot be started (`Err("no
program")` for an empty list, as in the C runtime); `wrong kind` for a non-array or a non-string element. The C
runtime has the same native (`runtime/io.c`, `posix_spawnp`), which is what makes the native `antc` work.
Nothing else under `bootstrap/` changed.

## What was built

### `runtime/` (C11, `make runtime` builds `runtime/libantrt.a`; `runtime/.gitignore` hides `obj/` and the archive)

- `ant.h`: the value representation of the design (16-byte tagged `AntValue`, `K_UNBOUND`/`K_UNINIT`/`K_ABSENT`
  sentinels), strings, tags, records with shapes, arrays, maps, boxes, `AntFunc`/`AntFuncInfo`, frames (push,
  pop and `ant_finish` inline), defer lists, field caches, the unwind state on `ant_g`, the `CHK`/`CHKB` macros,
  every entry point generated code uses, and, for the runtime's own natives, the argument-check macros
  (`ARR`/`MAP`/`STR`/`INT`/`FUNC`/`UNWOUND`) and the `AntMember` table a namespace is built from. It compiles
  with `-Iruntime` alone; `gc.h` is included only by `alloc.c`.
- `alloc.c` Boehm wrappers and the 512 MB main-stack thread (registered with the collector through gc.h's
  `pthread_create` redirect; no Boehm trouble seen). `value.c` kind names and the small checks (`ant_truth`,
  `ant_check_bool`, `ant_is_kind`, `ant_global`).
- `symbol.c`: symbols numbered at compile time and seeded by `ant_symbols_init`; `ant_sym` returns the seeded id
  or appends; interned shapes for spread literals.
- `string.c`: UTF-8 strings with `ncps`/`ascii` computed once, a builder, template helpers, and the String
  namespace (all of section 11 except that `toUpper`/`toLower` map ASCII only, left for 2.3).
- `number.c`: `+ - * / %` and comparisons with the `kind mismatch` before `not a number` order, integer ops
  through `__builtin_*_overflow` at the operand width, the `/ -1` and `% -1` cases (G5), `fmod` (G6), conversions
  (`conversion out of range`, G7), Bits (`shift count`, wrapping helpers), Math.
- `record.c` tags, records, `ant_record_build` for spreads (G10/G52), field get/set/check with per-site inline
  caches, `is`, module records (`not initialized` reads). `array.c` arrays, indexing, the Array namespace with a
  stable merge sort whose comparator bails out when a callback panics. `map.c` the compact dict (ordered entries,
  open-addressed index, tombstones, rebuild) and the Map namespace. `equal.c` structural `==` with the pair
  stack and the consistent hash. `render.c` `String.from` (quoting inside containers, shortest round-trip floats
  with the JavaScript layout, `...` for cycles).
- `func.c`: closures, natives with named parameters, boxes, `ant_call`/`ant_call_named` with every call check of
  section 12, yield objects (`yield after done`), `ant_for_each` over arrays (live length), maps, strings (code
  points) and sequences, `ant_try`. `unwind.c`: frames, `ant_panic` with the trace snapshot, `ant_set_return`,
  `ant_finish`, `ant_run_defers` (replacing panics, G59), the panic report, `ant_run_main` on the big-stack thread,
  `ant_exit` (`fflush` + `_exit`).
- `iter.c` the Iter namespace (stages as natives, a `false` travels back to the source), `result.c` Result and
  `Record.same`, `prelude.c` the prelude tags, conversions, `print`, `panic` and the namespace records,
  `io.c` print/Os/Time/File (whole-file calls only), `sched.c` a synchronous `ant_go` stub, `sync.c` the sync
  objects with the design's state and panics, where an operation that would block forever panics `deadlock`
  (step 2.6 brings the scheduler). Every namespace is one `AntMember` table (`{ member, params, code }`, or a
  constant value) that `ant_namespace` turns into the record, naming each native `Ns.member`.

### `src/antc/codegen/` (Ant, runs under antpy) and `src/antc/main.ant`

- `literals.ant`: int/float decoding with the range check (G1), string and template decoding with `\u{...}`
  validation (G2; template parts are re-scanned the way the lexer scanned them), C quoting with octal escapes.
- `modules.ant`: the module graph loaded depth first in import order (post-order result, entry last), path
  resolution, `not found` at the import line, `cycle: a.ant -> b.ant -> a.ant` at the first import of the cycle,
  the top-level name table per module (the module record's fields), import bindings (namespace or destructured
  name with the exporter's field index; an unknown name panics `unbound name` at the use site), and the `main`
  checks (G32/G53).
- `emit.ant`: the ANF lowering of the plan. Every subexpression lands in a temp before the next one is emitted
  (evaluation order), every panicking call is wrapped in `CHK`, blocks are C compound statements with a hoisted
  declaration of their `var`s as `K_UNBOUND`, an exit label, an optional defer list and the cascade
  (`if (unwind || pend) goto parent`), `pend` carries break/continue to the loop body's exit, `return` sets the
  unwind state and jumps. Names resolve through the textual-activation rule (G3) within a function and to the
  hoisted binding across function boundaries (G29); a name-based pre-pass boxes every local a nested body
  mentions, and closures copy box pointers through the free lists of every function in between. `for` bodies
  are closures carrying the enclosing frame as the return target; `defer`/`go` are zero-parameter closures;
  lambdas have the one parameter `x`. Module initializers run as an `init` function: imports first, then the
  record with its hoisted tags and functions, then the `var` initializers in order; a top-level `return` or
  `break` panics (G24).
- `build.ant`: C file to `$TMPDIR`, clang via `Os.exec` against `runtime/libantrt.a` (or `$ANT_RUNTIME`,
  `$ANT_CC`) with the link flags `make runtime` wrote to `runtime/libantrt.flags`, `antc run` builds a
  temporary binary, runs it with the program arguments and exits with its status. `main.ant` gains `build [--unchecked] [--emit-c] FILE -o OUT` and `run [--unchecked] FILE [ARGS...]`;
  `--unchecked` is accepted and ignored (no checker before M3). Diagnostics are `path:line:col: message`, exit 1;
  usage errors exit 2.
- `Makefile`: `runtime` (the archive and `libantrt.flags`, from `GC_PREFIX`, default `/opt/homebrew`) and
  `clean`.

## Decisions and deviations from the plan

- `Os.exec` is also in the C runtime now (the plan deferred it to 2.5): it is a few lines and the native `antc`
  needs it.
- Generated symbol constants are `SY_<name>`, not `S_<name>`: the runtime exports `S_value`, `S_error`, ... and
  the names would collide.
- Closures are created with `ant_closure(code, info, nfree, boxes)` / `ant_closure_ret(...)` (the plan's
  `ant_func_new` has no code pointer); `ant_callee(f)` checks `not a function` before the arguments evaluate;
  `ant_check_tag(v)` checks a record-literal head before its entries evaluate.
- `missing argument` is checked by `ant_call` before the callee runs rather than in parameter order between
  default evaluations (natives and compiled functions alike). The only observable difference is a default's side
  effect before the panic, which nothing tests.
- Record-literal shapes are one static per distinct field list, interned at startup (`init_constants`), so
  records of one shape share it with the records the runtime builds; `main` is called by its field index,
  which the emitter knows, rather than looked up by symbol.
- The `AntDefers` declaration of a block is filled into a reserved line once a `defer` is met, so blocks without
  one carry no defer list; no separate pre-scan.
- Literal errors (`literal out of range`, `invalid escape`) are collected by the emitter (first wins) and reported
  after emission rather than threaded as `Result`s through every emit function.
- The emitter's `Fn`/`Scope`/`Var` records and the `captured` pre-pass follow the plan; nothing in the AST
  changed.
- Prelude globals are read through `ant_global(G_x, "x")`, which yields `unbound name` for a name the prelude
  does not have, so `print(missing)` panics at run time after the preceding statements ran (G3).

## Self-hosting signal

`python3 -I bootstrap/antpy.py run src/antc/main.ant build src/antc/main.ant -o <bin>` builds a native `antc`
in about 6 s (5.7 s of antpy, the rest clang), and that binary passes the same 125 tests with 0 failures
(`--timeout 120`; re-verified after the refactor: `126 checks, 0 failures, 0 warnings`, 85 s for the five
sections against 3 minutes under antpy). Step 2.5's stage2/fixpoint Makefile targets remain to be written.

## Test changes

None.

## Refactor (after acceptance; the tree stayed green)

A `simplify` pass over the step's changes (four review angles: reuse, simplification, efficiency, altitude)
and five reviewer suggestions, applied one at a time with the smoke/functions subsets between and the full
acceptance command at the end. Behavior is unchanged: every panic phrase, output and exit status is the same.

- Runtime, shared: the argument-check macros `ARR`/`MAP`/`STR`/`INT`/`FUNC`/`UNWOUND` are defined once in
  `ant.h` (seven files defined their own); `ant_need_func` answers the function or NULL like the other
  `ant_need_*`; the hand-written copies of the int/array/string checks in io.c, map.c, string.c, sync.c,
  number.c and array.c use them. Every namespace is an `AntMember` table (`ANT_NAMESPACE` takes the count from
  `sizeof`; `ant_namespace` builds the `Ns.member` names and allocates its symbols for n, replacing the unchecked
  `Sym syms[64]`); prelude.c counts its tables with `ANT_LEN`.
- Runtime, one helper where there were copies: `ant_call_truth` (call then `ant_truth`, four copies),
  `with_lock` (Mutex and RwMutex), `run_stage` plus `ant_for_each` (iter.c's `drive` and three stage runners),
  `ant_need_ordered` (sort, sortBy, Math.min/max, which now use `ant_less`), `MATH1`, `clock_ms`,
  `io_error(err, path)` (also `Os.exec`), `ant_map_lookup` (get, has and map equality do one lookup), `put` in
  `ant_record_build`, one float case in the hash, `ant_closure` absorbing `ant_func_new`. Dead code removed:
  `ant_utf8_encode`, `S_message`, `S_index`, `need_s`. `ant_frame_push`/`pop`/`ant_finish` are inline in
  `ant.h`; `ant_exit` is `_Noreturn`; `ant_finish_init` holds the module initializer's `return` check that the
  emitter wrote inline; `ant_run_main` takes `main`'s field index (no `ant_shape_find` whose -1 went unchecked,
  no `SY_main`); templates build in an `AntSb` C local instead of a K_VOID value holding a pointer
  (`ant_template_start`/`finish` are gone); `ant_str_slice_cps` scans once; `*`, `<=`, `>`, `>=` have the int64
  fast path `+`, `-` and `<` had.
- Codegen: `declareInfo` records the info static on `Fn.info` (no scan of `g.statics` for a name at offset 25,
  no `#define FI_m_name` aliases; `emitInit` uses it too); `finish` pushes the prototype from the one
  signature; shapes are memoized per field list and interned at startup; `symArray`, `cPath` (a module path
  quoted once), `openChain`/`closeChain` (chains and pipes), `kindTest`, `assigned` (the `op=` read-once rule in
  one place), `bindName` with the module-var fallback (emitVarDecl's special case gone); the Field/OptField,
  Index/OptIndex, Call/OptCall and the five left/right branches are merged; the unused `g` parameters,
  `Loaded.kinds` and `cQuote`'s per-byte arrays are gone; `walkParams` takes `nested`.
- A bug the review found: record-literal entries before the first spread were emitted as raw C identifiers
  (`P{ x: 0, ...p }` failed in clang); every entry's symbol is emitted now.
- Build: `GC_PREFIX ?= /opt/homebrew` is the one place the Homebrew prefix lives; `make runtime` writes
  `runtime/libantrt.flags` (`-L$(GC_PREFIX)/lib -lgc -lpthread`) next to the archive and `antc build` reads it,
  so the stage targets of 2.5 and `antc build` cannot drift; the temporary C file is written only after the
  runtime check; `-Wno-missing-field-initializers` for the member tables. `main.ant`'s `problemExit` prints
  through `report`; the bootstrap's `Os.exec` reports a failed start through `fileio.failed`.
- Skipped (noted for later steps, not simplifications): the lexer and `literals.ant` both scan template
  parts; `Mod.Problem` beside `Lex.Diagnostic`; Iter stages rebuild an `AntFuncInfo` per run; the pair stack of
  `==` and the active list of `String.from` are linear scans; `ant_sb_finish` copies the buffer; `for` over a
  string and `String.chars` allocate per character; `Map.merge` rehashes keys it has hashes for; `ant_callee`
  stays out of line next to `callee`.

## Known limitations (for later steps)

- `String.toUpper`/`toLower` map ASCII only (2.3 may widen them); `Json`, file handles (`File.open`, `readLine`,
  ...) and `Http` are absent (2.7).
- Goroutines run synchronously and blocking sync operations panic `deadlock` (2.6).
- Stack traces show the enclosing function's frame for code in a `for` body (as the bootstrap does); line numbers
  are statement lines, with an operator's line for assignments.
- An assignment to a prelude name or a namespace import panics `unbound name` / `cannot assign` (checker rules;
  not tested).
- `ant_call` reports `missing argument` before any default runs (see above).
- The emitted C compiles with `-w`: unused labels and variables are expected.
- A record-literal spread checks `not a record` only after all entries evaluated (`ant_record_build`), while an
  array-literal spread checks in order (`ant_array_extend`); 2.2 should settle which the bootstrap does.
- `for`-body closures push no frame and emit no `ANT_SAFEPOINT`, so 2.6 must poll in `iterate()` and
  `yield_code`.
- The two-word `AntFieldCache` statics are only safe under the GIL.
- `build.ant`'s `tempPath` relies on millisecond clocks (fine while run.py is sequential).
- The yield object accepts the named argument `x:` instead of panicking `unknown parameter`.
