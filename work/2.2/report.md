# Report for step 2.2 "rt-types-errors"

## Status

Acceptance (`work/2.2/test.log`, exit 0), last line of the runner's output:

    144 checks, 0 failures, 0 warnings (implementation mode, 143 tests)

Self-check with `--consistency` (`work/2.2/consistency.log`, exit 0, required by gate 2.G rather than by this
row), last line:

    144 checks, 0 failures, 0 warnings (implementation mode, 143 tests)

(Both logs end with an extra `exit=0` line appended by the shell wrapper that ran them.)

## What was built

Step 2.1 had already built every item of this row's deliverable, because its own tests needed them: every
2.1 test imports `expect.ant`, which uses records, `==`, templates and `??`. The pieces live where the
design's file plan puts them:

- records with tags and shapes: `runtime/record.c` (`ant_record_new`, `ant_record_build`, field access with
  inline caches, `ant_is_tag`), shape interning in `runtime/symbol.c`;
- arrays `runtime/array.c`, maps (compact dict) `runtime/map.c`;
- `is`: `runtime/value.c` `ant_is_kind`, `runtime/record.c` `ant_is_tag`, emitter `emitIs` / `KIND_TESTS`;
- structural `==` with the pair stack and the consistent hash: `runtime/equal.c`;
- rendering with `...` for cycles and shortest round-trip floats: `runtime/render.c`;
- spread: `ant_record_build`, `ant_array_extend`;
- `?.`: `emitChain` / `nullSkip`; `??`: `emitNullish`; `try`: `runtime/func.c` `ant_try`, `emitUnary`;
- conversions: `runtime/number.c` `ant_convert`; replacing panics: `runtime/unwind.c` `ant_run_defers`;
- the prelude tags and `Result` / `Record`: `runtime/prelude.c`, `runtime/result.c`.

The acceptance command already passed on the committed tree (`work/2.2/baseline.log`, same last line). This
step's job was therefore to verify the deliverable against the spec beyond what the tests pin, which the probe
set did (below), and to fix the one divergence it found.

### The one change: a record spread is copied, and checked, at its position

`ant-lang.md` section 12: "Entries evaluate in order; a spread copies every field of a value that must be a
record" (and the panic table: "record literal | ... a spread operand is not a record"). The copy is the
spread entry's evaluation, in its place. The emitter used to evaluate every entry first and let
`ant_record_build` check and copy the spread operands afterwards, which was wrong in two ways:

1. a non-record operand panicked only after the later entries had evaluated: in
   `Point{ x: mark("a", 1.0), ...[1], y: mark("c", 2.0) }` the entry `y` printed `c` before
   `panic: not a record` (probes `01`, `41`; the bootstrap, and the array literal path the runtime already
   used through `ant_array_extend`, panic at the spread);
2. a later entry whose evaluation mutates the spread source leaked the mutation into the new record:
   `var p = P{ x: 1, y: 2 }; var bump = func(): int { p.y = 99; 5 }; print(P{ ...p, x: bump() })` printed
   `P{ x: 5, y: 99 }` instead of `P{ x: 5, y: 2 }` (probe `42`, found by the round-1 review: the first
   round had moved only the check to the spread's position, as `ant_check_record`, while
   `ant_record_build` still read the source's fields at build time).

`work/2.1/report.md` listed the first under "Known limitations" for 2.2 to settle. The fix (round 2):

- `runtime/ant.h`, `runtime/record.c`: a new `AntValue ant_record_snapshot(AntValue v)` next to
  `ant_check_tag`: `not a record` unless the value is a record, otherwise a shallow copy
  (`ant_record_make(r->tag, r->shape, r->fields)`; shapes are interned, so nothing else changes). The
  round-1 `ant_check_record` is gone; the snapshot subsumes it.
- `src/antc/codegen/emit.ant`, `emitRecordLit`, the `Ast.Spread` branch: the operand is landed through
  `checked(fn, `ant_record_snapshot(...)`)`, so the check and the copy both run through `CHK` in
  evaluation order: a non-record panics before the later entries evaluate, and a later entry sees only the
  source, never the copy. `names` / `syms` / `values` / `spreads` stay aligned by index, and
  `ant_record_build`'s own `not a record` check stays in place as a second line of defence (it now only
  ever sees the snapshots).

Diff against HEAD: 3 files, 16 insertions, 1 deletion. `make runtime` rebuilds without warnings. The cost
is one extra record allocation per spread entry, freed by the collector like any other.

## Decisions and deviations

- Round 2 (this round) addressed the review's blocking finding: the round-1 change had moved only the
  `not a record` check to the spread's position (`ant_check_record`) and left the copy in
  `ant_record_build`, so a later entry that mutated the spread source leaked into the literal. The fix is
  the snapshot described above; `ant_check_record` was removed rather than kept beside it, since the
  snapshot is its only caller's replacement. The review's program is probe `42` (expected and now `SAME`).
