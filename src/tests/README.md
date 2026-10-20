# Ant semantic test suite

This directory is the executable specification of Ant. Every test is an Ant program whose expected behavior is derived from `ant-lang.md` and `stdlib.md`, with the section that justifies it cited in the file. An implementation passes the suite when every tier below passes. Where the spec is silent or contradicts itself, the test records the reading it uses and `GAPS.md` lists the question.

The grammar suite in `../../tests` (reference parser `../../tools/antparse.py`) is tier zero: every file here must parse with it, and `run.py` checks that without any implementation.

## Tiers

| Directory | First line | What the implementation must do |
|---|---|---|
| `run/<section>/` | `// run` | Checker accepts. Program exits 0. Stdout equals the `output:` block (default: empty). |
| `panic/<section>/` | `// panic` | Checker accepts. Program exits non-zero and stderr contains the `panic:` phrase. Stdout before the panic equals `output:`. |
| `check/<section>/` | `// check` | Checker rejects. Every line marked `// ERROR` is reported, and no unmarked line is. |
| `unchecked/<section>/` | `// unchecked` | Checker rejects (markers matched if present). Run with checking off: panics with the `panic:` phrase, or, when there is no `panic:`, exits with `exit:` (default 0) and matching stdout. |
| `programs/<name>/` | `// program` | Whole program, possibly several modules, `main.ant` is the entry. Checker accepts; exit status is `exit:` (default 0); stdout equals `output:`. |
| `io/<name>/` | `// io` | As `program`; the runner passes a fresh empty temporary directory as the first argument, before `args:`. |

Sections: `smoke`, `lexical`, `variables`, `types`, `expressions`, `functions`, `nullability`, `errors`, `concurrency`, `modules`, `stdlib`, `dynamic`. A directory containing `main.ant` is one test in any tier (used for multi-file module cases); otherwise every `.ant` file is a test.

`run/smoke/` uses no imports and only `print`, so that a broken import system is reported as such.

## File format

The header is the leading block of `//` lines. The first line is the tier. Then directives, one per line:

```
// run
// spec: 8 defer; 12 deferred blocks and panics
// source: go:test/defer.go
// gap: G29
// args: alpha beta
// env: ANT_TEST=1
// panic: integer overflow
// stderr: panic: boom
// exit: 3
// output:
//   first line
//   second line
//
//   fourth line, after an empty one
```

