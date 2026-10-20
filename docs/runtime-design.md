# Ant runtime and code generator: technical design

Companion to `ROADMAP.md`. Milestone M2 implementers follow this document; deviations must be justified in the step's `work/<id>/report.md`. Everything here is derived from `ant-lang.md` §4/§8/§9/§11/§12, `stdlib.md`, `src/tests/README.md` (phrases, exit codes, report formats), and the GAPS readings the tests encode (G3, G10, G13, G23, G25, G29, G30, G33, G39, G48, G51, G52, G54, G55, G59). Guiding rule: generated code is fully dynamic; every operator and native is a kind-switching C function; one uniform unwinding protocol.

Scope: the C runtime (`runtime/`, static library `libantrt.a`) and the C emitted by `antc build`.

## 1. Value representation

**Tagged 16-byte struct, not NaN-boxing.** Ten numeric kinds including full-width `int64`/`uint64` and `float32` make NaN-boxing pointless (the big integers would be boxed anyway and the kind would still need storing). Passed by value in two registers on arm64.

```c
typedef enum { K_INT8, K_INT16, K_INT32, K_INT64, K_UINT8, K_UINT16, K_UINT32, K_UINT64,
               K_FLOAT32, K_FLOAT64, K_BOOL, K_STRING, K_NULL, K_VOID, K_ARRAY, K_MAP,
               K_RECORD, K_FUNC, K_TAG, K_SYNC,
               K_UNBOUND, K_UNINIT, K_ABSENT } AntKind;   /* last three never escape (see below) */
typedef struct { uint32_t kind; union { int64_t i; uint64_t u; double f; float f32; bool b; void *p; } as; } AntValue;
```

Sentinels: `K_UNBOUND` (hoisted local before its `var` ran; read panics `unbound name`), `K_UNINIT` (module var before its initializer; read panics `not initialized`), `K_ABSENT` (unfilled parameter slot in a call vector). Each is consumed by the one check that can see it; none is ever stored into an array/map/record.