- Procedure followed as `work/2.2/plan.md` section 2 states, with one timing deviation: the `--consistency`
  self-check and the full probe sweep were started in the background while the acceptance was still
  running, instead of after it. `antc` builds under paths named by the wall clock, the monotonic clock and
  a hash of the entry (`src/antc/codegen/build.ant` `tempPath`), and the runner already builds tests side
  by side, so the runs could not clash; both test runs ended in `0 failures`.
- Probe set: 35 programs in `work/2.2/probes/` (34 from the plan plus `42-spread-source-mutated-later`,
  the reviewer's program), run with `sh work/2.2/probes/run.sh [NN ...]` from the repository root; each
  runs under the bootstrap and under antc and prints `SAME` or `DIFF` (stdout, exit status, first stderr
  line). Full run after the fix (`work/2.2/probes.log`): 33 `SAME`, 2 `DIFF`, both kept on purpose:
  1. `37-cyclic-key`: a cyclic record or array used as a map key. The bootstrap panics
     `map key too deep (cyclic?)`; the runtime hashes with the depth cap of `docs/runtime-design.md`
     section 1 ("recursion depth-capped at 8 ... so cyclic keys terminate") and finds the key by `==`
     (which handles cycles through the pair stack). No spec row or README phrase names such a panic, so the
     runtime's behaviour is the one the spec supports; the divergence is recorded here for step 4.1.
  2. `06-conversions`, the line `print(float32(3.4028235e38))` (a double above `FLT_MAX` that rounds to
     it): the runtime panics `conversion out of range` (`ant_convert`: `fabs(d) > FLT_MAX`, the literal
     reading of section 12 "outside the target's range"); the bootstrap dies with
     `panic: internal interpreter error`, a bug in `bootstrap/antpy/arith.py` outside this step's
     directories. There is no bootstrap behaviour to match, so the runtime keeps the spec's wording. The
     probe's other 13 lines agree.
- Refactored after the review: see "Refactor" below.

## Test changes

None. No test contradicts the spec; `work/TEST-CHANGES.md` is unchanged. `bootstrap/antpy/system.py` is
unchanged (`Os.exec` was already in HEAD).

## Known limitations (carried forward)

- `ant_call` raises `missing argument` before any default evaluates; the yield object accepts the named
  argument `x:`; `for`-body closures push no frame: 2.1 decisions, not in this row.
- Library widening (`String.toUpper` beyond ASCII, `Json`, file handles): steps 2.3 and 2.7.
- Goroutines and the sync objects beyond the synchronous stubs: step 2.6.
- The bootstrap's `float32` crash on a double just above `FLT_MAX` and its `map key too deep` panic on a
  cyclic key: `bootstrap/` is closed to this step; for step 4.1.

## Refactor

Run after the review on the working tree (`runtime/ant.h`, `runtime/record.c`, `src/antc/codegen/emit.ant`):
a single-pass simplify review (reuse, simplification, efficiency, altitude), then the reviewer's suggestion.
Behaviour is unchanged.

- `runtime/record.c`: the `kind != K_RECORD` / `ant_panic("not a record")` check appeared three times in the
  file (the new `ant_record_snapshot`, the `ant_record_build` pre-pass, `field_index`). Folded into one
  `static AntRecord *need_record(AntValue)` next to `ant_check_tag`, mirroring `ant_need_tag` (the record, or
  NULL after the panic). The runtime builds at `-O2`, which inlines it, so the field-access path is unchanged;
  nothing outside `record.c` panics `not a record`, so it stays static.
- `runtime/record.c`, reviewer suggestion: the comment above `ant_record_build` now says that the emitter lands
  every spread operand through `ant_record_snapshot`, where a non-record panics in evaluation order, and that
  the pre-pass check is a second line of defence rather than the primary one, so a later step does not move
  it again.
- Considered and left alone: the one extra record allocation per spread entry (inherent: the copy must happen
  at the spread's position, before the final shape is known; a builder API would allocate the same), and the
  emitter change (one line through the existing `checked` helper, at the right depth).

Diff against HEAD after the refactor: 3 files, 31 insertions, 9 deletions. `make runtime` rebuilds without
warnings.

Runs: after the helper, `--filter /smoke/ --filter /nullability/ --filter /errors/` (`work/2.2/subset1.log`:
58 checks, 0 failures, 57 tests) and `--filter /types/rec-` (`work/2.2/subset2.log`: 20 checks, 0 failures,
19 tests, among them `rec-spread-leak`, `rec-spread-non-record-rt` and the `rec-field-*-rt` panics); after the
comment, `--filter /smoke/` (6 checks, 0 failures). Full acceptance (`work/2.2/test.log`, exit 0; the
round-2 log is kept as `work/2.2/test.round2.log`), last line:

    144 checks, 0 failures, 0 warnings (implementation mode, 143 tests)
