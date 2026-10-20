# Plan for step 2.1 "rt-core": C runtime core, ANF code generator, `antc build`/`run`

Acceptance: `RUN A0 RPU --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /functions/ --filter /expressions/`
(125 tests: 87 run, 18 panic, 20 unchecked; 126 checks). Every test except the three `run/smoke` files and most
panic/unchecked files imports `src/tests/lib/expect.ant`, so imports, module records, structural `==`, records,
arrays, maps, templates and a slice of the library are needed in this step, not in 2.2-2.4. Follow
`docs/runtime-design.md`; this plan fixes the choices it leaves open and the order of work.

## 0. Blocker to resolve first: a process-spawning primitive

The acceptance command runs `antc` under antpy: `python3 -I bootstrap/antpy.py run src/antc/main.ant run FILE ARGS`.
`antc run` must emit C, run clang, and run the built binary. `stdlib.md`'s `Os` has only `args`, `env`, `exit`; `File`
cannot start a process; the bootstrap (`bootstrap/antpy/*.py`) has no spawn native of any kind (verified by grep).
There is no way for Ant code under antpy to execute clang today, and `bootstrap/` is outside this step's directories.
The workflow treats any path outside `runtime/`, `src/antc/`, `Makefile`, `work/2.1/` in `git status` as a red round.

Decision: add one native, `Os.exec`, with these semantics (the C runtime gets the same one in 2.5 for the native `antc`):

```
Os.exec(args: Array[string]): Result[int, Error]
```
Runs the program `args[0]` (looked up on `PATH`) with the remaining elements as its arguments, stdin/stdout/stderr
inherited, flushes this process's stdout and stderr first, waits, and returns `Ok(status)` (`128 + signal` when the
child was killed by a signal); `Err{ error: Error{ message } }` when it cannot be started. `wrong kind` for a
non-array or a non-string element. Blocks only the calling goroutine.

