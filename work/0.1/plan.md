# Plan for step 0.1 "antpy-core"

Acceptance (verbatim):

    python3 -I src/tests/run.py --run "python3 -I bootstrap/antpy.py run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run --unchecked {file} {args}" --tier run --tier panic --tier unchecked --filter /smoke/ --filter /lexical/ --filter /variables/ --filter /functions/ --filter /expressions/

That selects 86 run tests, 18 panic tests and 20 unchecked tests (plus `src/tests/lib/expect.ant`, which 80 of them import). Nothing exists yet under `bootstrap/` and there is no earlier `work/*/report.md`; this is the first step of the roadmap.

Although the roadmap splits the bootstrap into 0.1–0.6, the 0.1 tests already exercise namespace imports, prelude tags (`Ok`, `Err`, `Error`, `Entry`, `Indexed`), `try`, `defer` with unwinding, `is`, `??`, `?.`, structural `==` with records/arrays/maps, sequences, a `go` block with a `WaitGroup`, and about 45 library functions. Build the core mechanisms properly now (they are listed below); later steps add breadth and edge cases, not new mechanisms.

## 1. Files

| File | Content |
|---|---|
| `bootstrap/antpy.py` | CLI. Must set `sys.path` itself: `python3 -I` adds neither the script directory nor `tools/`. Insert `bootstrap/` (for the package) and `<repo>/tools` (for `antparse`). Parses `run [--unchecked] FILE [ARGS...]`; `--unchecked` is accepted and ignored; everything after FILE is program arguments, never options. Runs the interpreter in a worker thread with `threading.stack_size(512 * 1024 * 1024)` and `sys.setrecursionlimit(1_000_000)`, `sys.setswitchinterval(0.005)`; the main thread joins it. Exit codes: 0 when `main` returns, 2 on a panic in the main goroutine, 1 on a load error (`path:line:col: message` on stderr: parse error, `literal out of range`, `invalid escape`, missing or non-function `main`). Always `sys.stdout.flush()` before writing to stderr and before `os._exit`. |
| `bootstrap/antpy/__init__.py` | Empty, but required: without it `import antpy` under `-I` resolves to `antpy.py` itself and `antpy.core` fails (verified on this machine; with the file, the regular package wins over the sibling module). |
| `bootstrap/antpy/values.py` | Kinds and value classes (section 2). |
| `bootstrap/antpy/arith.py` | Arithmetic, comparison, unary ops, conversions, `Bits` helpers, integer ranges and wrapping (section 5). |
| `bootstrap/antpy/equal.py` | Structural `==` with the pair stack, and the structural hash used by maps (section 6). |
| `bootstrap/antpy/render.py` | `String.from` (section 7). |
| `bootstrap/antpy/lower.py` | One pass over the `antparse` tuple AST producing the evaluator's nodes: decoded literals, split templates, marked defer blocks, optional-chain heads, statement line numbers (section 3). |
| `bootstrap/antpy/interp.py` | Scopes, frames, control-flow signals, panics, calls, loops, sequences, defers, module loading, `go` (section 4). |
| `bootstrap/antpy/prelude.py` | The prelude scope: tags, conversions, `print`, `panic`, and the native namespaces needed now (section 8). |

Keep each file plain: dispatch dictionaries and small functions, no metaprogramming. A later milestone rewrites this logic in Ant and reviewers read all of it.

## 2. Values (`values.py`)

Represent values by Python type so that `type(v)` identifies the kind in one dictionary lookup. Never use `isinstance` for kind tests: Python's `bool` is a subclass of `int`, and the sized kinds below are subclasses of `int`/`float`.

