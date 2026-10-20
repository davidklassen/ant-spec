# Spec gaps found while writing the test suite

One entry per question the spec does not settle. Each test that depends on an entry carries `// gap: G<n>`; `run.py --gaps` lists them. "Tests use" is the reading the suite encodes; it is a proposal, and changing the spec the other way means changing those tests. Section and line references are to `ant-lang.md` at the time of writing.

## G1 Numeric literals that do not fit
**Question.** `9223372036854775808` does not fit `int64`; `1e999` overflows `float64`. §2 says only that an integer literal is an `int`.
**Readings.** Parse error; checker error; runtime panic at evaluation; floats round to infinity.
**Tests use.** Rejected before the program runs (parse or check phase, the `check` tier accepts either), phrase `literal out of range`.

## G2 Non-scalar `\u{...}` escapes
**Question.** `\u{D800}` (surrogate) and `\u{110000}` are not code points. §2 lists the escape without limits.
**Tests use.** Rejected before running, phrase `invalid escape`.

## G3 Rows missing from the §12 panic table, and initializer scope
**Question.** The table claims completeness but has no row for a name with no binding, for `var { a } = x` when `x` is not a record or lacks `a`, or for import failures when running unchecked. Separately, §3 says a `var` is visible "from its declaration", which leaves `var x = x + 1` with an outer `x` open.
**Tests use.** `unbound name` panics; `var` destructuring panics `not a record` / `no field` like the `for` row; an initializer sees the outer binding of the same name (the new binding starts after the initializer).

## G4 Compound assignment and loop variables
**Question.** §12 says compound assignment "reads the target, applies the operator, and stores" without saying whether `xs[i()] += 1` evaluates `i()` once. Whether a `for` variable may be assigned is unstated.
**Tests use.** Target sub-expressions evaluate once. A `for` variable is an ordinary fresh `var` per iteration and may be assigned.

## G5 Integer edge cases the table does not name
**Question.** §12 prose says an integer result that does not fit panics; the table lists only `+ - *`. `INT_MIN / -1`, `INT_MIN % -1`, unary `-` on `INT_MIN`, and unary `-` on an unsigned value are not named.
**Tests use.** `INT_MIN / -1` panics `integer overflow`; `INT_MIN % -1` is `0`; `-INT_MIN` panics; `-x` on unsigned panics unless `x` is zero.

## G6 Float `%`
**Question.** §5 says `%` takes the sign of the dividend; §12 says float arithmetic follows IEEE 754 and never panics. The float remainder operation is not named.
**Tests use.** C `fmod`: `5.5 % 2.0 == 1.5`, `-5.5 % 2.0 == -1.5`, `x % 0.0` is `NaN`, never panics.

## G7 `float32` conversion of a value outside its range
**Question.** §12 says a float target "rounds to the nearest representable value" and the table says a conversion panics when "the value is outside the target's range".
**Tests use.** `float32(1e300)` panics `conversion out of range`; values inside the range round, so `float32(1e-50)` is `0.0`.

## G8 `bool` and `string` as run-time names
**Question.** §4 says `bool` and `string` are not callable. Whether they are bound to anything (for `is bool`, `is string`) is not said.
**Tests use.** `is bool` and `is string` work by name (§12); calling either panics `not a function` when unchecked.

## G9 `T | void` as a type
**Question.** An `if` without `else` has type `T | void` by §5; §4 says `void` is assignable only to `void`. Whether a union with `void` is a legal type is not said.
**Tests use.** `T | void` is a legal union; `(if false { 1 }) == void` is `true`.