| Directive | Required | Meaning |
|---|---|---|
| `spec:` | yes | Sections or subsections of `ant-lang.md` (or `stdlib`) that justify the expectation. Free text, section number first. |
| `source:` | no | Provenance of an adapted scenario: `go:test/<file>`, `zig:behavior/<file>`, `spec` (the spec's own example). |
| `gap:` | no | Comma-separated ids from `GAPS.md`. The expectation is a proposal, not settled spec. An entry whose heading says `(untested)` records a question no test relies on. |
| `args:` | no | Command-line arguments, shell-split. |
| `env:` | no | `NAME=value` pairs, space separated. |
| `panic:` | panic tier; optional in unchecked | Substring that stderr must contain. Use the canonical phrases below, or the message of a user `panic(...)`. |
| `stderr:` | no | Substring stderr must contain in tiers that exit normally (goroutine panics). |
| `exit:` | no | Expected exit status for `program`, `io`, and non-panicking `unchecked` tests. |
| `timeout:` | no | Seconds; default 60. |
| `output:` | no | Expected stdout. Each following `//` line with two spaces after the slashes is one line of output; a bare `//` is an empty line. The block ends at the first other line. No block means stdout must be empty. |

### ERROR markers (`check` and `unchecked` tiers)

Put `// ERROR "phrase"` at the end of the line the checker must report; `// ERROR` alone matches any message on that line. Use the checker phrases below; add a phrase only when none fits. In multi-file tests, markers may sit in any module.

```ant
var s: string = null            // ERROR "not assignable"
```

## Writing tests

- One scenario per file; the file name is the scenario id (`defer-block-scoped.ant`). Aim for 10–40 lines, never more than ~60.
- Run-tier tests are self-checking: import the helpers and panic on the first failure. Print only when rendering is the point of the test, and then give an `output:` block.
- Cite the spec. If the expectation comes from a reading the spec does not force, add `gap:`.
- Determinism: never print or compare the rendering of a map with more than one entry; sort keys first. Synchronize goroutines with `WaitGroup`; never rely on timing except in `io/` time tests.
- Use `== void` rather than `is void` (see G17).
- Literals: an integer literal is `int`, a float literal `float64`. Write `1.0` for floats and `int8(1)` for other kinds.
- Panic messages: for runtime rows use the canonical phrase; for `panic("x")` use `x`.

### Assertion helpers (`lib/expect.ant`)

```ant
import "../../lib/expect.ant" Expect

func main() {
  Expect.eq(1 + 2, 3, "addition")
  Expect.isTrue(Array.isEmpty([]), "empty")
  Expect.isNull(Array.get([1], 5), "out of range get")
  var v = Expect.notNull(Map.get(m, "k"), "k present")
  Expect.mark("a"); Expect.mark("b"); Expect.eq(Expect.marks(), "a,b", "order")
  var y = Expect.markv("f", 1)      // logs "f" and yields 1: useful inside argument lists
}
```

`eq`, `ne`, `isTrue`, `isFalse`, `isNull`, `notNull`, `same` (identity for records, arrays, maps via the `Record.same` family is a library call; `Expect.same` is for records only), `fail`. All panic with a message that includes the label. The log helpers record evaluation order.

### Ant syntax reminders

- Named `func` and `type` only at the top level; inside a block bind a function with `var f = func(a: int) { ... }`.
- A lambda `{ x * 2 }` has the single implicit parameter `x`; two parameters need `func(a: int, b: int) { a + b }` (`Array.reduce`, `Array.sortWith`).
- Braces after an identifier path are a literal: `Point{ x: 1.0 }`, `Map{ "k": 1 }`. In an `if`/`while`/`for` header parenthesize them: `if p == (Point{ x: 1.0, y: 0.0 }) { ... }`. The `{` of a block must be on the header's line.
- `else` may start a line. A line starting with `.`, `?.`, or `|` continues the previous expression; anything else starts a new statement.
- No `s[i]` on strings (`String.chars`, `String.slice`); no `.length` fields (`Array.length(xs)`); `m[k]` and `Map.get` give `V | null`; `xs[i]` panics out of range, `Array.get` gives `null`.
- `for x in xs`, `for { key, value } in m`, `for { index, value } in Array.entries(xs)`, `for c in "abc"` (code points), `for n in seq`.
- `x ?? return ...`, `try e`, `defer { ... }`, `go { ... }`, `Result.ok(void)`.
- Imports first, path with extension; `export` on declarations other modules use; `func main()` returns `void`.

## Implementation contract

The runner drives an implementation through three command templates, each run from the repository root. `{file}` is the absolute path of the test file (`main.ant` for directories), `{args}` the shell-quoted arguments, `{dir}` the temporary directory for `io` tests.

```
python3 -I src/tests/run.py \
  --check "antc check {file}" \
  --run "antc run {file} {args}" \
  --run-unchecked "antc run --unchecked {file} {args}"
```

- `check`: exit 0 when the program checks. Otherwise exit non-zero and print one report per error on stderr as `path:line:col: message` (`path:line: message` is accepted).
- `run`: check, then run. Exit 0 when `main` returns. A panic prints `panic: <message>` and a stack trace to stderr and exits non-zero. A panic in another goroutine prints the same and the program continues.
- `run-unchecked`: run without the checker. Where the checker would have rejected the program, the runtime panics at the point of the error with the phrases below.

### Canonical panic phrases (section 12 table)

| Row | Phrase |
|---|---|
| integer `+ - *`, compound assignment, `Array.sum`, `Atomic.add`, `Math.abs`, unary `-` | `integer overflow` |
| integer `/`, `%` by zero | `division by zero` |
| `Bits.shiftLeft`, `Bits.shiftRight` count | `shift count` |
| numeric conversion out of range, NaN, infinity | `conversion out of range` |
| arithmetic or comparison operand not a number | `not a number` |
| operands of different kinds | `kind mismatch` |
| `!`, `&&`, `\|\|`, `if`, `while` operand | `not a bool` |
| `a.b` on a non-record; record spread of a non-record | `not a record` |
| `a.b` with no such field; destructuring without the field | `no field` |
| `a[i]` on a value that is neither array nor map | `not indexable` |
| array index outside `0 ..< length` | `index out of range` |
| array index that is not an `int` | `index not an int` |
| callee not a function | `not a function` |
| too many positional arguments | `too many arguments` |
| named argument for an unknown parameter | `unknown parameter` |
| named argument for a filled parameter | `parameter already filled` |
| required parameter unfilled; `yield()` without an argument | `missing argument` |
| named argument to a lambda | `named argument to lambda` |
| record literal head or `is` path not a tag | `not a tag` |
| array spread of a non-array | `not an array` |
| `for` over a non-iterable | `not iterable` |
| `yield` after it returned `false` or after the sequence returned | `yield after done` |
| library argument of a kind the function does not take | `wrong kind` |
| top-level `var` read before its initializer ran | `not initialized` |
| name with no binding (G3) | `unbound name` |
| `Result.unwrap` on `Err` | `unwrap on Err` |
| `Array.filled`, `String.repeat`, `Iter.take` with a negative count | `negative count` |
| `String.split` with an empty separator | `empty separator` |
| `String.replace` with an empty pattern | `empty pattern` |
| `Mutex.unlock`, `RwMutex.*Unlock`, `Cond.wait` without the lock | `not locked` |
| `WaitGroup` counter below zero | `negative counter` |
| `Array.slice`, `String.slice` bounds | `index out of range` |

### Checker phrases (for `// ERROR "..."`)

`not assignable` (any type mismatch: initializer, assignment, argument, return, field, branch) · `unknown name` · `redeclared` (same scope, duplicate import names, top-level shadowing an import or prelude name) · `cannot assign` (to an import, a namespace field, a non-target, a field through a union) · `not a bool` · `mismatched operand types` · `not a number` · `missing field` · `unknown field` · `not callable` · `too many arguments` · `missing argument` · `unknown parameter` · `parameter already filled` · `named argument` (through a function value or to a lambda) · `cannot infer` (unsolved type parameter, mixed literal, lambda without an expected type) · `does not satisfy bound` · `always` (an `is` or `??` whose outcome is fixed) · `not a type name` · `not a record type` · `does not narrow` · `unreachable` · `needs a return type` · `cycle` (type alias or import) · `not exported` · `main` (missing or wrong signature) · `not found` (import path) · `literal out of range` · `invalid escape` · `try` (operand or enclosing function unsuitable) · `outside a loop` · `outside a function` · `not iterable` · `cannot index` · `alias of a record type` · `indistinguishable` · `void` (a `void` value used where another type is needed) · `not a record` (a field read or write on a value whose static type is not a record).

## Running

```
python3 -I src/tests/run.py                 # parse every test with the reference parser; validate headers
                                            # (check-tier tests count as skipped until --check is given)
python3 -I src/tests/run.py --strict        # also fail on gap ids missing from GAPS.md
python3 -I src/tests/run.py --tier run --filter defer --filter /errors/ -v   # filters are OR-ed; --exclude removes matches
python3 -I src/tests/run.py --timeout 300 --run ...                           # per-test timeout override
python3 -I src/tests/run.py --coverage      # checklist entries no test mentions
python3 -I src/tests/run.py --gaps          # tests per gap id
python3 -I src/tests/run.py --consistency --run ... --run-unchecked ...   # run/panic tiers must behave the same unchecked
```

`oracle.py` is a fake implementation that reads each test's own header and behaves accordingly. `run.py --oracle` drives the suite through it and must report zero failures; `--oracle --sabotage` must report a failure for every test. Use it after changing the runner or the header format.

## Coverage checklist

Entries `run.py --coverage` looks for in the test sources (library names) and in `panic:` directives (phrases).

- `print` `panic` `int8(` `int16(` `int32(` `int64(` `uint8(` `uint16(` `uint32(` `uint64(` `float32(` `float64(` `int(` `byte(`
- `Array.length` `Array.isEmpty` `Array.get` `Array.push` `Array.pop` `Array.slice` `Array.concat` `Array.map` `Array.filter` `Array.reduce` `Array.find` `Array.some` `Array.every` `Array.contains` `Array.indexOf` `Array.sort(` `Array.sortBy` `Array.sortWith` `Array.reverse` `Array.entries` `Array.range` `Array.filled` `Array.flatten` `Array.sum` `Array.same`
- `Map.size` `Map.isEmpty` `Map.has` `Map.get` `Map.set` `Map.remove` `Map.keys` `Map.values` `Map.entries` `Map.fromEntries` `Map.map` `Map.filter` `Map.merge` `Map.same`
- `String.length` `String.isEmpty` `String.slice` `String.concat` `String.split` `String.join` `String.repeat` `String.trim` `String.contains` `String.startsWith` `String.endsWith` `String.indexOf` `String.replace` `String.toUpper` `String.toLower` `String.chars` `String.toBytes` `String.fromBytes` `String.toInt` `String.toFloat` `String.from`
- `Iter.range` `Iter.fromArray` `Iter.map` `Iter.filter` `Iter.take` `Iter.toArray`
- `Math.abs` `Math.min` `Math.max` `Math.pow` `Math.sqrt` `Math.floor` `Math.ceil` `Math.round` `Math.PI` `Math.E`
- `Bits.and` `Bits.or` `Bits.xor` `Bits.not` `Bits.shiftLeft` `Bits.shiftRight` `Bits.popCount` `Bits.wrapAdd` `Bits.wrapSub` `Bits.wrapMul`
- `Result.ok` `Result.err` `Result.isOk` `Result.map` `Result.mapErr` `Result.unwrap` `Result.unwrapOr` `Record.same`
- `Mutex.new` `Mutex.lock` `Mutex.unlock` `Mutex.withLock` `RwMutex.new` `RwMutex.readLock` `RwMutex.readUnlock` `RwMutex.writeLock` `RwMutex.writeUnlock` `RwMutex.withRead` `RwMutex.withWrite` `WaitGroup.new` `WaitGroup.add` `WaitGroup.done` `WaitGroup.wait` `Once.new` `Once.do` `Cond.new` `Cond.wait` `Cond.signal` `Cond.broadcast` `Atomic.new` `Atomic.load` `Atomic.store` `Atomic.add` `Atomic.compareAndSwap`
- `File.read` `File.readBytes` `File.write` `File.writeBytes` `File.exists` `File.remove` `File.list` `File.open` `File.create` `File.readLine` `File.writeLine` `File.pipe` `File.close` `Time.now` `Time.sleep` `Time.monotonic` `Os.args` `Os.env` `Os.exit` `Json.parse` `Json.stringify`
- `panic:integer overflow` `panic:division by zero` `panic:shift count` `panic:conversion out of range` `panic:not a number` `panic:kind mismatch` `panic:not a bool` `panic:not a record` `panic:no field` `panic:not indexable` `panic:index out of range` `panic:index not an int` `panic:not a function` `panic:too many arguments` `panic:unknown parameter` `panic:parameter already filled` `panic:missing argument` `panic:named argument to lambda` `panic:not a tag` `panic:not an array` `panic:not iterable` `panic:yield after done` `panic:wrong kind` `panic:not initialized` `panic:unbound name` `panic:unwrap on Err` `panic:negative count` `panic:empty separator` `panic:empty pattern` `panic:not locked` `panic:negative counter`