Heap objects (all `GC_MALLOC`, strings' bytes `GC_MALLOC_ATOMIC`):

- **String** `{ int64 nbytes; int64 ncps; uint32 hash; uint8 ascii; char data[]; }` immutable UTF-8, validated at creation (`String.fromBytes` returns `null` on bad input). `ncps` and `ascii` computed once, so length is O(1) and index/slice are O(1) when ASCII, otherwise a forward scan. Ordering is `memcmp` on bytes, which equals code-point order for valid UTF-8.
- **Sym** (interned field/parameter name): a `uint32` id into a global intern table (`symbol.c`); the compiler emits `S_name` constants resolved at startup from string literals (one `ant_sym("x")` per distinct name in a module init).
- **Shape** `{ int n; Sym names[]; }` interned by its name sequence (hash table in `symbol.c`). **Record** `{ AntTag *tag; AntShape *shape; AntValue fields[]; }`. The shape is per literal site, not per tag, because a spread may add fields the tag never declared and they render in creation order (G10/G52). A literal without spread has a static shape computed once at its site; with spread the names are gathered in order (later entry replaces earlier, position kept) and interned (`not a record` for a non-record spread operand). Field read `a.b`: `not a record` unless K_RECORD, then linear scan of `shape->names` guarded by a per-site inline cache (`static AntShape *c_shape; static int c_idx;`), `no field` on miss. Field write: same, no new fields.
- **Tag** `{ const char *name; }`: one GC object per `type` declaration, allocated by the module's init and stored in the module record; identity is the address, so an imported alias is the same tag (mod-tag-identity). Prelude tags `Ok`, `Err`, `Error`, `Entry`, `Indexed` live in `prelude.c`; `Result.ok(v)` builds a record with `Ok` and shape `[value]`.
- **Array** `{ int64 len, cap; AntValue *data; }`, growth factor 2.
- **Map**: compact dict (insertion-ordered `entries[] {uint32 hash; AntValue key, value; bool alive}` plus an open-addressed `int32 index[]`). Hash is kind-sensitive and consistent with `==`: numbers by kind + bits, strings by content hash, bool/null/void constants, records by tag + fields, arrays elementwise, maps by XOR of entry hashes, func/tag/sync by address; recursion depth-capped at 8 (beyond that contributes a constant) so cyclic keys terminate; equality does the rest. `NaN` hashes but never compares equal, so each insert adds an entry and lookups miss (G13). `for` over a map walks `entries` by index below the current length skipping dead slots: mutation during iteration is harmless to memory.
- **Equality** (`equal.c`): structural per §12 with a per-goroutine stack of `(a,b)` pointer pairs under comparison; a pair already on the stack answers `true`. No identity shortcut (G11: `p == p` is false with a NaN field).
- **Func** `{ AntNative code; const AntFuncInfo *info; int nfree; AntBox *free[]; }` with `AntFuncInfo { name, file, line, nparams, Sym params[], bool hasDefault[], bool isLambda }`. Flat closure conversion: the compiler does a free-variable pass per function body; every local captured by any nested `func`/lambda/`go`/`defer` body becomes a heap cell `AntBox { AntValue v; }` and the closure copies the box pointers. Uncaptured locals are plain C `AntValue` locals. `go` and `defer` blocks are zero-parameter closures (so `return` inside them leaves the block only, G24).
- **Sync** `{ uint8 syncKind; ... }` (section 3). Renders as its namespace name; compares by identity.

**Scoping rules the codegen must implement** (they reproduce the tree-walker's dynamic lookup): all `var`s of a block are declared (and, if captured, boxed) at block entry, initialized to `K_UNBOUND`; a reference resolves lexically to the innermost block declaring the name, except that straight-line code textually before the declaration, and the declaration's own initializer, resolve outward (`var x = x + 1` sees the outer `x`, G3), while nested function bodies and deferred blocks see the hoisted binding (G29: `defer { print(y) }; var y = 2` prints 2). Re-entering a block allocates fresh boxes, which gives per-iteration `while`-body freshness and per-iteration `for` variables for free. A name with no binding anywhere compiles to `ant_panic("unbound name")` at the use site (after preceding statements ran).

**Modules and namespaces.** A module is a record whose shape lists every top-level name (var, func, type); the fields are the storage for the module's globals. A module's own code reads/writes `M->fields[idx]` (static index); `import "./x.ant" X` binds the local `X` to the importer's record value; `import { count: c }` compiles each use of `c` to a read of the exporter's field with the `K_UNINIT` check. Live imports, unchecked writes through a namespace (G31), and unchecked reads of private fields fall out of this. Prelude namespaces (`Array`, `Map`, ...) are ordinary records whose fields are native funcs, so `Array.map` is a field read with an inline cache; `print`, `panic`, the ten conversion functions, `bool` and `string` (bound to tag values named `bool`/`string`, so calling them panics `not a function`, G8) are prelude globals resolved after module scope.

**`is` resolution.** If the right side is a single identifier in the fixed set `null void bool string func Array Map int8..uint64 float32 float64 int byte`, the codegen emits a kind test by name regardless of bindings (`int` is K_INT64, `byte` is K_UINT8, G48). Any other path is evaluated; `not a tag` unless K_TAG; result is `v.kind == K_RECORD && rec->tag == tag`. Aliases of unions bind a tag of their own, so `x is Shape` is `false` (G17).

**Operator check order** (fixed by the unchecked tests): binary arithmetic/comparison first requires `a.kind == b.kind` (`kind mismatch`: `"a" + 1`, `1 < "a"`), then that the kind is numeric, or string for `+` and the comparisons (`not a number`: `true + true`); unary `-`/`!` check their one operand. Integer ops use `__builtin_*_overflow` at the operand width (`integer overflow`, including `-INT_MIN`, `INT_MIN / -1`, unsigned below zero), `division by zero` for `/` `%`; float `%` is `fmod`. Conversions: the argument must be a number (`wrong kind`, as for any library function), then range/NaN/Inf checks give `conversion out of range`; `float32(1e300)` is out of range (G7). Library natives check their arguments' kinds first and panic `wrong kind` (`panic(1)` too, G67).

**If a hand-written collector replaced Boehm:** the kind field already makes heap scanning precise; the missing piece is roots. Generated code would have to keep live `AntValue`s in the per-frame `AntFrame` (an `AntValue slots[N]` array registered on the shadow stack, section 2) rather than in bare C locals across calls, natives would use a push/pop root API, collection would run only at safepoints while holding the GIL (every allocation is a safepoint), and the collector must be non-moving unless strings/arrays never cache interior pointers. The frame struct exists already for stack traces, so this is an incremental change rather than a redesign.

## 2. Panics and non-local control flow

**Explicit status propagation, not setjmp/longjmp.** Every goroutine owns `AntUnwind { enum {NONE, RETURN, PANIC} mode; AntValue value; AntFrame *target; AntTrace *trace; }` reached through the thread-local `ant_g`. Rationale: no `volatile` hazards in generated code, natives that call back (`Array.map`, `Mutex.withLock`, `Iter.*`, sort comparators) just test one flag after each callback and release what they hold, the mechanism is identical for `return`-through-sequence and for panics, it is independent of the threading model, and the shadow stack used for traces doubles as the return target. Cost: one well-predicted branch after each call or panicking operation.

Generated-code protocol (the codegen lowers every expression to C statements with temps, so these checks are mechanical):

```c
#define CHK(e, L) ({ AntValue _v = (e); if (ant_g->unwind.mode) goto L; _v; })
```

- Every block owns a defer list `AntDefers dN` and an exit label `bN_exit:` which runs `ant_run_defers(&dN)` and then `if (ant_g->unwind.mode || pend) goto <outer exit>` (or falls through to the block's continuation). Within a function this cascade implements `return`, `break`, `continue`, `try`, and panic propagation uniformly; `pend` is a function-local `int` for break/continue (consumed at the loop's body-block exit: break jumps past the loop, continue to the next test) and is never crossing frames.
- `return e`: evaluate `e` (before any defers run, cf. defer-cannot-change-return), `ant_set_return(&fr, v)`, `goto` innermost exit. The function epilogue: `ant_frame_pop(&fr); if (mode == RETURN && target == &fr) { result = value; mode = NONE; } return result;`.
- `try e`: `null` -> set_return(null); record tagged `Err` -> set_return(fresh `Err{ error }`); `Ok` -> its `value` field; anything else unchanged (G28).
- `ant_panic(msg)` (also called by every runtime check): sets PANIC, snapshots the shadow stack into `trace`, returns; the next `CHK` starts the cascade. Natives return `ANT_VOID` after setting a panic; callers check.
- `ant_run_defers`: pops closures in reverse order; for each, saves and clears the pending unwind, calls the closure (its own `return` targets its own frame), then: if a new PANIC is pending it replaces the saved state (dropping a saved RETURN or PANIC, including its trace), otherwise the saved state is restored. Remaining defers still run (G59). Order in defer-panic-replaces: body, second, first, then `panic: replacement`.
- Goroutine root (`sched.c` trampoline, and `main`): after the root closure returns with PANIC pending, write `panic: <message>\n` followed by the trace lines `  at <func> (<file>:<line>)` to stderr; main goroutine: `fflush(stdout); _exit(2)`; other goroutines: thread ends. Panics are uncatchable by construction: no primitive clears PANIC except the root.

**Stack traces.** `AntFrame { const AntFuncInfo *info; int line; AntFrame *parent; }` lives on the C stack; `ant_frame_push/pop` link it into `ant_g->top`. The codegen stores `fr.line = N` before each statement that can panic. The snapshot is taken at `ant_panic` time, so unwinding (which pops frames) does not lose it. Hundreds of frames cost nothing until a panic.

**`for x in seq` and `return` through the sequence (G25).** Every `for` body is compiled as a *yield closure* with one parameter (the loop variable, a fresh binding per call) and the runtime drives it: `ant_for_each(v, body)` dispatches on kind: array (by index below the current length, so appends are visited and pops shorten the loop), map (by entry index), string (code points as one-character strings), K_FUNC (creates a yield object: a K_FUNC with `state = ACTIVE` wrapping `body`, calls `seq(yield)`, then sets `state = DONE`); anything else panics `not iterable`. Destructuring in the loop head is lowered inside the closure (`not a record`/`no field`). Inside the body: `continue` returns `true` from the closure; `break` for this loop returns `false` and the yield object becomes DONE; `return v` calls `ant_set_return(enclosingFrame, v)` where `enclosingFrame` is a captured pointer to the enclosing function's `AntFrame`, then returns. The sequence function sees the unwind flag at its `CHK` after `yield(...)`, runs its own defers (seq-return-runs-seq-defers), returns to the `for` site, whose `CHK` continues the cascade in the enclosing function, which consumes the RETURN at its epilogue. Yield checks: `missing argument` for zero arguments, `unknown parameter` for named ones, `yield after done` when state is DONE (so an escaped yield called after the sequence returned never touches the dead frame). Native sequences (`Iter.range`, `Iter.map`, ...) obey the same rule: after calling `yield`/the source, `if (ant_g->unwind.mode) return VOID;`. The per-iteration closure call is the price of one codegen shape; inlining array loops is a later, purely local optimization.

## 3. Goroutines and synchronization

**Choice: one OS thread per goroutine, a global interpreter lock (GIL) with safepoint polling.** Over coroutines on one thread because: (a) blocking I/O and `Time.sleep` become "release the GIL around the syscall", no thread pool or timer wheel; (b) Boehm supports pthreads directly (`#define GC_THREADS`, `gc.h` included before `pthread.h` redirects `pthread_create`); coroutine stacks would each need registering with the collector; (c) `makecontext/swapcontext` are deprecated on macOS arm64; (d) the GIL makes the §9 memory-safety guarantee free: every runtime operation (array growth, map insert, field write) executes atomically with respect to other goroutines, so a race can only reorder whole operations (race-memory-safe). Hundreds of threads with a 512 KB-1 MB stack each are fine (committed lazily). The spec promises concurrency, not parallelism, so the GIL costs nothing it promised.

**GIL** (`sched.c`): `{ pthread_mutex_t m; pthread_cond_t switch_cv; AntGoroutine *holder; int waiters; volatile int switch_requested; uint64 generation; }`. A ticker thread sets `switch_requested` every ~5 ms when `waiters > 0`. `ANT_SAFEPOINT()` (`if (gil.switch_requested) ant_gil_yield();`) is emitted at every loop back-edge and function entry, so `while true {}` cannot starve `main` (goroutines-preemptive). `ant_gil_yield` records the generation, unlocks `m`, waits on `switch_cv` until the generation changes (CPython-style forced handoff; a bare unlock/lock re-acquires immediately on macOS), then relocks. Blocking natives call `ant_gil_release()`/`ant_gil_acquire()` around the syscall and must not touch any AntValue in between (copy strings/buffers out first).

**Sync objects** are monitors on the GIL: each has a `pthread_cond_t` whose mutex is `gil.m`, and `ant_block_on(cv)` = `holder = NULL; pthread_cond_wait(cv, &gil.m); holder = me;`. Since all state lives under the GIL there are no lost wake-ups between test and wait.
- `Mutex { bool locked; cv }`: lock waits while locked; unlock panics `not locked` when not locked, else clears and signals. `withLock`: lock, call f, check unwind, unlock, propagate (mutex-withlock-returns-and-releases).
- `RwMutex { int readers; bool writer; int writersWaiting; cv }`: readers admitted while no writer (G51), writer waits for `readers == 0 && !writer`; `readUnlock` with `readers == 0` and `writeUnlock` without `writer` panic `not locked`.
- `WaitGroup { int64 n; cv }`: `add` with a result below zero and `done` at zero panic `negative counter`; `wait` blocks while `n > 0`; reaching zero broadcasts.
- `Once { enum {NEW, RUNNING, DONE}; cv }`: first caller sets RUNNING, calls f, sets DONE whether or not f panicked (G30), broadcasts; concurrent callers wait for DONE; later callers return at once.
- `Cond { Mutex *m; cv }`: `wait` panics `not locked` if `m` is not locked, releases `m` (signal its cv), `ant_block_on(c->cv)`, re-acquires `m`.
- `Atomic { AntValue v }`: all operations are trivially atomic under the GIL; `add` requires two numbers of the same kind (`wrong kind`) and panics `integer overflow`; `compareAndSwap` uses structural `==` (G40).

**Goroutine lifecycle.** `go { }` compiles to `ant_go(closure)`: allocate `AntGoroutine` (`GC_MALLOC_UNCOLLECTABLE`, linked into a global list so the closure and the unwind state stay reachable even though `ant_g` is `__thread`, which Boehm does not scan), `pthread_create` detached with a 1 MB stack. The thread acquires the GIL, sets `ant_g`, runs the closure, reports a pending panic (section 2), unlinks itself, releases the GIL. `Time.sleep(ms)` is `nanosleep` outside the GIL (sleep-blocks-only-caller). When `main` returns, the main thread holds the GIL, so no other goroutine is mid-operation: `fflush(stdout); fflush(stderr); _exit(status)` (no atexit handlers with live threads; spinners are simply abandoned, go-main-exit-cuts). `Os.exit(n)` from any goroutine does the same immediately without running defers (G39). The main goroutine is the process's main thread; `GC_INIT()` runs before anything else.

## 4. Code generation

The compiler lowers each function body to ANF-style C: every subexpression becomes a temp assigned by one statement, which fixes C's unspecified evaluation order (callee before arguments, arguments left to right, fields in order, target sub-expressions before the right-hand side) and gives each `CHK` a place. One C file per program with modules as sections prefixed `m<k>_`: the simplest build (one clang invocation, no headers, cross-module inlining); the sectioning keeps a later split into one TU per module mechanical.

Function (`func clamp(value, low, high)`):
```c
static const Sym P_clamp[] = {S_value, S_low, S_high};
static const AntFuncInfo FI_clamp = {"clamp", "clamp.ant", 3, 3, P_clamp, (bool[]){0,0,0}, false};
static AntValue F_clamp(AntFunc *self, int argc, AntValue *argv) {
  AntFrame fr; ant_frame_push(&fr, &FI_clamp); ANT_SAFEPOINT();
  AntValue value = argv[0], low = argv[1], high = argv[2];     /* K_ABSENT only where hasDefault */
  AntValue result = ANT_VOID; int pend = 0;
  { AntDefers d0 = {0};
    fr.line = 2; AntValue t0 = CHK(ant_lt(value, low), b0_exit);
    if (ant_bool(&t0)) { ant_set_return(&fr, low); goto b0_exit; }   /* ant_bool panics `not a bool` */
    ...
    result = value;
  b0_exit: ant_run_defers(&d0); }
  ant_frame_pop(&fr); return ant_finish(&fr, result);
}
```
Parameters with defaults: after all arguments are bound, in parameter order, `if (b.kind == K_ABSENT) b = CHK(<default expr>, b0_exit);` in the callee's scope, so `b: int = a * 2` works (G26) and defaults re-evaluate per call. A declared function value is `ant_func_new(&FI_clamp, 0, NULL)` created once at module init and stored in the module record.

Calls: `t0 = CHK(callee); t1 = CHK(arg1); ...; r = CHK(ant_call(t0, n, (AntValue[]){t1,...}));`. `ant_call` checks `not a function`, `too many arguments` (a lambda has one parameter `x` and accepts 0 or 1, `x = void` for 0, G23), fills a `nparams` vector with `K_ABSENT`, panics `missing argument` for an absent parameter without default, then invokes `code`. Named arguments: `ant_call_named(f, npos, pos, nnamed, syms, vals)` additionally checks `named argument to lambda`, `unknown parameter`, `parameter already filled`; through a function value it works when the parameter exists (G14). A pipe `a | f(b)` is the call `f(a, b)` with `a` evaluated first, then `f`, then `b`.

Lambda / anonymous func / `go` / `defer`: a separate C function `F_lam<k>` with its own `AntFuncInfo` (`isLambda` for `{ ... }`); creation is `ant_func_new(&FI, nfree, (AntBox*[]){box_a, box_b})`; inside, captured reads are `self->free[k]->v`. `defer { }` is `ant_defer_push(&dN, closure)`; `go { }` is `ant_go(closure)`.

Loops: `while` is an inline C loop with `ANT_SAFEPOINT()` at the back-edge and the body as a block whose exit consumes `pend`. `for` is the yield-closure form of section 2: `CHK(ant_for_each(CHK(e), closure))`; its body closure captures the enclosing `&fr` for `return`.

`a ?? b`: `t = CHK(a); if (t.kind == K_NULL) t = CHK(b);` (b may be `return`/`break`/`panic`, which just emit their cascade). Optional chains `a?.b.c(x)`: the whole postfix chain is lowered with one skip label; a null left side jumps to it with `null` and the rest is not evaluated (optchain-skips-rest). Template strings: `ant_sb_*` builder appending `ant_render(v)` per part. Record literal: `not a tag` check on the head, static shape when there is no spread. Map literal: `ant_map_set` per entry in order. Array literal: `not an array` on a spread operand. Compound assignment: target sub-expressions evaluated once into temps (G4).

Modules: for each module the emitter produces the tag objects, the function bodies, a static shape of all top-level names, `AntValue M_k`, and `void init_k(void)` which: returns if already running or done (unchecked cycles then read `not initialized`); calls the inits of its imports in source order (depth-first, once: mod-init-order-diamond); allocates the record with every field `K_UNINIT`, then stores tags and function values (hoisted), then runs `var` initializers in source order. The generated `main`: `GC_INIT(); ant_runtime_init(argc, argv)` (stores `Os.args()` as `argv[1..]`, G39), `ant_prelude_init()`, `init_entry()`, look up `main` in the entry module and `ant_call` it as the main goroutine, then exit as in section 3. `main` in imported modules is an ordinary function.

`antc build` writes the C file to a temp directory and runs `clang -O1 -std=gnu11 -o <bin> out.c -I<runtime> <runtime>/libantrt.a -lgc -lpthread` (Homebrew `bdw-gc` include/lib paths); `antc run` builds to a temp binary and `execv`s it with the arguments.

## 5. Runtime modules and implementation order

`runtime/` (C11, clang): `ant.h` (AntValue, kinds, macros, every `ant_*` entry point the codegen uses) · `value.c` (constructors, sentinels, `ant_bool`, kind names) · `alloc.c` (Boehm wrappers, GC init) · `symbol.c` (symbol and shape interning) · `string.c` (UTF-8 strings, builder, code-point index/slice, String natives) · `number.c` (arithmetic, comparison, conversions, Bits, Math) · `record.c` (literals, spread, field access with inline caches, tags, `is`) · `array.c` · `map.c` · `equal.c` (structural `==` with the pair stack) · `render.c` (`String.from`: quoting inside containers, shortest round-trip floats with JS exponent thresholds via `%.{1..17}g` and re-parse, `NaN`/`Infinity`, `...` for back-references; shared walker for `Json.stringify`) · `func.c` (closures, `ant_call`, `ant_call_named`, yield objects, `ant_for_each`) · `unwind.c` (frames, panic, set_return, run_defers, traces, root reporting) · `iter.c` · `result.c` (Result, Record.same, prelude tags) · `sched.c` (GIL, goroutines, safepoints, exit) · `sync.c` · `io.c` (print with a runtime-owned stdout buffer, File, Os, Time) · `json.c` · `prelude.c` (namespace records). `Http` is in `stdlib.md` but untested; stub it to return `Err` until needed.

Order, matched to the tiers:
1. `ant.h value alloc symbol string number func unwind` plus the codegen for expressions, blocks, calls, closures, defers, `while`, `for` (all four kinds; sequences need `func.c`'s yield objects from day one) and basic `print`: passes `smoke`, `lexical`, `variables`, `functions`, `expressions`.
2. `record equal map render result`, conversions, `try`, `is`, `??`/`?.`: `types`, `nullability`, `errors`; the `unchecked` tier is the same code with the phrase table audited.
3. Remaining natives in `array map string iter number`, float formatting, stable merge sort with unwind bail-out: `stdlib`.
4. Module sections, init order, imports, `Os.args`: `modules`, `programs` (except the concurrent programs).
5. `sched sync`: `concurrency`, `prog-parallel-sum`, `prog-producer-consumer`, `race-memory-safe`.
6. `io json`: `io`.

## 6. Risks and the decisions not to get wrong

1. **The unwind check is non-negotiable.** Every call into Ant code and every runtime operation that can panic must be followed by `CHK`, and every native that calls back must test the flag after each callback and release what it holds. One missed check means a deferred block runs in the wrong order or code continues after a panic. Make the codegen's expression lowering the only path that emits calls, and audit natives by grepping for `ant_call`.
2. **Hoisted, boxed locals with the `K_UNBOUND` sentinel and fresh boxes per block entry.** This single rule yields per-iteration `for` bindings, fresh `while`-body variables, closures seeing later assignments, `defer` reading a later `var`, and `var x = x + 1` seeing the outer `x`. Do not "optimize" captured variables into C locals, and do not resolve names in nested bodies by textual position.
3. **GIL discipline and safepoints.** All AntValue access happens under the GIL; natives release it only around pure syscalls on copied buffers; `ANT_SAFEPOINT` at every loop back-edge and function entry (otherwise a spinner starves `main`); process exit is `fflush` + `_exit` from the thread holding the GIL. Keep goroutine structs reachable from a global list, since Boehm does not scan `__thread` storage.

Other risks: the operator check order (`kind mismatch` before `not a number`) and the library `wrong kind` rule are fixed by tests, so centralize both in two helpers; shortest-round-trip float rendering (G33/G55) is fiddly, test it against `print-float-format` early; Boehm stop-the-world with hundreds of macOS threads works but set thread stacks explicitly and build with `GC_THREADS`; `Record.same` and friends compare pointers, `==` never does; keep field-name symbols and inline caches so the fully dynamic self-hosted compiler stays fast enough to compile itself.