## G10 Duplicate fields and spread
**Question.** A literal with the same field twice is "later wins" at run time (§12); the checker's verdict is unstated. A spread copies every run-time field (§12), so `Named{ ...n }` with `n: { name: string }` holding a `Person` yields a `Named` that also carries `age`, visible to `==`, `String.from`, and `Json.stringify`.
**Tests use.** Duplicate fields are a compile error (`unknown field`), later wins unchecked. The spread leak is tested as written in §12; the suite flags it for a decision (forbid spreading a value of anonymous record type, or copy only the head's fields).

## G11 Identity short-circuit in `==`
**Question.** §12 defines `==` structurally with a rule for pairs already under comparison. Whether `p == p` may answer `true` without comparing fields (a NaN field) is open.
**Tests use.** No identity short-circuit: `p == p` is `false` when a field is NaN.

## G12 Literals with mixed element types and no expected type
**Question.** §4 gives `Map{ (name): 1, 7: "seven" }` as an example but Inference says nothing about joining element types.
**Tests use.** With an expected type each element is checked against it. Without one, every element must have the same type; otherwise `cannot infer`.

## G13 NaN and `null` as map keys
**Question.** Maps are keyed by `==` and `NaN != NaN`; `null` as a key is never mentioned.
**Tests use.** A NaN key can be inserted but never found or removed, and repeated inserts grow the map. `null` is an ordinary key.

## G14 Named arguments through a function value
**Question.** §6 makes them a compile error; §12's call rule fills a named parameter when the callee has it, and the table only names lambdas.
**Tests use.** Checker rejects (`named argument`); the unchecked runtime accepts when the parameter exists and panics `unknown parameter` otherwise.

## G15 Operators on bounded type parameters, and union solutions
**Question.** §4 says an operator applies to `T` when it applies to every member of the bound, which makes `+` legal on `T: Ordered` (numbers add, strings concatenate) but is not spelled out. A union such as `int | string` is assignable to `Ordered`, so `Array.sort(xs)` with `xs: Array[int | string]` checks and then panics comparing an `int` to a `string`.
**Tests use.** `+` and `<` on `T: Ordered` are legal, `-` is not. The union case is a `panic` test as the spec stands; proposal: a bound solution must not be a union.

## G16 Unsolved type parameters
**Question.** §4 makes `var r = Result.ok(1)` a compile error because `E` is unsolved. Confirm this is intended.
**Tests use.** Compile error `cannot infer`.

## G17 `is` with an alias, and `is void`
**Question.** §5 restricts `is` to named record types, primitives, `null`, `Array`, `Map`, `func`. `x is UserId` (alias of `int`) and `x is Shape` (alias of a union) evaluate the alias tag at run time (§12) and answer `false`. §12 lists `void` as a kind test but the grammar's `is_target` excludes the keyword.
**Tests use.** `is` on any alias is a compile error (`not a record type`); unchecked it answers `false`. `is void` is rejected; tests write `== void`.

## G18 Mutual recursion with one annotation
**Question.** §4 says meeting a function whose inference is in progress is an error. Whether an annotated function's body counts as "in progress" while it is checked is unstated.
**Tests use.** Annotating one function of a mutually recursive pair suffices.

## G19 Narrowing cases §7 does not cover
**Question.** Assignment never narrows (implied); narrowing after an early-exit `if` (`if u == null { return }`); assignment inside the narrowed branch; reads inside a lambda created in the branch; `null` on the left of `==`.
**Tests use.** Assignment never narrows. Narrowing persists after an `if` whose branch cannot complete (`never`). An assignment inside a branch resets the variable to its declared type. Inside a nested function body nothing is narrowed; bind a local first. `null == u` narrows like `u == null`.

## G20 `??` with a non-nullable left side or a differently typed right side
**Question.** Unstated. The literal `null` on the left (`null ?? 2`) has a fixed outcome too.
**Tests use.** A left side that cannot be `null`, or that is the literal `null`, is a compile error (`always`); the result type is the left type without `null`, joined with the right type.

## G21 `?.` "never panics"
**Question.** §12's table says `?.` never panics; its rule says the plain rule applies when the left side is not `null`.
**Tests use.** Never panics on `null`; on a non-null left side the plain rule applies, so `(1)?.x` panics `not a record` unchecked.

## G22 Shifts that drop bits, and mixed-kind `Bits` arguments
**Question.** `Bits.shiftLeft(1, 63)` changes the sign; whether that is "overflow" is unstated. `Bits.and(int8(1), 1)` mixes kinds.
**Tests use.** Shifting bits out never panics. Mixed kinds are a compile error and panic `wrong kind` unchecked.

## G23 Lambdas: as a pipe target and with zero arguments
**Question.** `5 | { x + 1 }()` satisfies "the right side must be a call". A lambda called with no argument binds `x` to nothing in particular.
**Tests use.** The pipe form is legal. `x` is `void` when the lambda is called with no argument.

## G24 Placement of `break`, `continue`, and `return`
**Question.** None of these is addressed: `break` outside a loop, inside a lambda or `defer`/`go` block within a loop, `return` in a top-level initializer, `return` inside `go` or `defer`.
**Tests use.** `break`/`continue` must sit inside a loop of the same function body, so inside a lambda, `defer`, or `go` block they are compile errors (`outside a loop`). `return` in a top-level initializer is `outside a function`. `return` inside a `go` or `defer` block leaves that block only.

## G25 Sequences: unwinding and foreign `yield`
**Question.** A `return` in the loop body "propagates through the sequence as through any call", so the sequence function's deferred blocks should run; `yield()` with no argument and `yield` from another goroutine are unstated.
**Tests use.** The sequence function unwinds and its defers run. `yield()` with no argument panics `missing argument`. Calling `yield` from another goroutine is not tested.

## G26 Defaults that refer to earlier parameters
**Question.** §12 evaluates defaults "in the callee's scope" after the arguments, which implies `func f(a: int, b: int = a * 2)` works; the checker rule is unstated.
**Tests use.** Allowed, in parameter order.

## G27 A `var` lambda that refers to itself
**Question.** `var f = func(n: int): int { f(n - 1) }`: is `f` in scope inside its own initializer?
**Tests use.** No: `unknown name`. Use a top-level `func` for recursion.

## G28 `try` edge cases
**Question.** `try` on a value that is neither a `Result` nor nullable; `try` on `Result[T, E] | null`; `try` inside a lambda, `defer`, or `go` block where "the enclosing function" is unclear.
**Tests use.** A non-`Result`, non-nullable operand is a compile error (`try`), and unchecked yields the value. `Result | null` is handled both ways. Inside `defer` and `go` blocks `try` is a compile error; inside a lambda it is legal only when the lambda's expected type returns a `Result` or a nullable type.

## G29 A deferred block that reads a variable declared later in the block
**Question.** `{ defer { print(y) }; var y = 2 }`: §3 makes `y` invisible at the `defer`; §12's scope lookup at run time finds it.
**Tests use.** Compile error (`unknown name`); unchecked it prints `2`.

## G30 `Once.do` when `f` panics
**Tests use.** The panicking call counts as done; later calls return at once.

## G31 Writes through imports when unchecked, and module records as values
**Question.** Assignment to an imported name or namespace field is a compile error; the unchecked semantics follow §12's field-assignment rule. How a module record renders is unstated.
**Tests use.** Unchecked, assigning `Geometry.counter = 1` mutates the exporter's binding. `Geometry == Geometry` is `true`; rendering is not asserted.

## G32 Missing `main`, missing import file, missing extension
**Question.** Which phase reports these, and how.
**Tests use.** Reported before running (`check` tier accepts either phase): `main` and `not found`.

## G33 Rendering by `String.from`
**Question.** §11 says primitives render in literal syntax and leaves floats, nested strings, sized ints, `void`, NaN and infinities, map order, and cycles open.
**Tests use.** Strings bare at top level, quoted with escapes inside arrays, maps, and records. Sized ints render as bare digits. Floats: shortest decimal that round-trips, always with a `.` or an exponent (`50.0`, `0.1`, `1e21`, `-0.0`); `NaN`, `Infinity`, `-Infinity`. `void` renders `void`. Multi-entry maps are never asserted. A cyclic value renders the back-reference as `...`.

## G34 `Array.pop` on an empty array
**Question.** §11 says it returns `null`; the §12 table uses it as an example of a documented panic.
**Tests use.** `null`.

## G35 Sorting details and `Array[void]`
**Question.** Stability of `sortBy` and `sortWith`; NaN in `sort`, `min`, `max`; whether `Array[void]` is a legal type (`Array.map(xs, { print(x) })`).
**Tests use.** `sortBy` and `sortWith` are stable. NaN ordering is not tested. `Array[void]` is legal.

## G36 String edge cases
**Question.** `String.split("", sep)`; `String.indexOf(s, "")`; case mapping beyond one-to-one; the syntax `toInt`/`toFloat` accept and what happens on overflow.
**Tests use.** `split("", ",")` is `[""]`; `indexOf(s, "")` is `0`; `toUpper`/`toLower` apply simple one-to-one Unicode mappings (`"ß"` unchanged, no locale rules); `toInt` accepts an optional sign and decimal digits only, `null` on anything else including overflow; `toFloat` accepts an optional sign, digits with an optional fraction and exponent, `null` otherwise.

## G37 `Iter.take(s, 0)`
**Tests use.** Does not run the source at all.

## G38 `int(1.5)`
**Question.** §4 says float-to-int conversion truncates toward zero; `stdlib.md` says `int(...)` panics on a fraction.
**Tests use.** Truncates: `int(1.5) == 1`.

## G39 `Os` and `Json` details
**Question.** Whether `Os.args()` includes the program name; whether `Os.exit` runs deferred blocks and what it does from another goroutine; `Json.stringify` of non-finite floats, functions, sized ints, and map ordering; duplicate keys and surrogate pairs on parse.
**Tests use.** `Os.args()` holds only the arguments. `Os.exit` ends the process at once from any goroutine without running deferred blocks. `Json.stringify` renders NaN and infinities as `null`, functions, tags, and sync objects as `null`, sized ints as numbers, map keys through `String.from`; `Json.parse` keeps the last duplicate key and decodes surrogate pairs to one code point.

## G40 `Atomic.compareAndSwap` compares with `==`
**Question.** Structural `==` means a CAS on `Atomic[Array[int]]` can succeed against a different array with equal contents, which is not compare-and-swap.
**Tests use.** As written: an equal-but-distinct record swaps. Proposal: compare by identity, or bound `T` to primitives.

## G41 Static typing of `==` and `!=`
**Question.** §5 says both operands must have the same type, yet `x == null` on `User | null` is the canonical narrowing test and `id == 5` on `int | string` is natural.
**Tests use.** Legal when either operand's type is assignable to the other's; otherwise `not assignable`. The runtime never panics.

## G42 Assignment targets the grammar accepts
**Question.** `g() = 1` and `(x) = 1` parse as assignments; §3 lists the legal targets but no phase is named for rejecting the others. Whether a field target may start from any expression (`p().x = v`) rather than a variable is also unstated.
**Tests use.** `g() = 1`, `(x) = 1`, and a destructuring target are compile errors (`cannot assign`). A field or index target may hang off any expression: `p().x = v()` is legal and evaluates `p()` before `v()`.

## G43 `File.list` entries
**Question.** Bare names or full paths; whether `.` and `..` appear; order.
**Tests use.** Bare entry names without `.` or `..`, in unspecified order.

## G44 Line terminators in `File.writeLine` and `File.readLine`
**Tests use.** `writeLine` appends `"\n"`; `readLine` returns the line without its terminator and `null` at end of file.

## G45 `Json.stringify` layout
**Question.** stdlib.md fixes neither whitespace nor field order.
**Tests use.** Compact JSON with no whitespace; record fields in declaration order; numbers as `String.from` renders them, so `Point{ x: 1.0, y: 2.0 }` is `{"x":1.0,"y":2.0}`.

## G46 Code after `return` or a `never` expression
**Question.** Nothing in the spec rejects a statement after `return`, `panic(...)`, or `Os.exit(...)`; README's `unreachable` phrase has only the §5 `else`-after-complete-chain rule behind it.
**Tests use.** Legal: statements after a `never` expression check and never run.

## G47 Identifier case
**Question.** §2 says "Case carries no meaning", which could mean case-insensitive names.
**Tests use.** Identifiers are case-sensitive; the sentence means case has no semantic role such as Go's export-by-capital.

## G48 `is int` and `is byte`
**Question.** §12 lists "the ten numeric names" as kind tests; `int` and `byte` are aliases (§4) bound to conversion functions, so by the letter `x is int` would evaluate a non-tag and panic. §4's own `describe` example uses `id is int`.
**Tests use.** `is int` and `is byte` are kind tests for `int64` and `uint8`; G17 applies to user aliases only.

## G49 Type arguments of a generic record literal
**Question.** §4 says how calls solve type parameters, and that a literal head never writes them, but not how `Pair{ first: 1, second: "a" }` gets its arguments without an expected type.
**Tests use.** The expected type fixes them first; otherwise each is synthesized from its field's initializer, so the literal is `Pair[int, string]`.

## G50 Static type of `a?.b.c`
**Question.** §12 settles the run time (the rest of the postfix chain is skipped) but not whether the checker types `.c` against `b`'s type or against `b | null`.
**Tests use.** The short-circuit covers the rest of the postfix chain: `a?.b.c` has the type of `b.c` joined with `null`, and `f?.(g())` is typed like `f(g())` joined with `null`.

## G51 `RwMutex` semantics
**Question.** §11 lists the operations and the unlock panics only.
**Tests use.** Readers share the lock; a writer excludes readers and other writers.

## G52 Field order of a record with undeclared fields
**Question.** Once a spread carries fields the head's type does not declare (G10), rendering order is unstated.
**Tests use.** Creation order: literal entries in written order, a spread contributing its source's fields in the source's order.

## G53 Where a missing `main` is reported
**Question.** G32 fixes the phase and phrase; a missing declaration has no line.
**Tests use.** The report points at the entry module's last declaration. The runner cannot place a marker on line 1, which is the tier line.

## G54 A type name as a value
**Question.** A `type` name is bound to a tag value (§4, §10) and tags render as their name (§11), but the static type of a bare type name in expression position is unstated, so whether `print(Point)` checks and what `Point.x` reports is open.
**Tests use.** A type name is a legal expression whose value is the tag, accepted where an unbounded `T` is expected, rendering as the name. `Point.x` is rejected by the checker (bare marker) and panics `not a record` unchecked.

## G55 Exponent thresholds in float rendering
**Tests use.** JavaScript's thresholds, which is where G33's `1e21` comes from: exponent form when the magnitude is at least 1e21 or below 1e-6, no `+` sign on positive exponents; `1.5e-7` renders `1.5e-7`, `123456789.0` stays positional.

## G56 `Math.abs(-0.0)`
**Tests use.** IEEE 754 `abs` clears the sign bit: `1.0 / Math.abs(-0.0)` is `Infinity`.

## G57 The last `is` test of a complete chain
**Question.** §5 makes an `is` whose outcome is fixed a compile error, yet §4's `area` example ends with `else if s is Empty` where `s` can only be `Empty`.
**Tests use.** An `is` test on the last remaining member of a union is allowed; the chain is then complete and its implicit `else` is dropped.

## G58 (untested) Narrowing in `while` conditions
**Question.** §7 describes narrowing for `if` conditions and `??` exits only.
**Tests use.** Not relied on; `io/file-lines` uses `?? break` instead. Proposal: a `while` condition narrows its body like an `if` condition.

## G59 Deferred blocks after a replacing panic
**Question.** §12 says a panic inside a deferred block replaces the one in progress but does not say outright that the block's remaining deferred blocks still run.
**Tests use.** They run; unwinding continues with the replacement panic.

## G60 (untested) Generic calls under a union expected type
**Question.** §4 Inference does not say how `Result.ok(x)` solves its parameters when the expected type is `Result[T, E] | null`.
**Tests use.** Not relied on; tests bind an annotated local first. Proposal: the member of the expected union with the call's result constructor is the expected type.

## G61 (untested) Hoisted function bodies and later top-level `var`s
**Question.** §3 makes a top-level `var` visible from its declaration; §10 hoists `func` bodies. Whether a hoisted body may name a `var` declared later in the file is unstated; §10's "reaching one through a function call ... is a runtime panic" suggests yes.
**Tests use.** Not relied on; the panic tests declare the function after the var. Proposal: allowed, and reading it before initialization panics.

## G62 (untested) A lambda against a union expected type
**Question.** `var f: (func(int): int) | null = { x }`: §6 says a lambda needs an expected function type; whether the checker picks the function member of a union is unstated.
**Tests use.** Not relied on; such variables are initialized with `func`. Proposal: a union with exactly one function member supplies the expected type.

## G63 (untested) An assignment's target type as an expected type
**Question.** §4 Inference lists the sources of expected types and omits assignment targets, so `scores = []` after `var scores: Array[int]` would be `cannot infer` by the letter.
**Tests use.** Not relied on. Proposal: the target's type is the expected type of the right-hand side.

## G64 The sync type names
**Question.** §9 and §11 use `Mutex`, `RwMutex`, `WaitGroup`, `Once`, `Cond`, and `Atomic[T]` as types, but the prelude listing in §11 declares none of them.
**Tests use.** They are prelude types usable in annotations and record fields.

## G65 When an `Iter` ◆ panic fires
**Question.** §11 says every `Iter` function returns a new `Seq` without running the source, so `Iter.take(s, -1)` could panic at the call or on the first run.
**Tests use.** The test runs the sequence, so either reading passes. Proposal: at the call.

## G66 `Record.same` across named record types
**Question.** `Record.same[T: {}](a: T, b: T)` forces one `T`, and a named type accepts only its own tag, so `Record.same(p, v)` with `p: Point` and `v: Vec` does not check even though §4 presents identity as a library call for any two records.
**Tests use.** Widening both to an anonymous record type first. Proposal: two type parameters, `Record.same[A: {}, B: {}]`.

## G67 `panic(1)` when unchecked
**Question.** The §12 table has both "library function: an argument has a kind the function does not take" and "`panic`: always"; which applies to `panic(1)` is unstated.
**Tests use.** `wrong kind`.
