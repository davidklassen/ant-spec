# Report for step 2.G "runtime gate"

Acceptance: `make fixpoint && python3 -I src/tests/run.py --run "bin/antc run {file} {args}" --run-unchecked "bin/antc run --unchecked {file} {args}" --consistency --tier run --tier panic --tier unchecked --tier programs --tier io`

The gate was green before the simplify pass (`work/2.G/review-test.log`: `445 checks, 0 failures, 0 warnings (implementation mode, 444 tests)`) and is green after it (`work/2.G/test.log`, the verbatim acceptance command rerun once every change was in; `work/2.G/test-first.log` is the run after the last group).

## Milestone simplify

Four reviewers (reuse, simplification, efficiency, altitude) read everything M2 changed; their 50 findings were deduplicated first (the name-resolution ladder was reported three times; the SYNC macro, the spelled-out unwind checks, the `check`/`need` pairs, the Iter stage rebuilds and the diagnostics twice each). Changes were made in three groups, the gate run after each (every run green, no reverts): shared-rule consolidations in the runtime, the emitter's, then the efficiency items. `make fixpoint` holds (`work/2.G/fixpoint.log`); `tools/compare_frontend.py lex|parse --all` still agrees on every file (1062 files, 0 failures each, `work/2.G/frontend-all.log`), since the lexer and parser changed for the template chunks.

Every panic phrase, output and exit status is unchanged. The one rule that moved is `withLock`'s merge of a panic raised by the unlock (the mutex unlocked inside `f`) over a panic raised by `f`: it now follows the deferred-block rule (G59, the newer panic wins), which is what the bootstrap's `try/finally` already did.

### Applied: shared rules, runtime

| Finding | Change |
|---|---|
| 3, 21, 45 (SYNC) | `SYNC(v, T, NAME, name)` in `ant.h`; the 18 natives of `sync.c` and the four File-handle natives of `io.c` use it (`need_file` gone). `NUM`, `ORD` and `COUNT` (an `INT` that rejects negatives: the `negative count` check was written three times) complete the set; `RESULT` is local to `result.c`. |
| 4, 22, 44, 46 (in part) | One implementation per check: `ant_check_bool` is `ant_truth` plus the value, `ant_not` is `ant_truth` negated, `ant_check_tag` wraps the static `need_tag`, `ant_callee` wraps the static `callee` that `ant_call` uses. `ant_global` and `ant_box_get` are one `ant_bound(v, name)`; the emitter emits it for prelude globals, boxed reads and statically unbound names, so `unbound name` lives in one C function. The record constructors take the tag (and the spread operands) as preconditions, documented in `ant.h`: the emitter lands them through `ant_check_tag` / `ant_record_snapshot` in evaluation order, so the second `not a tag` and the `not a record` pre-pass are gone (28, 46). |
| 6, 20 | Every spelled-out `if (ant_g->unwind.mode) return ANT_VOID;` is `UNWOUND();` (result.c, func.c, sync.c, array.c). |
| 7 | `ant_array_from(p, n)` for `Array.slice` and `sorted_copy`; `Array.concat` is two `ant_array_extend`s, `Os.args` one. |
| 10 | `ant_fmt` / `ant_vfmt` (measure, allocate, print) in `string.c`; `ant_panicf`, the trace lines, `path_error`, the `Json.parse` error and the `Ns.member` names use it. No fixed buffer truncates any more. |
| 11 | `ant_fnv1a` exported from `string.c`; `ant_str_hash` caches it, `symbol.c` hashes C strings with it. |
| 14 | `ANT_PUSH(items, n, cap, first, item)` in `ant.h` for the five grow-by-doubling lists (render, equal, unwind, prelude, symbol). |
| 15 | `ant_utf8_cont` (inline) at the five continuation-byte tests; `ant_str_next_char` for `String.chars` and `for c in s`. |
| 16 | `alloc_record` shared by `ant_record_make` and `ant_module_new`; `ant_check_slice` for the two slice natives. |
| 17 | `ant_wrong_kind(what)` and `ant_need_kind(v, k, what)` in `value.c`: every typed need (`ant_str`, `ant_need_array`, `ant_need_map`, `ant_need_func`, `ant_need_int`, `need_f64`) is a one-liner, the predicate ones (`number`, `ordered`, `integer`, `integers of one kind`, `a Result`, `a record`, `a byte`, the sync names) call `ant_wrong_kind`. `ant_need_int` moved to `value.c`. |
| 26 | `wg_change(wg, n)` holds the WaitGroup counter rule; `add` and `done` call it. |
| 29 | `ant_str_slice_cps`, `ant_convert`, `ant_map_has`, `ant_map_remove`, `ant_yield_new`, `ant_yield_done`, `ant_need_tag` are static (or folded into their one caller) and out of `ant.h`. |
| 30 | `alloc.c`: one `checked(p)` behind every wrapper. |
| 31 | `yield_info` is a `static const` initializer. |
| 39 | `ant_unwind_aside` / `ant_unwind_merge` in `unwind.c` hold the set-aside-and-merge rule; `ant_run_defers` and `with_lock` call them. |
| 40 | The sort comparator answers the bool; `merge_sort` tests the unwind flag after each call like every other callback site, and `sorted_copy` uses `UNWOUND()`. |
| 47 | `value.c` owns one `{ name, kind }` table (`ant_kind_table`), aliases included; `prelude.c` defines the conversion functions by iterating its numeric rows. |
| 48 | `field_at(r, i)` consumes `K_UNINIT` for `ant_field_get` and `ant_module_get`. |
| 49 | `ant_namespace` defines the prelude global itself; the `ant_ns_*` functions are `void` and `ant_prelude_init` calls them from one table. A namespace's name is written once. |

