# antpy: the bootstrap interpreter

`antpy` is the M0 deliverable of `ROADMAP.md`: a tree-walking interpreter for
Ant in Python, with no checker. It exists to run `antc` (the compiler written
in Ant, M1 and M2) once, and to shake out wrong test expectations cheaply. It
implements section 12 of `ant-lang.md` and the library of `stdlib.md` over the
AST of the reference parser, `tools/antparse.py`, and it passes every tier of
`src/tests` except `check`.

## Command line

```
python3 -I bootstrap/antpy.py run [--unchecked] FILE [ARGS...]
```

Runs the module at `FILE` as the entry module with `ARGS` as `Os.args`.
`--unchecked` is accepted and identical: types are erased and nothing is
checked either way. The exit status is

| Status | When | On stderr |
|---|---|---|
| 0 | `main` returns | nothing |
| 2 | a panic reaches the root of `main` | `panic: <message>`, then one `  at name (path:line)` line per frame, innermost first |
| 1 | a load error: the entry or an imported module is missing, does not parse, or forms an import cycle, or the entry module has no `func main()` with no parameters | `path:line:col: message` |
| n | `Os.exit(n)` | nothing |

A panic in a goroutine is reported the same way but ends only that
goroutine (section 9). The program ends when `main` returns, whatever other
goroutines are doing. `python3 -I` is required by the test contract and is
what makes `antpy.py` put `bootstrap/` and `tools/` on `sys.path` itself.

## Layout

`bootstrap/antpy.py` is the CLI; it parses the arguments above and calls
`antpy.interp.run_program`. The package `bootstrap/antpy/` holds the
interpreter, one concern per module:

| Module | Holds |
|---|---|
| `values.py` | the value classes and kinds, `KIND[type(v)]`, the panic exception, scopes and frames |
| `equal.py` | structural equality with cycle handling, the structural hash that agrees with it, and `AntMap`, the map keyed by them |
| `arith.py` | arithmetic, comparison, unary operators, the ten numeric conversions, and the `Bits` namespace |
| `render.py` | `String.from`, the rendering of every kind |
| `lower.py` | one pass from the reference parser's tuple AST to the evaluator's nodes: literals decoded, blocks that register deferred blocks marked |
| `interp.py` | scopes, frames, evaluation, calls, loops, deferred blocks, panics and traces, modules, goroutines, and the program root |
| `library.py` | the data namespaces `Array`, `Map`, `String`, `Iter`, `Math`, and the argument checks they share |
| `sync.py` | the sync objects `Mutex`, `RwMutex`, `WaitGroup`, `Once`, `Cond`, `Atomic` |
| `system.py` | the `Os` and `Time` namespaces |
| `fileio.py` | the `File` namespace |
| `jsonio.py` | the `Json` namespace |
| `prelude.py` | the prelude scope: tags, conversions, `print`, `panic`, `Result`, `Record`, and the binding of every namespace |

## Design

These are the decisions the roadmap's M0 paragraph names, as built.

- **Values carry their kind in their exact Python type.** A `KIND` table maps
  each type to the kind name, so a kind test is one dictionary lookup and
  `is` compares types. Python's `bool` subclasses `int` and the sized numeric
  kinds subclass `int` or `float`, so every kind test is `type(v) is ...`,
  never `isinstance`. Integer arithmetic checks the kind's range and panics
  `integer overflow`; `float32` is its own class rounded through `struct`.
- **Control flow is exceptions.** `return`, `break`, `continue`, and panics
  are `BaseException` subclasses, so they pass through the Python frames of
  the library (a `return` inside a `for` over a sequence leaves the Ant
  function, not the sequence driver). `ReturnSignal` carries the frame it
  leaves; `AntPanic` carries the message and the trace, filled at the
  innermost statement runner.
- **Deferred blocks belong to the block that registers them.** The lowering
  pass marks each block that contains a `defer` directly; only those blocks
  allocate a defer list at run time, and they run it in reverse on every exit.
  A panic inside a deferred block replaces the pending one (G59) while the
  rest still run.