The bootstrap side is this patch to `bootstrap/antpy/system.py` (the only file outside the step's directories):

```python
import subprocess                                   # with the other imports
from antpy.values import VOID, need_int, need_string, need_array, ok, err, error

def os_exec(args):
    """Runs args[0] with the rest as arguments, stdio inherited; Ok(exit status) or Err."""
    items = [need_string(a) for a in list(need_array(args))]
    sys.stdout.flush()
    sys.stderr.flush()
    try:
        code = subprocess.run(items).returncode
    except OSError as e:
        return err(error((e.strerror or str(e)) + ": " + (items[0] if items else "")))
    return ok(code if code >= 0 else 128 - code)

OS = { "args": ..., "env": ..., "exit": ..., "exec": (["args"], os_exec) }
```

Implementer: first check whether `Os.exec` already exists in `bootstrap/antpy/system.py` (the user may have applied the
patch and committed it). If it does, touch nothing under `bootstrap/`. If it does not, apply exactly the patch above,
nothing else in `bootstrap/`, and open `work/2.1/report.md` with a "Deviation: Os.exec in the bootstrap" section
stating why (this section, in one paragraph). The tester will list it as a scope violation; do not revert it, the
step cannot pass without it. Recommended for the user (not an agent): either add `bootstrap/antpy/system.py` to step
2.1's `dirs` in `.claude/workflows/antc-roadmap.js`, or commit the patch by hand before (re)running the step.

Everything else below stays inside `runtime/`, `src/antc/`, `Makefile`, `work/2.1/`.

## 1. Files

Create:
- `runtime/ant.h` – the whole runtime API used by generated code (section 2).
- `runtime/value.c alloc.c symbol.c string.c number.c record.c array.c map.c equal.c render.c func.c unwind.c iter.c result.c sched.c sync.c io.c prelude.c` – as in design §5. `sched.c`/`sync.c` are stubs in this step (section 2.9).
- `runtime/.gitignore` – `obj/` and `libantrt.a` (build outputs must not reach `git add -A`).
- `Makefile` – `runtime` and `clean` targets only (stage targets arrive in 2.5).
- `src/antc/codegen/literals.ant` – decoding of int/float/string/template literal text, C string quoting.
- `src/antc/codegen/modules.ant` – module graph: paths, loading, cycles, top-level name tables, `main` checks.
- `src/antc/codegen/emit.ant` – the C emitter (scopes, blocks, expressions, closures, module inits, `main`).
- `src/antc/codegen/build.ant` – driver: build to C text, write, clang, run.
- `work/2.1/probes/` – hand-written probe programs (optional, for debugging).

Change:
- `src/antc/main.ant` – commands `build [--unchecked] [--emit-c] FILE -o OUT` and `run [--unchecked] FILE [ARGS...]`.
- `src/antc/ast.ant` – no change required (the emitter walks the existing nodes; `Template.text` is split in `literals.ant`).

## 2. Runtime (C11, clang, `runtime/`)

### 2.1 ant.h: representation (design §1, fixed here)

```c
typedef enum { K_INT8, K_INT16, K_INT32, K_INT64, K_UINT8, K_UINT16, K_UINT32, K_UINT64, K_FLOAT32, K_FLOAT64,
               K_BOOL, K_STRING, K_NULL, K_VOID, K_ARRAY, K_MAP, K_RECORD, K_FUNC, K_TAG, K_SYNC,
               K_UNBOUND, K_UNINIT, K_ABSENT } AntKind;
typedef struct { uint32_t kind; union { int64_t i; uint64_t u; double f; float f32; bool b; void *p; } as; } AntValue;
typedef uint32_t Sym;
typedef struct { int n; const Sym *names; } AntShape;                 /* static per literal site, interned when dynamic */
typedef struct { int64_t nbytes, ncps; uint32_t hash; bool ascii; char data[]; } AntString;   /* data NUL-terminated */
typedef struct { const char *name; } AntTag;
typedef struct { AntTag *tag; const AntShape *shape; AntValue fields[]; } AntRecord;
typedef struct { int64_t len, cap; AntValue *data; } AntArray;
typedef struct AntMap AntMap;                                          /* compact dict, map.c */
typedef struct { AntValue v; } AntBox;
typedef struct AntFrame { const struct AntFuncInfo *info; int line; struct AntFrame *parent; } AntFrame;
typedef struct AntFunc AntFunc;
typedef AntValue (*AntNative)(AntFunc *self, int argc, AntValue *argv);
typedef struct AntFuncInfo { const char *name, *file; int line, nparams; const Sym *params; const bool *hasDefault; bool isLambda; } AntFuncInfo;
struct AntFunc { AntNative code; const AntFuncInfo *info; AntFrame *ret; void *data; int nfree; AntBox *free[]; };
typedef struct { int n, cap; AntValue *items; } AntDefers;
typedef struct { enum { U_NONE, U_RETURN, U_PANIC } mode; AntValue value; AntFrame *target; struct AntTrace *trace; } AntUnwind;
typedef struct { AntUnwind unwind; AntFrame *top; /* equality pair stack */ } AntGoroutine;
extern AntGoroutine *ant_g;            /* a plain global pointing at a static main goroutine in this step; __thread in 2.6 */
```
- `AntFunc.ret`: non-NULL only for `for`-body closures: the enclosing Ant function's frame, the target of `return`.
- `AntFunc.data`: native state (yield objects, `Iter.range` bounds); NULL for compiled closures.
- Symbols are numbered **at compile time**: the generated file defines `enum { S_x = 0, ... }` and
  `static const char *const ant_symnames[]`, and `main` calls `ant_symbols_init(n, ant_symnames)` before anything
  else. `ant_sym(name)` returns the seeded id when the name is present and appends otherwise, so the runtime's own
  names (`value`, `error`, `message`, `key`, `index`, `x`, native parameter names) are interned in `ant_prelude_init`.
  Consequence: shapes and `AntFuncInfo`s of generated code are `static const`; the runtime's native infos are
  filled at prelude init. `ant_symbols_init` must run before any `ant_sym` call.
- Constants/macros: `ANT_VOID`, `ANT_NULL`, `ANT_TRUE`, `ANT_FALSE`, `ANT_UNBOUND`, `ANT_UNINIT`, `ANT_ABSENT`,
  `ant_int(i)`, `ant_float(f)`, `ant_bool(b)`, `ant_ptr(kind, p)`; `ANT_SAFEPOINT()` (empty in this step);
  `#define CHK(e, L) ({ AntValue _v = (e); if (ant_g->unwind.mode) goto L; _v; })`.
- `gc.h` is included only by `alloc.c` (`#define GC_THREADS` first); `ant.h` must compile with `-Iruntime` alone.

### 2.2 Entry points generated code uses (declare all in ant.h, one line each)

Frames/unwind (`unwind.c`): `ant_frame_push(&fr, info)`, `ant_frame_pop(&fr)`, `ant_finish(&fr, result)` (consumes a
RETURN whose target is `&fr`, else returns `result`), `ant_set_return(target, v)`, `ant_panic(msg)`,
`ant_panicf(fmt, ...)` (message detail after a colon: `unbound name: missing`, `no field: nope`), `ant_run_defers(&d)`,
`ant_defer_push(&d, closure)`, `ant_report_panic()` (writes `panic: <msg>` + `  at <name> (<file>:<line>)` lines,
innermost first, to stderr, clears the state), `ant_run_main(module, S_main)`.
Values (`value.c`): `ant_truth(v)` (C bool; `not a bool` sets PANIC and returns false), `ant_kind_name(k)`.
Numbers (`number.c`): `ant_add sub mul div mod lt le gt ge`, `ant_neg`, `ant_not`, `ant_convert(K, v)`, Bits natives.
Strings (`string.c`): `ant_str_lit(bytes, n)`, `ant_str_concat`, `ant_str_cmp`, `ant_sb_*` builder (new/append/finish).
Records (`record.c`): `ant_tag_new(name)`, `ant_record_new(tag, shape, values)` (`not a tag` check is `ant_need_tag(v)`),
`ant_record_build(tag, n, names, values, isSpread)` for literals with spread, `ant_field_get(v, S, &cache)`,
`ant_field_set(v, S, val, &cache)`, `ant_field_check(v, S)` (the target check before a right-hand side),
`ant_is_kind(v, K)`, `ant_is_tag(v, tagv)` (`not a tag`), `ant_module_new(shape)` (fields `K_UNINIT`, module tag),
`ant_module_get(m, idx)` (`not initialized`), `ant_module_set(m, idx, v)`.
Arrays/maps: `ant_array_new(cap)`, `ant_array_push`, `ant_array_extend(a, v)` (`not an array`), `ant_index_get(a, i)`,
`ant_index_set(a, i, v)`, `ant_index_check(a, i)`, `ant_map_new`, `ant_map_set/get/has/remove`.
Equality/render: `ant_equal(a, b)` (bool, never panics), `ant_hash(v)`, `ant_render(v)` (an AntValue string).
Functions (`func.c`): `ant_func_new(info, nfree, boxes)`, `ant_box_new(v)`, `ant_box_get(b)` (`unbound name` on
`K_UNBOUND`), `ant_call(f, argc, argv)`, `ant_call_named(f, npos, pos, nnamed, syms, vals)`, `ant_for_each(v, body)`,
`ant_try(v, &fr, &out)` style helper for `try` (see 3.8), `ant_go(closure)` (`sched.c`).
Program: `ant_runtime_init(argc, argv)` (GC_INIT, stdout buffer via `setvbuf` 64 KB, `Os.args` = `argv[1..]`),
`ant_prelude_init()`, `ant_prelude_get("print")` (an `AntValue`; `K_UNBOUND` for an unknown name).

### 2.3 Numbers (`number.c`)

- Binary ops: `a.kind != b.kind` -> `kind mismatch`; then strings allowed for `+` and the four comparisons, otherwise
  the kind must be numeric -> `not a number`. Unary `-`: `not a number`; `!`: `not a bool`.
- Integer `+ - *`: a macro per kind using `__builtin_{add,sub,mul}_overflow` with a result of the kind's C type
  (`int8_t` ... `uint64_t`); overflow -> `integer overflow`. `/` and `%`: divisor zero -> `division by zero`;
  signed: `b == -1` is special-cased (`/` panics `integer overflow` iff `a` is the kind's minimum, `%` is 0) because
  C leaves both undefined (G5). Unary `-`: signed minimum -> overflow; unsigned nonzero -> overflow.
- Floats follow IEEE; `%` is `fmod`/`fmodf`; `float32` ops computed in `float`.
- Conversions: argument must be a number (`wrong kind`); integer targets: NaN/inf -> `conversion out of range`,
  truncate toward zero, range check -> `conversion out of range`; `float32`: finite value whose `(float)` cast is
  infinite -> `conversion out of range` (G7); `int` = `int64`, `byte` = `uint8` (same native values).
- Bits: `a` (and `b`) must be integers of one kind (`wrong kind` otherwise); `n` must be `int`; `shift count` when
  `n < 0 || n >= width`; shifts wrap into the width (shiftRight arithmetic for signed, logical for unsigned);
  `popCount` returns `int`; `wrapAdd/Sub/Mul` wrap two's complement (compute in `uint64_t`, truncate, sign-extend).

### 2.4 Strings (`string.c`)

UTF-8 bytes, `ncps` and `ascii` computed at creation, `hash` lazily (0 = not computed). Code-point index/slice: direct
when ascii, forward scan otherwise. Ordering: `memcmp` then length. `for c in s` yields one code point per step
(string.c exports an iterator `ant_str_next_cp(s, &byteoff)`). Natives this step: `String.length isEmpty concat join
chars toBytes from`. `String.from` is `ant_render`.

### 2.5 Records, tags, modules (`record.c`)

Per design. Field read: `not a record` unless `K_RECORD`; shape scan with a per-site inline cache
(`static const AntShape *c; static int ci;` emitted at each site, passed by pointer); miss -> `no field: <name>`;
a `K_UNINIT` field (module records) -> `not initialized: <name>`. Field write: same, no new fields. Equality
compares tag identity, field count, and each field of `a` by name in `b` (order-insensitive); rendering uses shape
order. Module records carry a private `ant_module_tag` and render as `Module`. Destructuring reads fields through
`ant_field_get`, so `var { nope } = p` / `for { nope } in xs` panic `no field` (G3).

### 2.6 Arrays and maps

`array.c`: growth factor 2; `ant_index_get`: `not indexable` unless array or map; array: `index not an int` unless
`K_INT64`, `index out of range` unless `0 <= i < len`; map: `ant_map_get` (`null` when absent). Natives this step:
`Array.length isEmpty get push pop slice concat map filter reduce find some every contains indexOf sort sortWith
reverse entries range same` (`filled flatten sum sortBy` wait for 2.3). `Array.slice(xs, from, to)`: `index out of
range` when `from > to` or either outside `0 ..= len`. `sort`: every element must be a number or a string
(`wrong kind`), compared with `ant_lt` (`kind mismatch` across kinds); `sortWith(xs, cmp)`: `cmp` must return an
`int` (`wrong kind`). One stable merge sort over an index array with a C callback `(ctx, a, b, &less)` that returns
false when the unwind flag is set, so a panicking comparator stops the sort; reused by `sortBy` in 2.3.
Callback natives (`map filter reduce find some every sortWith`) test `ant_g->unwind.mode` after every `ant_call` and
return `ANT_VOID`. Predicates must return a bool (`not a bool`).
`map.c`: the compact dict of design §1 (insertion-ordered entries + open-addressed index, tombstones on remove,
rebuild when the index is more than half full or tombstones exceed live entries). Hash: numbers by kind + bits
(`-0.0` hashes like `0.0`? no: `-0.0 == 0.0` is true, so hash the value `0.0` for both; NaN hashes but never equals,
G13), strings by content, bool/null/void constants, records by tag address + fields, arrays elementwise, maps by XOR
of entry hashes, func/tag/sync by address; depth capped at 8. Natives this step: `Map.size isEmpty has get set remove
keys values entries same`. Iteration (`for`, `keys`...) walks entries by index below the current length, skipping dead
slots. `Map{ k: v }` literals call `ant_map_set` per entry in order.

### 2.7 Equality and rendering (`equal.c`, `render.c`)

`ant_equal`: structural per §12 with the pair stack on `ant_g` (a pair already under comparison answers true); no
identity shortcut (G11). `ant_render`: strings bare at the top level, quoted inside containers with `\\ \" \n \t \r`
escaped; sized ints as bare digits; floats: `NaN`, `Infinity`, `-Infinity`, `-0.0`, else the shortest digit string
that round-trips (try `%.*e` with 1..17 significant digits and `strtod`; 1..9 and `strtof` for float32) laid out
with JavaScript's rule (G33/G55: let `k` = digit count, `n` = decimal-point position; `k <= n <= 21` ->
digits, zeros, `.0`; `0 < n <= 21` -> point inside; `-6 < n <= 0` -> `0.` + zeros + digits; else `d.ddde<n-1>`, no
`+`); `true/false`, `null`, `void`, `func`, a tag's name, a sync object's namespace name (`Mutex`), `Module`;
`Tag{ a: 1, b: 2 }` / `Tag{}`; `[1, 2]`; `Map{ k: v }` / `Map{}`; a container already being rendered prints `...`.
Copy the bootstrap's `render.py` `layout()` exactly; test by hand against `src/tests/run/stdlib/print-float-format.ant`
early even though it is a 2.3 test.

### 2.8 Functions, calls, sequences (`func.c`)

- `ant_call(f, argc, argv)`: `not a function` unless `K_FUNC`; lambda (`info->isLambda`): argc 0 -> `x = void`
  (G23), argc 1 -> `x`, more -> `too many arguments`; otherwise argc > nparams -> `too many arguments`; slots
  `AntValue slots[nparams]` (VLA) filled by position, the rest `K_ABSENT`; an absent slot whose `hasDefault` is false
  -> `missing argument: <name>`; then `f->code(f, nparams, slots)`.
- `ant_call_named(f, npos, pos, nnamed, syms, vals)`: lambda -> `named argument to lambda`; `too many arguments`;
  each named: no such parameter -> `unknown parameter: <name>`; already filled (by position or an earlier name) ->
  `parameter already filled: <name>`; then the missing-argument rule. Works through any function value (G14).
- Natives are `AntFunc`s with `AntFuncInfo`s carrying §11 parameter names (`xs`, `from`, `to`, `f`, `init`, `cmp`,
  `m`, `k`, `v`, `s`, `parts`, `sep`, `value`, `message`, `a`, `b`, `n`, `r`, `fallback`, `wg`, ...), so
  `xs | Array.slice(to: 2, from: 0)` resolves. The prelude's namespace records (`Array`, `Map`, ...) are records
  with a shape of their function names; `print`, `panic`, the twelve conversion names, the tags `Ok Err Error Entry
  Indexed bool string Seq Integer Number Ordered never`, and `Result` (a namespace, not a tag) are prelude globals
  read through `ant_prelude_get`.
- Yield objects: an `AntFunc` with `code = ant_yield_code`, `info = &ant_yield_info` (one parameter `x`), `data` ->
  `{ AntValue body; bool done; }`. `ant_yield_code`: `done` -> `yield after done`; call `body(x)`; unwind pending ->
  return VOID; body returned false -> `done = true`; return the bool. `ant_for_each(v, body)`: array by index below
  the live length; map by entry index (fresh `Entry{ key, value }` records); string by code point; `K_FUNC`: make
  the yield object, `ant_call(seq, 1, &yield)`, set `done` whether or not the call unwound, propagate; else
  `not iterable`. A body returning false stops array/map/string loops.
- `Iter.range(from, to)`: a native `AntFunc` whose `data` holds the bounds, one parameter `yield`; the loop is
  `r = ant_call(yield, 1, &i); if (unwind) return VOID; if (!ant_truth(r)) return VOID;`. `Iter.toArray(s)` drives
  with its own yield object. `Iter.fromArray` is cheap; `map/filter/take` wait for 2.3.

### 2.9 Program root, goroutine stub, sync stubs, I/O

- `ant_runtime_init`: `GC_INIT()`, stdout buffer, args array. `ant_run_main(M, S_main)`: if an init panicked, report
  and exit 2; else `ant_call(main, 0, NULL)` under a root frame; pending PANIC -> `ant_report_panic`; then
  `fflush(stdout); fflush(stderr); _exit(2 or 0)`. Run `main` on a pthread with a 512 MB stack (`GC_THREADS` is
  defined, so `gc.h`'s redirect registers it) and join; if that causes any Boehm trouble, run on the main thread and
  note it for 2.6.
- `sched.c` (stub, replaced in 2.6): `ant_go(closure)` runs the closure synchronously under a frame named
  `goroutine`; a pending PANIC is reported and cleared so the caller continues; `ANT_SAFEPOINT()` is empty.
  `sync.c` (stub): `Mutex.new` (a `K_SYNC` object, renders `Mutex`, identity equality), `WaitGroup.new/add/done/wait`
  with the counter rules (`negative counter`); `wait` with a nonzero counter panics `deadlock: WaitGroup.wait with
  no other goroutines (step 2.6)`. The 2.1 tests need only `braces-disambiguation` (`go { WaitGroup.done(wg) }`).
- `io.c`: `print` (render + `\n` to the buffered stdout), `Os.args`, `Os.env`, `Os.exit(n)` (flush, `_exit(n & 0xFF)`,
  `wrong kind` for a non-int). `File`, `Time`, `Json`, `Os.exec` in C wait for 2.5/2.7.
- `result.c`: prelude tags; `Result.ok err isOk unwrap unwrapOr` (`wrong kind` unless an `Ok`/`Err` record,
  `unwrap on Err`, `no field: value` for a payload-less record); `Record.same` (pointer identity, `wrong kind`
  for non-records). `Result.map/mapErr` can wait for 2.2.

### 2.10 Makefile and build outputs

```
CC ?= clang
CFLAGS = -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -DGC_THREADS -I/opt/homebrew/include
runtime: runtime/libantrt.a          # runtime/obj/%.o from runtime/%.c (depends on runtime/ant.h), ar rcs
clean:
```
`runtime/.gitignore` holds `obj/` and `libantrt.a`. Run `make runtime` after every runtime change and before every
acceptance run (antc does not rebuild the archive).

## 3. Code generator (`src/antc/codegen/`, Ant, runs under antpy)

### 3.1 Driver (`build.ant`, `main.ant`)

- `build [--unchecked] [--emit-c] FILE -o OUT`: load the module graph from FILE, emit one C file; with `--emit-c`
  write it to OUT and stop; else write it to `<tmp>/antc-<Time.now()>-<Time.monotonic()>-<basename>.c` and run
  `Os.exec([cc, "-O1", "-std=gnu11", "-w", "-o", OUT, cfile, "-I" + rt, rt + "/libantrt.a", "-L/opt/homebrew/lib",
  "-lgc", "-lpthread"])` where `cc = Os.env("ANT_CC") ?? "clang"`, `rt = Os.env("ANT_RUNTIME") ?? "runtime"`
  (relative to the working directory: every command runs from the repository root), `<tmp> = Os.env("TMPDIR") ??
  "/tmp"`. Missing `rt/libantrt.a` -> diagnostic `runtime library not built: run make runtime` (exit 1). A nonzero
  clang status -> diagnostic naming the kept C file (exit 1); on success remove the C file.
- `run [--unchecked] FILE [ARGS...]`: build to `<tmp>/antc-...-bin`, `Os.exec([bin, ...ARGS])`, remove the binary,
  `Os.exit(status)`. `--unchecked` is accepted and ignored (no checker before M3). Everything after FILE is a
  program argument, flags included.
- Diagnostics stay `path:line:col: message`, exit 1: parse errors (existing), `not found: <path>` at the import line
  (G32), `cycle: a.ant -> b.ant -> a.ant` at the first import of the cycle (basenames), `main: missing or not a
  function` at the entry module's last declaration (G53), `main: takes no parameters`, `literal out of range` (G1),
  `invalid escape` (G2: `\u{...}` surrogate or above `10FFFF`), `return outside a function` for a top-level
  `return` (G24). Exit 2 for a usage error.

### 3.2 Modules (`modules.ant`)

`Loaded = { path: string, index: int, ast: Ast.Module, imports: Array[{ path, line, col, alias|names }],
names: Array[string] (top-level var/func/type names, destructured var names included, in declaration order),
isFunc/isType per name }`. Load depth first in source order from the entry (absolute path as given); resolve an
import path against the importer's directory and normalize (`split on "/"`, drop `.`, fold `..`); one `Loaded` per
normalized path; a module on the loading stack -> cycle diagnostic. Emission order = post-order (dependencies first);
the entry module is last. The emitter needs, for `import "./x.ant" X`, the imported module's index, and for
`import { a: b }` the exporter's field index of `a` (unknown name -> the use site panics `unbound name`, G3).

### 3.3 Literals (`literals.ant`)

- Int text: strip `_`, `0x`/`0b` prefixes, accumulate with an overflow guard (`acc > (MAX - d) / base` before
  multiplying, since Ant's own arithmetic panics); `> 2^63-1` -> `literal out of range`. Emit `ant_int(INT64_C(n))`.
- Float text: strip `_`, `String.toFloat`; `null` -> `literal out of range`. Emit `ant_float(<String.from(f)>)`
  (shortest round-trip text is a valid C double constant; append nothing; `1e21` and `0.1` are fine).
- String text (raw, quotes included): decode `\n \t \r \\ \" \` \$ \u{hex}` (UTF-8 encode the code point, 1-4
  bytes). Template text (raw, backticks included): walk the characters; `\` -> decode an escape (so `\${` is a
  literal `${`); `${` -> close the current chunk and skip `2 + String.length(parts[i].text) + 1` characters
  (the part's raw body and its `}`), `i += 1`; else append. Result: `n+1` literal chunks around `n` parts.
- C quoting: emit every byte outside `0x20..0x7E` and `"`, `\`, `?` as a three-digit octal escape (never `\x`,
  which swallows following hex digits); the literal's byte length is passed explicitly (`\u{0}` is legal).
  Each distinct literal is a `static AntValue L_<n>` set once in `init_literals()` via `ant_str_lit(bytes, n)`.

### 3.4 Emitter structure (`emit.ant`)

Output is assembled in sections (arrays of lines joined once at the end): includes and the symbol enum/table;
literal and shape statics; `AntFuncInfo`s and C prototypes for every function; function definitions; per-module
`M_k` statics and `init_k`; `main`. The emitter keeps:
- `Fn` (one per emitted C function): C name, kind (`decl`, `anon`, `lambda`, `defer`, `go`, `forbody`, `init`),
  temp counter (temps `t<n>` are declared together in the prologue, so `goto` never crosses a declaration),
  lines buffer, exit-label stack, `retTarget` C expression (`&fr` for every kind except `forbody`, which uses
  `self->ret`), `lineTarget` (`fr.line` or `self->ret->line`), free-variable list (name -> index into `self->free`),
  parent `Fn`.
- `Scope` (one per Ant block, parameters included in the function's root scope): names declared in the block (all
  `var` targets, pre-scanned before emitting the block), each with `{ cexpr, boxed, active }`; `exitLabel`;
  `defers` C name or none; `isLoopBody`; owning `Fn`.
- Capture pre-pass, run per function body before emitting it: collect every identifier referenced inside any
  nested function body (lambda, anonymous `func`, `go`, `defer`, `for` body) of that function. A local, parameter
  or loop variable is **boxed** iff its name is in that set (name-based over-approximation is fine; never box less).

Name resolution for an identifier, walking scopes inward to outward:
1. A scope of the current `Fn`: the binding counts only if `active` (its `var` statement has been emitted), which
   gives G3 (`var x = x + 1` sees the outer `x`; code before a later `var` sees the outer binding).
2. A scope of an enclosing `Fn` (the lookup crossed a function boundary): the binding counts whether or not active
   (hoisting: G29 `defer { print(y) }; var y = 2`), it must be boxed, and it is routed through `self->free[k]` of
   every `Fn` between (add it to each free list as the lookup returns).
3. The module's top-level names: `ant_module_get(M_k, idx)` for `var` names (`not initialized` check inside), the
   plain field for `func`/`type` names.
4. Import bindings: a namespace alias -> `M_j`; a destructured name -> `ant_module_get(M_j, idx)`.
5. Otherwise a prelude global: `static AntValue G_<name>` filled in `init_prelude_refs()` by
   `ant_prelude_get("<name>")`; the use site emits `if (G_x.kind == K_UNBOUND) ant_panic("unbound name: x")` and
   a `CHK`. This is what `print(missing)` compiles to; it panics at run time after the preceding statements ran.

Reads: an unboxed local is the C variable; a boxed local in its own function is `b_x->v`; through a closure
`self->free[k]->v`; closure reads go through `ant_box_get` (the `unbound name` check for `K_UNBOUND`). Writes
mirror reads (`cannot assign` is a checker rule; unchecked, assignment to a prelude name panics `unbound name`,
assignment to an import writes through to the exporter's field, G31).

### 3.5 Blocks, exits, control flow

Every Ant block is a C compound statement:
```c
{ AntValue v_x = ANT_UNBOUND; AntBox *b_y = ant_box_new(ANT_UNBOUND);     /* all vars of the block, at entry */
  AntDefers d3 = {0};                                                        /* only if the block has a direct defer */
  fr.line = 12; t5 = CHK(..., b3_exit); ...
  <result temp> = <last expression>;                                         /* when the block's value is used */
b3_exit: ;
  ant_run_defers(&d3);                                                       /* only with a defer list */
  if (ant_g->unwind.mode || pend) goto <parent exit>;                        /* cascade; see loop bodies below */
}
```
- Block value: the last statement's value; `ANT_VOID` when the block is empty or ends in a `var` or an assignment
  (assignments are void-valued expressions anyway).
- `pend` is one `int` per `Fn` (0, 1 = break, 2 = continue). `break` -> `pend = 1; goto <innermost exit>`;
  `continue` -> `pend = 2; goto ...`. A loop body block's exit replaces the cascade line with: `if (unwind) goto
  <parent exit>; if (pend == 1) { pend = 0; break; } pend = 0;` (the C `break` leaves the `for (;;)` the body sits
  in). A `pend` still set at a function's root exit panics `break or continue outside a loop` (G24, unchecked only).
- `while c { B }`: `for (;;) { ANT_SAFEPOINT(); <cond into tC>; c = ant_truth(tC); if (unwind) goto <parent exit>;
  if (!c) break; { body as a loop body } }` followed by `wN_end: ;`. Value `ANT_VOID`.
- `return e`: evaluate `e` (or `ANT_VOID`) first, `ant_set_return(<retTarget>, v); goto <innermost exit>`. The
  expression's own value is an unused temp. `return` inside a lambda / anonymous `func` / `defer` / `go` body targets
  that body's own frame (G24); inside a `for` body it targets the enclosing function's frame via `self->ret`.
- Function shape (every kind except `forbody` and `init`):
```c
static AntValue F_2_clamp(AntFunc *self, int argc, AntValue *argv) {
  AntFrame fr; ant_frame_push(&fr, &FI_2_clamp); ANT_SAFEPOINT();
  AntValue t0, t1, ...; int pend = 0; AntValue result = ANT_VOID;
  AntValue v_value = argv[0]; AntBox *b_k = ant_box_new(argv[1]);          /* parameters: boxed when captured */
  if (v_b.kind == K_ABSENT) { fr.line = N; <default expr -> tD, CHK -> f_exit>; v_b = tD; }   /* in order, G26 */
  { <root block, result = last value> b0_exit: ... }
f_exit: ant_frame_pop(&fr); return ant_finish(&fr, result);
}
```
  Frame names: the declared name; `func` for anonymous functions; `lambda`; `defer`; `goroutine`; `module` for
  `init_k`. `fr.line = N` before every statement and before each call argument list that spans lines is not needed:
  one update per statement, using the statement node's `line`.
- `for` body (`forbody`): `F_for_<n>(self, argc, argv)`: no frame push; `argv[0]` bound to the loop variable (boxed
  when captured) or destructured through `ant_field_get`; the body is a loop body block whose exit sets
  `result = ANT_FALSE` when `pend == 1`; returns `result` (`ANT_TRUE` otherwise). Line updates write
  `self->ret->line`. Creation at the loop site: `tB = ant_func_new(&FI_for_n, nfree, (AntBox*[]){...}); tB.ret =
  <retTarget of the enclosing Fn, i.e. &fr or self->ret>; r = CHK(ant_for_each(tS, tB), ...)`. Nested `for` bodies
  pass `self->ret` through.
- `defer { B }` -> `ant_defer_push(&dN, <closure of kind defer>)`; `go { B }` -> `ant_go(<closure of kind go>)`.
  Both are zero-parameter closures created like lambdas.
- `if`: `tC`, `ant_truth` + unwind check, branches as blocks writing the shared result temp; no `else` and false ->
  `ANT_VOID`. `else if` chains nest naturally.

### 3.6 Expressions (ANF)

`emitExpr(e): string` appends statements and returns a C expression that is either a literal constant or a temp;
every subexpression with a side effect or a variable read is materialized in a temp **before** the next
subexpression is emitted, which fixes evaluation order (callee, then arguments left to right; `a | f(b)`: `a`, `f`,
`b`; record/array/map entries in order; assignment target sub-expressions before the right side).
- Call: `tF = CHK(callee); t1..tn = CHK(args); r = CHK(ant_call(tF, n, (AntValue[]){t1..tn}))` (`(AntValue[]){}`
  needs a non-empty list; emit `NULL, 0` for no arguments). Named: `ant_call_named(tF, npos, pos, nnamed,
  (const Sym[]){S_a, ...}, (AntValue[]){...})`. Pipe: the call with the left value prepended to the positional list,
  evaluated before the callee. `f?.(x)`, `a?.b`, `a?.[i]`: one skip label per postfix chain (the maximal spine of
  Field/OptField/Index/OptIndex/Call/OptCall), `if (t.kind == K_NULL) { tR = ANT_NULL; goto skip_n; }` at each
  optional step, the rest of the spine not evaluated.
- Field read: `r = CHK(ant_field_get(tO, S_name, &cache_n))` with `static const AntShape *cs_n; static int ci_n;`
  declared next to the site (file scope statics, one pair per site, passed as `&cs_n, &ci_n` or one struct).
  Index read: `ant_index_get`. Assignment: evaluate target sub-expressions, then `ant_field_check` /
  `ant_index_check` (the bootstrap validates the target before the right side), then the right side, then the store
  (which re-checks the range: the right side may shrink the array). Compound: target sub-expressions once, read,
  operator, store (G4). Variable targets: as in 3.4.
- Binary operators: `ant_add` ... ; `==`/`!=`: `ant_bool(ant_equal(a, b))`, never panics. `&&`/`||`: left value,
  `ant_truth` + unwind check, short-circuit with the right side as a nested statement list; `??`: `t = a; if
  (t.kind == K_NULL) { t = b; }` where `b` may be `return`/`break`/`continue`/`panic(...)` (they just emit their
  cascade). `is`: a single identifier in `null void bool string func Array Map int8 ... uint64 float32 float64 int
  byte` -> `ant_is_kind(t, K_...)` (`int` -> `K_INT64`, `byte` -> `K_UINT8`, G48; regardless of bindings); any other
  path -> evaluate it (a prelude/module/local lookup followed by field reads) -> `CHK(ant_is_tag(t, tag))`.
  Unary: `ant_neg`, `ant_not`, `try` (3.8).
- Literals: `L_n` statics; `true/false/null/void` constants. Template: `sb = ant_sb_new(); ant_sb_append_str(sb,
  L_chunk0); t = CHK(part0); ant_sb_append(sb, ant_render(t)); ...; r = ant_sb_finish(sb)` (rendering never panics).
  Record literal: head path evaluated like any path, `ant_need_tag` (`not a tag`), entries in order into temps,
  then `ant_record_new(tag, &SH_n, (AntValue[]){...})` with a static shape (duplicate keys: keep the first
  position, use the last value, G10); any spread entry switches the site to `ant_record_build` with a name/value/
  isSpread vector (`not a record` for a non-record spread). Map literal: `ant_map_new` + `ant_map_set` per entry.
  Array literal: `ant_array_new(n)` + `push`/`ant_array_extend` (`not an array`).
- Lambda / anonymous `func`: emit the inner `Fn` first (into its own buffer; its free list is complete when its body
  is done), then at the site `t = ant_func_new(&FI_lam_n, nfree, (AntBox*[]){<box exprs in free-list order>})`.
  A lambda's `AntFuncInfo` has one parameter `x`, `isLambda = true`.
- Block as an expression (`if` branches, a lambda body): the block's result temp, per 3.5.

### 3.7 Modules in C

Per module `k` (emission order: dependencies first): tags `static AntValue T_k_<name>`; function statics
`static AntValue M_k; static int M_k_state;` (0 new, 1 running, 2 done); the module shape `SH_M_k` over all top-level
names (declaration order; this is the module record's field order); `init_k()`:
```c
if (M_k_state) return; M_k_state = 1;
init_j(); if (ant_g->unwind.mode) return; ...            /* imports in source order; depth first, once */
M_k = ant_module_new(&SH_M_k);                            /* every field K_UNINIT */
ant_module_set(M_k, i, ant_tag_new("Point")); ... ant_module_set(M_k, j, ant_func_new(&FI_k_f, 0, NULL)); ...
AntFrame fr; ant_frame_push(&fr, &FI_k_module); { ...var initializers in order as statements: fr.line = N;
t = CHK(expr, m_exit); ant_module_set(M_k, idx, t) (or the destructured fields)... }
m_exit: ant_frame_pop(&fr); M_k_state = 2;
```
`init_k` is emitted through the same `Fn` machinery (kind `init`: temps, `pend`, blocks, lambdas inside initializers
all work). A panic during an initializer leaves PANIC pending; `main` reports it (var-toplevel-read-before-init-via-call).
Generated `main`: `ant_runtime_init(argc, argv); ant_symbols_init(N, ant_symnames); ant_prelude_init();
init_literals(); init_prelude_refs(); init_<entry>(); return ant_run_main(M_entry, S_main);`.
C names: `F_k_<name>`, `FI_k_<name>`, `F_lam_n`, `F_for_n`, `F_defer_n`, `F_go_n`, `L_n`, `SH_n`, `S_<name>`,
`G_<name>`, `v_<name>_<n>` / `b_<name>_<n>` for locals (a per-`Fn` counter avoids shadowing surprises).

### 3.8 `try` and panics

`try e`: `t = CHK(e); if (t.kind == K_NULL) { ant_set_return(ret, ANT_NULL); goto exit; } if (ant_is_err(t)) {
ant_set_return(ret, ant_err_rewrap(t)); goto exit; } if (ant_is_ok(t)) t = CHK(ant_field_get(t, S_value, ...));`
(`no field: value` for a payload-less `Ok`; a fresh `Err{ error }` with the same error, `no field: error` if absent);
anything else is yielded unchanged (G28). `panic(msg)` is an ordinary call of the prelude native, which checks
`wrong kind` for a non-string (G67), sets PANIC with the trace snapshot, and returns; the `CHK` after it starts the
cascade. `ant_run_defers`: pop in reverse; save and clear the pending unwind; call the closure; a new PANIC replaces
the saved state (dropping a saved RETURN or PANIC and its trace); otherwise restore; keep going (G59).

## 4. Order of work (each step ends with the named subset green)

1. `runtime/ant.h`, `alloc.c value.c symbol.c string.c number.c unwind.c func.c io.c prelude.c` with `print`,
   `panic`, conversions, arithmetic; `Makefile runtime`; a hand-written `work/2.1/probes/hello.c` that calls the API
   like generated code would (push a frame, print, panic, defer) to validate the protocol before the emitter exists.
2. `modules.ant` (single module), `literals.ant`, `emit.ant` for: functions, calls, locals, literals, `if`, `while`,
   arithmetic/comparison, `print`, `panic`; `build.ant` + `main.ant` commands. Subset: `--filter /smoke/`
   (5 tests: `run/smoke`, `panic/smoke`, `unchecked/smoke`).
3. Imports and module records, hoisting, top-level vars, `Expect`; records/tags/field access/destructuring;
   arrays and `==` (`equal.c`, `array.c`, `record.c`, `render.c` with templates); closures and boxes; lambdas;
   defaults and named arguments; `Array.push length isEmpty reduce sortWith map filter contains same`, `Map` basics,
   `Record.same`, `String.join`, `Mutex.new`. Subset: `--filter /variables/ --filter /functions/`.
4. `for` over arrays/maps/strings/sequences with yield objects; `break/continue/return` through bodies and
   sequences; `defer`; `??`, `?.`, `is`, `try`; `Bits`; `Iter.range/toArray`; `Array.sort/slice/entries/some/every/pop`;
   `String.length/chars/toBytes/from`; `Result.*`; `WaitGroup`/`go` stubs. Subset: `--filter /expressions/
   --filter /lexical/`.
5. Audit the panic and unchecked phrases against the README table (`grep -rn "panic:" src/tests/{panic,unchecked}/
   {smoke,variables,functions,expressions}`), run the full acceptance, then the same command with `--consistency`
   as a self-check (not required). Write `work/2.1/report.md`.

Expected cost per test under antpy: lex+parse of `expect.ant` and the test (~0.2 s), emission (< 1 s), clang -O1
(~0.5 s), the run; the suite is sequential, about 5 minutes. Run it in the background with the log in
`work/2.1/test.log` when in doubt.

## 5. Spec rules easy to get wrong (all exercised by the selected tests)

- Operator check order: `kind mismatch` before `not a number` (`"a" + 1`, `1 < "a"`); `true + true` is `not a
  number`; `-"a"` is `not a number`; `!1`, `1 && true`, `if null`, `while 1` are `not a bool`.
- Call checks happen after the callee evaluates and before the arguments: `n()` with `n = 1` panics `not a function`
  without evaluating arguments; argument evaluation order includes named arguments in written order.
- Defaults evaluate per call, after all arguments, in parameter order, in the callee's scope (`b: int = a * 2`,
  `xs: Array[int] = []` gives a fresh array each call, `b: int = base` reads the module var at call time).
- A lambda called with no argument has `x == void`; a named argument to a lambda panics; `5 | { x + 1 }()`.
- `var x = x + 1` reads the outer `x`; a `var` in an inner block shadows and does not leak; a `while` body's `var`
  is fresh per iteration while the counter outside is shared (boxes allocated at block entry); `for` variables are
  fresh per iteration and assignable (`i = i * 10`).
- `for` over an array re-reads the length each step (append visited, pop shortens); over a string yields code points;
  over a map yields `Entry{ key, value }`; a `break` in the body makes `yield` return `false` and the sequence must
  then return; `continue` returns `true`; `return v` in the body unwinds through the sequence running the sequence's
  deferred blocks and leaves the enclosing function with `v`; a stashed `yield` called after the loop panics
  `yield after done`; a sequence (`Iter.range`) may be run again.
- Compound assignment evaluates the target sub-expressions once; `xs[i()] = f()` runs `i` before `f`;
  `p().x = v()` runs `p` before `v`; `(x = 1) == void`.
- `return` takes the whole expression to its right (`x ?? return y + 1`); a parenthesized `return` ends earlier.
- `??`, `&&`, `||` short-circuit: the right side is not evaluated otherwise, and a `panic(...)` there must not fire.
- `==` is structural for records (same tag + same fields), arrays, maps; functions, tags, sync objects by identity;
  two lambdas with the same text are different values and distinct map keys; `x == null` is false for a non-null.
- `INT_MIN / -1` panics, `INT_MIN % -1` is 0, `-INT_MIN` panics, `-uint8(1)` panics, `uint8(0) - uint8(1)` panics,
  `int8(-128) - int8(1)` panics, `x += 1` at the maximum panics; results keep the operands' kind.
- `Bits.shiftLeft(1, 63)` is `INT_MIN` (no panic); `shiftLeft(int8(1), 8)` and `shiftRight(1, -1)` panic `shift
  count`; `popCount` returns `int`; mixed kinds are `wrong kind`.
- Templates: `\$`, `\``, `\${` are literal; parts render with `String.from` (a record renders `Pair{ a: 1, b: 2 }`,
  a bool `true`); a newline inside a template is part of the string. Strings: `"\u{1F600}"` has length 1;
  `String.toBytes` gives UTF-8 bytes as `uint8` values.
- Top-level: `func` and `type` are hoisted before any `var` initializer runs; initializers run in source order;
  a `var` read before its initializer (through a call) panics `not initialized`; `import` of the same file through
  two paths initializes it once.
- `print("before")` must reach stdout before a later panic's `_exit(2)`: flush in the root.
- `x is y` with `y` a non-tag panics `not a tag`; `is int` / `is int64` test `K_INT64`; `is func`, `is Array`,
  `is Map`, `is null` test kinds; a user tag test is `K_RECORD && tag == T`.
- Unbound names are run-time panics at the use site, not compile errors (`print("start")` runs first).

## 6. Risks

1. **The spawn primitive (section 0).** Without `Os.exec` in the bootstrap the acceptance command cannot pass; with
   it, the tester reports a scope violation every round until a human widens the step's directories or commits the
   patch. Flagged first in the report; nothing else can be done inside the allowed directories.
2. **A missed unwind check.** Every runtime call that can panic and every `ant_call` must be followed by `CHK`
   (generated code) or an `if (ant_g->unwind.mode) return ANT_VOID;` (natives). Make `emitExpr` the only place that
   emits calls, and `grep -n ant_call runtime/*.c` before finishing.
3. **Scoping/boxing.** The textual-activation rule (3.4) plus boxing of every captured name is what yields G3, G29,
   fresh `while` locals, per-iteration `for` variables and closures seeing later assignments. Boxing too little
   breaks closures silently; boxing more only costs an allocation.
4. **C scoping of `goto`.** Hoist temps to the function prologue and declare block locals at block entry so no
   `goto` jumps over an initialization; compile the generated code with `-w` (unused labels/variables).
5. **Evaluation order under ANF.** Materialize every variable read and call result into a temp before emitting the
   next subexpression; `(AntValue[]){...}` initializers must only contain temps and constants.
6. **antpy speed of the emitter.** Build output as arrays of lines joined once; keep scopes as `Map`s; avoid
   quadratic string concatenation. Per-test time above ~5 s means something is quadratic.
7. **Signed overflow UB in C.** Only `__builtin_*_overflow` with typed results and the explicit `-1` divisor cases;
   wrap helpers compute in `uint64_t`.
8. **Float rendering.** Shortest round-trip via `%.*e` + `strtod`, laid out as in `render.py`; validate against
   `run/stdlib/print-float-format.ant` by hand.
9. **Symbols seeded at compile time.** `ant_symbols_init` before `ant_prelude_init`; nothing in the runtime may
   intern a name in a static initializer; `ant_sym` of a seeded name must return the seeded id.
10. **Stack depth.** Generated frames are large (many temps); run `main` on a big-stack thread, or note it for 2.6.
11. **Build outputs in git.** `runtime/.gitignore` must exist before the first `make runtime`, or `git add -A` at
    commit time picks up `obj/` and `libantrt.a`.
12. **clang on PATH is Homebrew's LLVM 22** on this machine; both it and `/usr/bin/clang` compile and link the
    Boehm test program; `ANT_CC` overrides. `-lgc` links the dylib from `/opt/homebrew/lib`.