### Applied: the emitter

| Finding | Change |
|---|---|
| 1, 18, 41 | `resolve(fn, name): Resolved` (`LocalVar`, `ModuleVar`, `ModuleRef`, `PreludeName`, `Unbound`) is the one ladder; `emitIdent` and `storeName` are switches over it that differ only in the read or write they emit. The last rung is unchanged: a read of a prelude name goes through `ant_bound(G_x, "x")`, a write to one is unbound. |
| 2 | The lexer records the raw text around a template's parts (`chunks` on the TEMPLATE token, `text(lx, start)` at each `${` and the closing backtick); it travels through `parser.ant` into `Ast.Template.chunks` (the dump ignores it); `Lit.templateChunks` decodes each chunk with `decodeRange`, and `skipPart` / `skipString` / `skipTemplate` are deleted (literals.ant 201 -> 132 lines). |
| 8, 50 (minimal) | `modules.ant` builds a `Problem` from a parser `Diagnostic` by spread (`Problem{ path, ...d }`); `main.ant`'s `report` takes any positioned record (`{ line, col, message }`), so `problemExit` is `report(p.path, p)`. No field is copied by hand. |
| 9 | `fieldRead(g, fn, obj, name)` for destructuring, chain steps and paths; `emitAssign`'s field arm keeps its shared sym/cache for the check/get/set triple. |
| 12 | `symC`, `globalC`, `moduleC`, `initC`, `declFnC` spell each C prefix once. |
| 13, 24 | `Mod.intern(table, key, declare)` (first-seen numbering, `declare` once) behind `addName`, `sym`, `global`, `literal` and `shape`; all five tables are `Map[string, int]`. |
| 19 | `emitBody(g, fn, stmts, rootKind, resultInit, bind)` is the scaffold of `emitFunction`, `emitForBody` and `emitInit`; `emitPendCheck` the shared epilogue line. |
| 23 | Defer lists are declared in the prologue like the temps (`fn.defers`); `Scope.declAt` and the placeholder line are gone. Correct because every block exit runs `ant_run_defers`, which leaves the list empty for the next entry (an iteration now reuses its buffer instead of reallocating it). |
| 25 | The emitter emits `ant_str_new`; `ant_str_lit` is deleted. |
| 27 | `emitShortCircuit(g, fn, left, right, isAnd)`; `emitTargetAssign(check, get, set)` for the field and index arms; `emitPipe` is `emitChain(call, [left])` with `first` threaded through `emitStep`. |
| 42 | `ast.ant` exports `mentions`, `children` and `nestedBodies` (with a final panic for an unknown node); `walkExpr` is a four-line generic walk. `x is int` now also counts as a mention of `int` inside a nested body, which can only box a local of that name, never change what it holds. |

### Applied: efficiency

Measured with `bin/antc run` on the rebuilt compiler, against the reviewers' baseline measurements:

| Finding | Change | Effect |
|---|---|---|
| 32 | `Map{}` and `[]` allocate their tables at the first insertion (`ant_map_new`, `ant_array_new(0)`, `grow` starting at 4). | 1M empty `Map{}`: 206 -> 28 ms. |
| 33 | `File.readLine` reads with `getline` into a buffer kept on the handle (freed at close), then one validation and one `ant_str_new`; `Json.parse` appends string runs at once. | 200k 40-byte lines: 164 -> 42 ms, the write of the file included. |
| 34 | `ant_alloc_big` / `ant_alloc_atomic_big` (`GC_MALLOC_IGNORE_OFF_PAGE`) for array data, map entries and the map index. | 1M `Map.set` plus a merge prints no `GC Warning` lines. |
| 35 | The string builder writes into the `AntString` it becomes (`alloc_str`, `finish_str`): one allocation and one scan per built string. | Part of the template timing below. |
| 36 | Integers render through `render_uint` (digits backwards into a local, one append). | 1M two-int templates: 65 ms (170 ns each before). |
| 37 | One-character ASCII strings are shared (`ascii1` in `string.c`). | `String.chars` of 2M characters: 27 -> 11 ms. |
| 5, 38 | One `static const AntFuncInfo` per Iter stage kind (`STAGE_INFO`), created through `ant_closure_data`; `ant_native` stays for the init-time tables. `S_yield` joins the prelude symbols. | 500k three-stage pipelines: 287 -> 117 ms. |

### Skipped

| Finding | Why |
|---|---|
| 46 (the `callee` part) | `ant_call` keeps its own `not a function` check: `main` can be reassigned at top level and the root's call must still panic, and dropping the check would make every native's callback path rely on the FUNC() audit alone. The check is one static function that `ant_callee` wraps, so the phrase lives once; the generated-code path tests twice, as before. |
| 50 (one diagnostic type) | Giving the lexer's `Diagnostic` a nullable path would thread the path through `tokenize` and `parse` for no reader yet; the conversions it complained about are gone through 8, and M3's checker can report through `report`'s structural `{ line, col, message }` without a new type. |
| 43 | Sending duplicate-name record literals through `ant_record_build` would make those sites intern their shape at run time and change which shape identity `==`'s fast path sees; one copy is compile-time and the other run-time, and G10 has tests on both. Left as is. |
| 45 (macros beyond SYNC/NUM/ORD/COUNT) | `RESULT` stays local to `result.c` (its check is private); no other check recurred enough to earn a macro. |

### Test changes

None.