- **Goroutines are threads under the GIL.** `go` starts a daemon
  `threading.Thread`; the GIL keeps the interpreter's data structures
  memory-safe under data races, and `sys.setswitchinterval(0.005)` makes
  preemption fine enough for the concurrency tests. The sync objects wrap the
  `threading` primitives; `Cond` is its own queue of waiters so that `signal`
  and `broadcast` may run without the mutex held.
- **Exit through `os._exit`.** When `main` returns, the process ends at once
  whatever the other threads are doing, after a bounded wait for a goroutine
  that has panicked but not yet written its report. `Os.exit(n)` is the same
  call.
- **Rendering per G33 and G55.** `String.from` renders a float from its
  shortest round-trip digits in JavaScript's layout (G55), always with a
  point or an exponent (G33); records as `Tag{ field: value }`, strings
  quoted inside containers and bare at the top level, and a container that
  contains itself as `...`. `render.py` is the one place that knows the form.
- **Deep recursion.** The recursion limit is raised to 5,000,000 (about ten
  Python frames per Ant call) and the program runs on a thread with a 512 MiB
  stack; a `RecursionError` is reported as `panic: stack overflow`.

## Running the suite

The acceptance command of roadmap step 0.G, from the repository root:

```
python3 -I src/tests/run.py --run "python3 -I bootstrap/antpy.py run {file} {args}" --run-unchecked "python3 -I bootstrap/antpy.py run --unchecked {file} {args}" --consistency --tier run --tier panic --tier unchecked --tier programs --tier io
```

It must exit 0 and end in `0 failures`. While working on one area, narrow it:
`--tier run` (repeatable) picks tiers, and `--filter /stdlib/` (repeatable,
any match) keeps the tests whose path contains the substring, so the 0.1
acceptance is the same command with `--tier run --tier panic --tier
unchecked --filter /smoke/ --filter /lexical/ --filter /variables/ --filter
/functions/ --filter /expressions/`. `--exclude` drops tests, `-v` prints one
`ok` line per passing test, and `--consistency` also runs the `run` and
`panic` tiers with the unchecked command and requires the same behaviour. The contract of the
tiers, exit codes, and canonical phrases is `src/tests/README.md`; the readings
the tests take where the spec is silent are `src/tests/GAPS.md`.

## Known limitations

Collected from the step reports under `work/0.*/report.md`.

- A string holding a lone surrogate (reachable only through a `\u{D800}`
  escape in source) cannot be printed or written by Python: `File.write` of
  one returns an `Err` (`invalid UTF-8`), and `Json.stringify` of one
  produces a text that `print` cannot encode.
- `Json.parse` error messages are Python's (`Expecting ',' delimiter: line 1
  column 6 (char 5)`); the spec fixes nothing about them.
- Map iteration (`for`, `Map.keys`, `Json.stringify` of a map) follows the
  bucket order of `AntMap`, and `File.list` follows the file system's order;
  section 11 and G43 leave both unspecified, and the tests sort first.
- `x is Result` panics `not a tag` when run unchecked: `Result` is the
  namespace record, not a tag. G17 reads `is` on an alias as `false`
  unchecked, and the checker rejects it either way, so the difference is
  unobservable in a checked program.
- Deep recursion through a library callback (a lambda passed to `Array.map`
  that calls `Array.map` again, and so on) re-enters the C stack at each
  level; the 512 MiB thread stack covers the suite, where plain Ant recursion
  lives on the heap.
- Memory safety under data races rests on the GIL; a free-threaded Python
  build would invalidate the argument. A goroutine that panics and then
  blocks forever in a deferred block delays the exit by up to one second.
- `Cond.wait` cannot tell who holds the mutex: "not locked" means free, so a
  wait on a mutex held by another goroutine releases it (a data race in the
  program).
- `func main(): int` is not refused: types are erased, so the rule waits for
  the checker (M3).

## After M2

Once `antc` compiles itself (roadmap step 2.5), `antpy` is retired from the
loop: every later acceptance command runs the native `bin/antc`, `make
stage1` is the only build that still goes through this interpreter, and the
roadmap's final step (4.1) only gives `bootstrap/` a dead-code and refactor
pass. Nothing after M2 depends on `antpy` being fast or complete beyond what
`antc`'s own sources use.