| Ant kind | Python |
|---|---|
| `int` (= `int64`) | `int` |
| `float64` | `float` |
| `int8 int16 int32 uint8 uint16 uint32 uint64` | `class Int8(int): __slots__ = ()` and so on, one class per kind |
| `float32` | `class Float32(float)`; the constructor rounds through `struct.pack("f", x)` / `unpack` (raises `OverflowError` outside the float32 range: that is G7's `conversion out of range`; `1e-50` becomes `0.0`) |
| `bool` | `bool` |
| `string` | `str` (code points are native: `len`, iteration, comparison are all per code point) |
| `null` | `None` |
| `void` | `VOID`, the single instance of `class Void` |
| array | `list` |
| map | `class AntMap`: `buckets: dict[int, list[list]]` from structural hash to `[key, value]` pairs, plus `size`. Methods: `get(k, default)`, `has`, `set` (replace the pair whose key is `==`, else append), `remove`, `items()` (a fresh list, so `for` over a map never sees a mutation). Key hashing is done at insertion (section 6). |
| record | `class Record: __slots__ = ("tag", "fields")`; `fields` is a dict in creation order |
| tag | `class Tag: __slots__ = ("name",)`; compared by identity only |
| func | `Closure` (declared function, anonymous function, lambda), `Native` (library function), `YieldFn` (the `yield` a `for` hands to a sequence). All three are kind `func` for `is func` and rendering. |
| sync | `Mutex`, `WaitGroup` (the others come in 0.4); rendered as their class name |

`KIND: dict[type, str]` maps every class above to its kind name (`"int64"`, `"float64"`, `"int8"`, ..., `"bool"`, `"string"`, `"null"`, `"void"`, `"array"`, `"map"`, `"record"`, `"func"`, `"tag"`, `"sync"`). `INT_KINDS`, `FLOAT_KINDS`, `INT_RANGE[kind] = (lo, hi)`, `INT_WIDTH[kind]`, `INT_CLASS[kind]` (with `int` for `int64`), `NUMBER_CLASS` for the ten conversion names. `int` and `byte` are aliases: `NUMBER_CLASS["int"] is int`, `NUMBER_CLASS["byte"] is UInt8`.

Closure fields: `name` (for traces: the declared name, `"lambda"`, or `"func"`), `params` (list of `(name, default_node_or_None)`), `body` (the lowered block node), `scope` (captured), `is_lambda`, `module` (path for traces). Native fields: `name`, `params` (list of parameter names, needed because named arguments resolve against them: `xs | Array.slice(to: 2, from: 0)`), `fn` (called as `fn(interp, frame, *args)` after argument resolution). YieldFn fields: `body` (a Python callable taking the element), `done` flag.

Sentinels: `UNINIT` (a top-level `var` whose initializer has not run; reading it panics `not initialized`) and `MISSING` (internal "no such key" for dict lookups; never stored).

Module records are `Record` instances whose `fields` dict is the very same object as the module scope's `vars` dict, so a namespace import sees every later assignment for free. Give them a shared `MODULE_TAG = Tag("Module")`; prelude namespaces are records with a tag named after the namespace (`Tag("Array")`), so `Array.map` is an ordinary field read.

## 3. Lowering (`lower.py`)

`lower_file(ast, path) -> ("File", imports, decls)`. Walk the tuples from `antparse` once and return new tuples. Node shapes to produce (the evaluator dispatches on `node[0]`):

- `("Const", value)` for `Int`, `Float`, `String`, `Lit`. `Int`: strip `_`, honour `0x`/`0b`; a value above `2**63 - 1` is a load error `literal out of range` (G1; `-9223372036854775808` is unary minus applied to an out-of-range literal, which is why the tests write `-9223372036854775807 - 1`). `Float`: `float(text.replace("_", ""))`; an infinite result is `literal out of range`. `String`: unescape `\n \t \r \\ \" \` \$ \u{...}`; a `\u` value above `0x10FFFF` or in `D800..DFFF` is `invalid escape` (G2). Load errors raise `LoadError(line, col, message)`; the CLI prints `path:line:col: message` and exits 1 (line/col from the statement being lowered, see below).
- `("Template", parts)`: `parts` is a list of `str` chunks and lowered expression nodes in order. The parser gives the raw text (with backticks) and the expressions; re-scan the raw body: on `\` apply the escape table above, on `${` skip to the matching `}` counting nested braces and skipping `"..."` and `` `...` `` literals inside (mirror `Lexer.embedded` in `antparse.py`) and emit the next expression, otherwise append the character. Newlines inside a template are literal.
- `("Block", stmts, has_defer)`: `has_defer` is true when a `Defer` node occurs anywhere inside the block without an intervening block, lambda, anonymous function, `go` or `defer` body (keep a stack of enclosing blocks while lowering; a `Defer` marks the top of the stack). Only such blocks allocate a defer list at run time.
- `("Func", name, params, body, is_lambda)` for `Func`, `AnonFunc` and `Lambda` (a `Lambda` has the single parameter `x`, no defaults, and its `stmts` become a `Block`). `params` is `[(name, default_node_or_None)]`; type annotations, return types and type parameters are dropped. `("Export", d)` becomes `d`.
- `("Chain", node)` wraps the head of a postfix chain (`Field`, `OptField`, `Index`, `OptIndex`, `Call`, `OptCall`) when any node on its object spine is an `Opt*` node, so `a?.b.c` yields `null` as a whole (section 4). A postfix node in argument or index position starts its own chain.
- `("Pipe", left, callee, args)`: the parser guarantees the right side is a `Call`; split it so the evaluator does not treat it as a call of its own.
- `("Paren", e)` becomes `e`. `Var`, `Assign`, `If`, `While`, `For`, `Is`, `Bin`, `Unary`, `And`, `Or`, `Nullish`, `Return`, `Break`, `Continue`, `Defer`, `Go`, `RecordLit`, `MapLit`, `ArrayLit`, `Ident`, `Field`, `Index`, `Call` and the `Opt*` nodes keep their shape with lowered children. `Type` declarations keep only the name. `Import` paths are unescaped strings.
- Line numbers: `antparse` nodes carry none. Parse with a subclass `class LineParser(antparse.Parser)` whose `statement()` reads `self.peek().line` before calling the base method and returns `("At", line, node)`. The evaluator's `At` handler stores the line in the current frame, then evaluates the node. This gives traces of the form `  at main (path:line)` and gives load errors a line. Nothing in the tests depends on it, so if it causes any trouble drop it and print traces without lines; do not modify `tools/antparse.py`.

The lowering pass is also where the evaluator's hot path is kept small: literals are decoded once, not per evaluation.

## 4. Interpreter (`interp.py`)

### Scopes and frames

`class Scope: __slots__ = ("vars", "parent", "frame", "defers", "imports")`. `vars` is a dict; `frame` is the function activation the scope belongs to; `defers` is `None` or a list; `imports` is `None` except on module scopes (section 4, modules).

`class Frame: __slots__ = ("name", "path", "line", "parent")`. A frame is created per function call (and per deferred block, per goroutine root, per module initialization) with `parent` = the caller's frame; the chain from the current frame upward is the stack trace, so no thread-local state is needed.

Lookup walks `vars` outward; a hit that is `UNINIT` panics `not initialized`; a miss on a module scope consults `imports`; a miss everywhere panics `unbound name` (G3). Assignment to a name finds the scope whose `vars` holds it and rebinds there; none found is `unbound name`. The prelude scope is the outermost scope, so `var print = 1` in a function simply shadows it.

Block scopes are shared dictionaries that closures reference, and `var` binds its name only after evaluating the initializer. This alone gives both G3 (`var x = x + 1` reads the outer `x`) and G29 (`defer { print(y) }; var y = 2` prints `2`, because the deferred block looks `y` up when it runs). Do not hoist inside blocks. Hoisting exists only at module level (below).

### Evaluation

`eval(node, scope)` dispatches through `EVAL = {"Const": ..., ...}`. Statement lists: evaluate in order; the block's value is the last expression's value, `VOID` for an empty block; `Var` and `Assign` evaluate to `VOID`, so "a block ending in a declaration or assignment is void" needs no special case. A `Block` node opens a child scope (same frame); function bodies and `for` bodies are evaluated directly in the scope that holds the parameters or the loop variable (one scope per call / per iteration, not two).

Control flow is Python exceptions: `ReturnSignal(frame, value)`, `BreakSignal`, `ContinueSignal`, `AntPanic(message, trace)`. Panics raised from helpers carry no trace; the `At` handler catches an `AntPanic` whose `trace` is `None`, fills it from `scope.frame` (innermost first, `name` and `path:line` per frame) and re-raises. Every panic message starts with the canonical phrase and may add detail after a colon (`unbound name: missing`, `no field: nope`), because the runner only checks that stderr contains the phrase.

Expressions, in the terms of §12:

- `Ident`: lookup. `Const`: the value. `Template`: concatenate `render(eval(part))` for expression parts, left to right.
- `Bin`: evaluate left then right, then `arith.binary(op, a, b)` for `+ - * / % < <= > >=`, `equal.eq` for `==`/`!=` (never panics). `Unary -`/`!`: `arith.neg`, `arith.not_`. `And`/`Or`: left must be `bool` (`not a bool`), short-circuit, right must be `bool` when evaluated. `Nullish`: evaluate left; if `None`, evaluate right.
- `Unary try`: `None` → `raise ReturnSignal(scope.frame, None)`; record tagged `Err` → `raise ReturnSignal(scope.frame, Record(ERR, {"error": r.fields["error"]}))` (a fresh record); record tagged `Ok` → its `value` field; anything else → the value itself (G28).
- `Is`: if the path is one name in `{"null", "void", "bool", "string", "func", "Array", "Map", "int", "byte", "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float32", "float64"}` it is a kind test decided by name before any scope lookup (`int` means `int64`, `byte` means `uint8`, G48; `func` covers all three function classes). Otherwise evaluate the path (lookup, then field reads); the result must be a `Tag` (`not a tag`); the test is `type(v) is Record and v.tag is tag`.
- `Field a.b`: `a` must be a `Record` (`not a record`), the field must exist (`no field`). `Index a[i]`: list → `i` must be of kind `int64` (`index not an int`; an `Int8` index is not an int) and `0 <= i < len` (`index out of range`); `AntMap` → `get(i, None)`; anything else `not indexable`. `OptField`/`OptIndex`/`OptCall`: evaluate the left operand; if it is `None`, raise the private `OptNull` exception, which the enclosing `Chain` node turns into `None`; otherwise apply the plain rule.
- `Call`: evaluate the callee first, then the arguments in written order, positional and named interleaved exactly as written (`add3(c: ..., a: ..., b: ...)` logs `c,a,b`), then `call(callee, positional, named, frame)` (below). `Pipe`: evaluate `left`, then `callee`, then the call's own arguments, then call with `[left] + positional`.
- `RecordLit`: evaluate the head path (lookup, then field reads); it must be a `Tag` (`not a tag`). Entries in order: a spread operand must be a `Record` (`not a record`) and contributes its fields in its own order; a later entry with the same name replaces the earlier one in place (dict assignment, G52). `MapLit`: keys and values in order; a bare identifier key was already turned into a string by the parser; `(name)` arrives as the identifier and evaluates to the variable; a later `==` key replaces. `ArrayLit`: elements in order; a spread operand must be a `list` (`not an array`).
- `Assign`: evaluate the target's sub-expressions first, then the right side (`xs[i()] = f()` logs `i,f`; `p().x = v()` logs `p,v`). Variable targets rebind in the declaring scope; field targets need a `Record` that already has the field (`not a record` / `no field`); index targets need a list with an `int64` index in range or a map (insert or replace); anything else panics `cannot assign`. Compound assignment evaluates the target sub-expressions once, reads, applies `arith.binary`, stores (G4). Value `VOID`.
- `If`: condition must be `bool` (`not a bool`); no `else` and `false` → `VOID`. `While`: condition checked before each iteration, must be `bool`; body in a fresh scope per iteration; catch `BreakSignal` (leave) and `ContinueSignal` (next test) around the body; value `VOID`.
- `For`: see sequences below. `Return`: evaluate the operand (or `VOID`) and raise `ReturnSignal(scope.frame, value)`. `Break`/`Continue`: raise the signal. `Defer`: append `(block_node, scope)` to the innermost block's defer list (the current `scope.defers`; the lowering pass guarantees it exists); value `VOID`. `Go`: section 4, goroutines. `Func`: `Closure(..., scope=scope)`.
- `Var` with a `Name` target: evaluate, then `scope.vars[name] = value`. With a `Destructure` target: the value must be a `Record` (`not a record`) with every named field (`no field`); bind each field under its alias or name (G3).

### Calls

`call(fn, positional, named, caller_frame)`:

1. `type(fn)` not one of `Closure`, `Native`, `YieldFn` → `not a function`.
2. Lambda (`is_lambda`): any named argument → `named argument to lambda`; more than one positional → `too many arguments`; `x` is the argument or `VOID` when there is none (G23).
3. Declared or anonymous function: more positional than parameters → `too many arguments`; fill left to right; each named argument must name a parameter (`unknown parameter`) that is not yet filled (`parameter already filled`); then, in parameter order, every unfilled parameter evaluates its default in the callee's scope (so `b: int = a * 2` sees `a`, G26, and `xs: Array[int] = []` is a fresh array per call) or panics `missing argument`.
4. Native: resolve named arguments against `fn.params` with the same four phrases, then `fn.fn(interp, frame, *args)`. Natives that call back (`Array.map`, `Array.sortWith`, `Iter.*`, ...) use `interp.call` and let signals and panics propagate as Python exceptions; nothing to clean up because they hold no locks.
5. YieldFn: no positional argument → `missing argument`; a named argument → `unknown parameter`; more than one → `too many arguments`; `done` → `yield after done`.
6. Body: `frame = Frame(fn.name, fn.module, 0, caller_frame)`, `scope = Scope({params...}, fn.scope, frame)`, evaluate the body block's statements in that scope (honouring its `has_defer`). Catch `ReturnSignal` whose `frame is frame` and yield its value; any other signal propagates unchanged. A `return` therefore leaves the function it is written in even when raised inside a `for` body that a sequence function is driving.

### Defers and panics

Evaluating a block whose `has_defer` is set:

    defers = scope.defers = []
    pending = None
    try: value = run the statements
    except (AntPanic, ReturnSignal, BreakSignal, ContinueSignal) as e: pending = e
    for (block, reg_scope) in reversed(defers):
        try: run_deferred(block, reg_scope)
        except AntPanic as p: pending = p        # replaces the pending panic or return (G59); later defers still run
    if pending is not None: raise pending
    return value

`run_deferred` evaluates the block in a child of `reg_scope` with a fresh `Frame("defer", ...)` and swallows a `ReturnSignal` targeting that frame (a `return` leaves the deferred block only; `defer-return-local`). Treat `BreakSignal`/`ContinueSignal` the same way (unspecified, compile error by G24). Note the value of a function body or `return` operand is computed before the defers run (`defer-cannot-change-return`), which the code above does naturally, and that `defer` inside an `if` body registers with that block, not with the function (`rt-unwind-nested` prints `d3,d2,d1,d0`).

Goroutine root (main and `go` blocks): on an escaping `AntPanic`, `sys.stdout.flush()`, write `panic: <message>\n` followed by one `  at <name> (<path>:<line>)` line per frame to stderr, then `os._exit(2)` for the main goroutine or just end the thread. Convert a Python `RecursionError` into `AntPanic("stack overflow")` at the root so that a runaway recursion is still reported as a panic. Never let a Python traceback be the program's only output.

### Loops over values and sequences

`For(target, iterable, body)`:

- list: iterate by index while `i < len(xs)` re-read each time (appends are visited, pops shorten the loop).
- `AntMap`: iterate over `items()` (a snapshot), each element an `Entry` record `{ key, value }` built with the prelude tag.
- `str`: Python iteration (code points, one-character strings).
- function (`Closure`, `Native`, `YieldFn`): create `y = YieldFn(body=run_iteration)` and `call(seq, [y], ...)` inside `try/finally: y.done = True`. `run_iteration(v)` binds the loop variable in a fresh scope (same frame as the enclosing function), runs the body, returns `True`; on `ContinueSignal` returns `True`; on `BreakSignal` sets `y.done = True` and returns `False`. `ReturnSignal` and `AntPanic` propagate through the sequence function's Python frames, which runs the sequence's deferred blocks on the way (`seq-return-runs-seq-defers`, `seq-panic-propagates`). The `done` flag makes an escaped `yield` panic `yield after done` (`seq-yield-after-return`) and so does a second `yield` after `false` (`seq-yield-after-false`).
- anything else: `not iterable`.

Each iteration binds a fresh scope holding the loop variable (closures capture per-iteration values; the variable is assignable, G4). A `Destructure` target reads the named fields of each element (`not a record` / `no field`). The loop's value is `VOID`.

### Modules

`load_module(abs_path)` with a cache keyed by absolute path: read, parse (`LineParser`), lower, create `Scope(vars, prelude_scope, Frame("module", path))` and the module `Record` sharing `vars`, then:

1. Imports, depth first in source order, path resolved relative to the importing file's directory. `Namespace` form: bind the module record. `Names` form: record `imports[local] = (exporter_vars, name)`; lookup dereferences it on every read, so the binding is live. (Cycle detection, write-through semantics and error reporting are 0.5's job; for now a module in the cache is returned as is.)
2. Hoist: bind every top-level `type` name to `Tag(name)` (every declaration, aliases included) and every top-level `func` to a `Closure` over the module scope; set every top-level `var` name (or destructured names) to `UNINIT`.
3. Run `var` initializers in source order.

The entry module's `main` must be a `Closure`; call it with no arguments from `Frame("main", path)`; when it returns, flush stdout and `os._exit(0)`. Keep the program arguments on the interpreter for `Os.args()` later.

### Goroutines (minimum for `braces-disambiguation.ant`)

`Go(block)`: start a `threading.Thread(daemon=True)` that evaluates the block in a child scope with `Frame("goroutine", path)` as its root; apply the goroutine-root panic rule above (report, do not exit). `WaitGroup` is a counter guarded by a `threading.Condition`: `add(n)` (counter below zero → `negative counter`), `done` (zero already → `negative counter`), `wait` (loop on the condition until zero). `Mutex.new()` returns a `Mutex` holding a `threading.Lock` (only identity is tested now). The GIL gives the memory safety the spec asks for; 0.4 completes this.

## 5. Arithmetic (`arith.py`)

`binary(op, a, b)`: let `ka, kb = KIND[type(a)], KIND[type(b)]`. Check order is fixed by the tests: `ka != kb` → `kind mismatch` (`"a" + 1`, `1 < "a"`); then, for `+` and the comparisons, two strings are allowed (`+` concatenates, comparisons use Python's code-point order); then a non-numeric kind → `not a number` (`true + false`, `-"a"`). Then:

- Integer kinds. `+ - *`: exact Python result, then `INT_RANGE` check → `integer overflow` (covers unsigned subtraction below zero). `/`: zero divisor → `division by zero`; truncate toward zero (`q = abs(a) // abs(b)`, negated when signs differ; never Python `//`), then the range check (`INT_MIN / -1` overflows, G5). `%`: zero divisor → `division by zero`; `r = abs(a) % abs(b)` with the sign of `a`; no range check (`INT_MIN % -1` is `0`). Wrap the result in `INT_CLASS[kind]`.
- Float kinds: `+ - *` are Python's (overflow gives `inf`, no exception). `/`: a zero divisor is not an exception: `nan` when `a` is zero or NaN, else `±inf` by the signs of `a` and `b` (`-1.0 / 0.0` is `-inf`, and respect `-0.0`). `%`: `math.fmod(a, b)`; a zero divisor, an infinite dividend or NaN gives `nan` (G6, `-5.5 % 2.0 == -1.5`). Comparisons are Python's (NaN compares false). Wrap in `Float32` when the kind is `float32`.
- `neg(v)`: non-number → `not a number`; integer kinds: range check after negation (`-INT_MIN` and `-uint8(1)` overflow, `-uint8(0)` is fine, G5); floats: `-v`. `not_(v)`: `type(v) is not bool` → `not a bool`.

`convert(kind, v)`: the ten conversion natives. `v` must be numeric (`wrong kind`). Integer target: a float that is NaN or infinite → `conversion out of range`; truncate toward zero (`math.trunc`); range check → `conversion out of range`. `float64` target: `float(v)`. `float32` target: `Float32(float(v))`, mapping `OverflowError` to `conversion out of range`.

`Bits`: operands must be integer kinds and, for the binary functions, of the same kind (`wrong kind`, G22). `wrap(x, kind)`: mask to the width, then subtract `2**width` when a signed value has its top bit set. `and/or/xor/not` are Python's on two's-complement integers followed by `wrap` (`-1 & 5 == 5`, `~uint8(0)` wraps to `255`). `shiftLeft(a, n)`: `n` must be kind `int64` (`wrong kind`), `0 <= n < width` else `shift count`; result `wrap(a << n)` (bits shifted out are dropped, never an overflow, G22). `shiftRight`: same count check; Python's `>>` is arithmetic for negative values and the unsigned kinds are never negative, which is exactly the spec. `popCount`: `bin(a & mask).count("1")` as an `int`. `wrapAdd/Sub/Mul`: `wrap(a op b)`.

## 6. Equality and hashing (`equal.py`)

`eq(a, b, pairs=None)`: never panics. Different kinds (`KIND[type(a)] != KIND[type(b)]`) → `False`, which makes `int8(1) == 1`, `1.0 == 1`, `true == 1` and `x == null` all false without special cases. Same kind: numbers by Python `==` (NaN is unequal to itself, `-0.0 == 0.0`); strings, bools, `None`, `VOID` by `==`/identity; lists by length then elements; records by `tag is` and equal field sets then fields; maps by size then every key of `a` present in `b` with an equal value (use a `MISSING` default so a stored `null` is distinguishable); functions, tags, sync objects by `is`. Keep a set of `(id(a), id(b))` for the container pairs currently under comparison and treat a repeat as equal. No identity short-circuit for containers (G11: `p == p` is false when a field is NaN).

`hash_value(v)` for `AntMap`, consistent with `eq`: numbers `hash((kind, v))` (so `1` and `int8(1)` differ; `-0.0` and `0.0` agree), strings/bools/null/void by a tagged hash, lists by the tuple of element hashes, records by the tag's `id` and a frozenset of `(name, hash)`, maps by an order-independent combination of entry hashes, functions/tags/sync objects by `id`. Guard against cyclic keys with a depth limit (a cyclic key is a program error, not a crash). Bucket lookup compares candidates with `eq`, so a NaN key can be inserted but never found (G13).

## 7. Rendering (`render.py`)

`render(v, top=True, path=())` implements `String.from` as the suite asserts it (the stdlib tests `print-*` and `str-from-kinds` are the contract; 0.3 runs them, but get it right now because `Expect` messages and templates use it):

- `str`: bare at the top level; inside a container quoted, escaping `\\`, `"`, `\n`, `\t`, `\r`. Integer kinds: decimal digits. `bool`: `true`/`false`; `None`: `null`; `VOID`: `void`.
- `float64`: `NaN`, `Infinity`, `-Infinity`; otherwise the shortest round-trip digits (take them from `repr`) laid out with JavaScript's thresholds (G33, G55): positional with at least one fractional digit when `1e-6 <= |x| < 1e21` (`1.0`, `100.0`, `-0.0`, `123456789.0`, `0.30000000000000004`), else `d.ddde-N` / `d.dddeN` with no `+` (`1e21`, `1.5e-7`). Python's `repr` switches to exponent form at `1e16` and below `1e-4`, so the digits must be re-laid out, not copied. `float32`: use the shortest digits that round-trip through `Float32` (`%.{1..9}g`), same layout.
- list: `[a, b]`, `[]`. Map: `Map{ "a": 1 }`, `Map{}` (keys rendered with the inner rule). Record: `Name{ x: 1.0, y: 2.0 }`, `Empty{}` (fields in dict order).
- Functions of all three classes: `func`. Tags: their name. Sync objects: their class name. Module records: `Module`.
- A container that is already on the current render path renders as `...` (G33: `Node{ name: "a", next: ... }`, `Tree{ kids: [...] }`); a value that merely appears twice renders twice.

## 8. Prelude (`prelude.py`)

`make_prelude() -> Scope` with: the tags `Ok`, `Err`, `Error`, `Entry`, `Indexed`; the remaining prelude type names (`Seq`, `Integer`, `Number`, `Ordered`) also bound to tags for completeness; `bool` and `string` bound to tags so that calling them panics `not a function` (G8); the ten conversion natives plus `int` and `byte`; `print(value)` (`render` + newline to `sys.stdout`); `panic(message)` (a non-string argument is `wrong kind`, G67; otherwise raise `AntPanic`). Natives are declared with their parameter names, e.g. `native("slice", ["xs", "from", "to"], fn)`.

Namespaces and functions the 0.1 tests need (others arrive in 0.3; structure the registry so adding one is one line):

- `Array`: `length isEmpty get push pop(null when empty, G34) slice(bounds → index out of range) map filter reduce(f(acc, v)) find some every contains(by eq) sort(new list, stable, compare via arith) sortWith(functools.cmp_to_key over the callback) entries(Indexed records) same(identity)`.
- `Map`: `get has set size`.
- `String`: `length chars join toBytes(list of UInt8 from UTF-8) from`.
- `Iter`: `range(from, to)` returns a `Native` sequence that calls `yield` per element and stops when it returns `false` (re-runnable, `seq-rerun`); `toArray(s)` calls `s` with a collecting `Native` yield.
- `Bits`: `and or xor not shiftLeft shiftRight popCount wrapAdd wrapSub wrapMul`.
- `Result`: `ok err isOk unwrap` (`unwrap on Err`).
- `Record`: `same`.
- `Mutex`: `new`. `WaitGroup`: `new add done wait`.

Argument kind checks in natives use one helper per expected kind (`need_array`, `need_string`, `need_int`, `need_func`, `need_record`) that panics `wrong kind`; a callback result that must be a `bool` (`filter`, `some`, `every`, a sequence's `yield` result) panics `not a bool` otherwise. Library functions never mutate their arguments unless marked `mutates` (`push`, `pop`, `set`); `sort`, `map`, `filter`, `slice`, `entries` return new lists.

## 9. Order of work

1. Skeleton: `values`, `lower` (literals only), `interp` with `Const`/`Ident`/`Var`/`Assign`/`Bin` on `int64`/`Call` of declared functions/`If`/`While`/`Block`, `print`, `panic`, module load with hoisting, `main`, the CLI with exit codes and the panic report. Target: `run/smoke`, `panic/smoke`, `unchecked/smoke`.
2. What `expect.ant` needs: namespace imports, templates, structural `==`, `Array.push`, `String.join`, assignment to a module `var` from a function (`log = []`). From here every test can run; use `--filter` per file.
3. `lexical`: hex/binary/underscore literals, float literals, escapes, `String.toBytes/chars/length`, template escapes and nesting, bare `return`.
4. `variables`: scoping and shadowing, compound assignment including float `%` and string `+=`, `UNINIT`, `var x = x + 1`.
5. `functions`: positional/named/default arguments and their evaluation order, closures, lambdas (`x`, zero arguments), anonymous functions, `Array.map/filter/reduce/sortWith/contains`, maps keyed by functions, `Record.same`/`Array.same`.
6. `expressions`: the operator table, `Bits`, `is`, `??`, `?.` with `Chain`, `try`, pipes (including into conversions and lambda calls, named arguments after the piped value), `for` over all four iterables with destructuring, `break`/`continue` as expressions, sequences with `yield` state, defers and unwinding, `go` + `WaitGroup`, conversions.
7. The `panic` and `unchecked` tiers: audit every phrase against the README table; check that stdout printed before a panic appears (flush order) and the exit status is 2.
8. The full acceptance command; then run it once more with `--consistency` added as a sanity check (not required).

## 10. Spec rules that are easy to get wrong

1. Python `bool` is an `int` and the sized kinds subclass `int`/`float`: dispatch on `type(v)` everywhere; `true + false` is `not a number`, not `1`.
2. Operator check order: `kind mismatch` for different kinds before `not a number` for same-kind non-numbers.
3. Integer `/` truncates toward zero and `%` takes the sign of the dividend; Python's `//` and `%` floor. `INT_MIN / -1` panics `integer overflow`; `INT_MIN % -1` is `0`.
4. Unary minus on an unsigned non-zero value and on `INT_MIN` panics `integer overflow`.
5. Float division and `fmod` by zero raise in Python; produce `inf`/`-inf`/`nan` instead. `1e308 * 10.0` is `inf`, not an error.
6. `==` never panics; values of different kinds are simply unequal; `NaN != NaN`; `-0.0 == 0.0`; no identity shortcut for containers.
7. Call order: callee, then arguments in written order (named ones too), then defaults in parameter order in the callee's scope, re-evaluated on every call.
8. A lambda takes `x`; zero arguments bind `x = void`; a named argument panics `named argument to lambda`; two positional arguments `too many arguments`.
9. `return` and `try` target the lexically enclosing function's frame, not the nearest call boundary: a `return` inside a `for` body passes through the sequence function (running its defers) and a `return` inside a deferred block leaves only that block.
10. `break`/`continue` inside a sequence-driven `for` body are caught by the `yield` object (which is dynamically nearer than the sequence's own `while`), returning `false`/`true`.
11. Deferred blocks belong to the innermost block, run in reverse order, in their registration scope, and a panic in one replaces whatever was pending while the remaining ones still run.
12. `for` over an array re-reads the length every iteration; over a map it iterates a snapshot of `Entry` records; over a string it yields code points; each iteration is a fresh scope and the variable is assignable.
13. Compound assignment evaluates the target's sub-expressions exactly once; plain assignment evaluates the target's sub-expressions before the right side.
14. Top-level `func` and `type` are hoisted before any `var` initializer runs; a `var` is `UNINIT` until its initializer finishes (`not initialized` through a call, `var-toplevel-read-before-init-via-call`). Inside blocks nothing is hoisted and `var` binds after its initializer.
15. `is` with a kind name is decided by the name (`int` = `int64`, `byte` = `uint8`, `func` covers natives and yields); any other path must evaluate to a tag or panic `not a tag`.
16. `Error{ message: "odd" }`, `Ok{ value: 1 }` and `missing is Err` work only if the prelude tags exist and `Result.ok/err` build records with exactly those tags.
17. `a[i]` on an array requires an `int64`: `xs[int8(0)]` is `index not an int`. A map index of any kind is fine.
18. Templates: `\$`, `` \` `` and nested `${...}` with braces and nested templates inside; parts render through `String.from`, so a string part is bare and a record part is `Pair{ a: 1, b: 2 }`.
19. Float rendering always shows `.0` or an exponent, with `1e21` (no `+`) and `1.5e-7`; the suite's `print-float-format` is the reference.
20. Flush stdout before writing `panic: ...` to stderr and before `os._exit`; the runner compares stdout even on panic (`op-int-overflow` prints `before`).
21. `panic(1)` is `wrong kind`; `panic` needs a string.
22. `yield` is an ordinary identifier (parameter, variable, named argument in `param-named-yield`); nothing in the interpreter may key on the word.
23. Natives need parameter names, or `xs | Array.slice(to: 2, from: 0)` cannot resolve.
24. `Array.sort` returns a new array; stable; compares with the operator rules (`kind mismatch` on mixed kinds).
25. `python3 -I`: no script directory on `sys.path`, so the CLI must insert both `bootstrap/` and `tools/`; the package needs `__init__.py`.
26. Deep recursion: without the big-stack thread and the raised recursion limit, recursive Ant programs die with a Python `RecursionError`.

## 11. Risks

- Performance. M1 and M2 run `antc` (a compiler written in Ant) under this interpreter with a 600 s timeout per test. Keep the evaluator's hot path a dictionary dispatch over tuples, decode literals in the lowering pass, and avoid per-expression allocation and `isinstance` chains (one dict per block scope is acceptable). Do not add tracing or logging to the hot path.
- Thread-based goroutines are only sketched here; 0.4 may need to rework `go`, preemption and exit behaviour. Keep the goroutine root logic in one function so it can be changed in one place.
- The statement-line trick subclasses `antparse.Parser`; it must not change the AST shapes the lowering pass expects beyond the `At` wrapper. If `template_expression` or `lambda_` paths produce unwrapped statements, the evaluator must accept statements with and without `At`.
- `float32` rendering and `float32` arithmetic rounding (compute in double, round per operation) are not fixed by the spec; document the choice in the report.
- Unchecked programs can put `break`/`continue` inside a lambda inside a loop (compile error by G24); the signal would escape through the call boundary. Not tested; leave it, but mention it in the report.
- Map keys that are cyclic containers recurse in `hash_value`; the depth guard turns this into a panic rather than a crash.
- Optional chains through a pipe callee (`a | b?.f()`) are untested; the `Chain` wrapper around the callee yields `null` for the whole pipe, which is a reasonable reading of "the rest of the postfix chain is skipped".
