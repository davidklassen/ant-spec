# Report for step 1.2 "antc-parser": the parser in Ant, `antc parse --dump`, `compare_frontend.py parse`

## What was built

| File | Lines | Content |
|---|---|---|
| `src/antc/ast.ant` (new) | 93 | the AST: 60 exported record types and the unions `Module`, `ImportPattern`, `Decl`, `Target`, `TypeExpr`, `Stmt`, `Expr`, `Arg`, mirroring the reference's node kinds; every record carries `line`/`col` (the dump ignores them, M3 reports through them); the header comment states which token a node's position is |
| `src/antc/dump.ant` (new) | 193 | `module(m): string`, the S-expression renderer of the shared dump contract: `quote`, `list`, `strs`, one renderer per union (`decl`, `target`, `typ`, `stmt`, `expr`, `arg`, `elem`, `entry`) plus `importDecl`, `alias`, `varDecl`, `tparam`, `param`, `signature`, `fieldType`, `ftParam`, `block`, `ifExpr`, `exported` (the `Export` re-wrap) |
| `src/antc/parser.ant` (new) | 947 | `parse(src): Result[Ast.Module, ParseError]`, a method-by-method port of the `Parser` class of `tools/antparse.py` (lines 282-972): the state record `Parser{ toks, i, nl, header }`, the token helpers (`raw`, `peek`, `next`, `at`, `atOp`, `atKw`, `atKind`, `expect`, `expectIdent`, `skipTerms`, `expectTerm`), `describe` (the reference's `Token.__repr__`) with `pyRepr`, the scope helpers `enterList`/`enterBlock`/`leave`, and the grammar functions in the reference's order: `parseFile`, `importDecl`, `aliasItem`, `declaration`, `varDecl`, `funcDecl`, `typeDecl`, `typeParams`, `signature`, `typeExpr`, `typeTerm`, `recordType`, `funcType`, `block`, `statements`, `statement`, `lambda`, `expression`, `pipe`, `pipeOperand`, `equality`, `comparison`, `isTarget`, `additive`, `multiplicative`, `unary`, `postfix`, `callArgs`, `primary`, `keyword`, `parsePart`, `headerExpr`, `ifExpr`, `forExpr`, `destructure`, `braceLit`, `arrayLit` |
| `src/antc/main.ant` (changed) | 80 | `parse FILE` and `parse --dump FILE` through `parseCommand(path, withDump)`; `readSource` shared by both commands; the usage line covers every other form (exit 2); header comment updated |
| `tools/compare_frontend.py` (changed) | 238 | the `parse` mode: `ANTC_ARGS`, `quote`/`sexpr`/`ast_dump` (the reference side of the contract), `reference()` for both modes, the rule that a `tests/err` file must be rejected by both sides (detail `tests/err file accepted by <side>`), docstring with the AST dump form |
| `work/1.2/probes/*.ant`, `work/1.2/probes.log` | 72 files | hand-written edge cases (see Probes); all agree |

Every parsing function takes the state `p` first and returns `Result[X, ParseError]`; callers unwrap with `try`, so an error propagates to `parse` with the position and message of the reference. A lexer error is converted by `fromLex` and keeps the lexer's position, as the reference's `Parser.__init__` does. `exported` on `VarDecl`/`FuncDecl`/`TypeDecl` replaces the reference's `("Export", d)` wrapper and the dump re-wraps.

The dump contract (in the docstring of `compare_frontend.py` and the header of `dump.ant`): `nil` for `None`; strings quoted with `\`, `"`, LF, CR escaped in that order and nothing else; lists `[a b]`; tuples `(Kind a b)` with a bare head when the first element is a string (the node kind, or the name that opens the reference's untagged tuples: import items and destructure fields, type parameters, parameters, record-type fields), the other first elements rendered normally (`([...] ret)` of a signature, `(nil type)` of an unnamed function-type parameter); the File node is the only multi-line one.

Work followed the plan's order: the compare tool with the reference side first (the reference dump of `tests/ok/braces.ant` checked by eye against the contract), then `ast.ant`, `dump.ant`, the `parse` command, and `parser.ant` in one pass; `tests/ok` (8/8) and `tests/err` (35/35) agreed on the first run, then `--sample`, `--all`, the probes.

## Acceptance

Command, verbatim: `python3 -I tools/compare_frontend.py parse --sample` (output in `work/1.2/test.log`, exit 0). Last line:

```
85 files, 0 failures
```

(8 `tests/ok` files, 35 `tests/err` files, 42 suite files.)

The 1.G gate command for this mode, `python3 -I tools/compare_frontend.py parse --all` (`work/1.2/all.log`, exit 0), last line:

```
797 files, 0 failures
```

(the 722 files of step 1.1 plus `src/antc/ast.ant`, `src/antc/dump.ant`, `src/antc/parser.ant` and the 72 probes under `work/1.2/probes/`; a first run before the probes existed gave `725 files, 0 failures`, and the final run followed the one-line usage fix in `main.ant`). antc parses its own parser identically to the reference, including the 36 `work/1.1/probes/` files that fail with the positions the plan lists.

## Test changes

None. No test contradicted the spec; nothing under `src/tests` or `tests/` was touched.

## Decisions and deviations from the plan

1. **`while true` loops with an inner `return` are written as condition loops.** The reference's `comparison` and `postfix` loop forever and return from inside; in Ant a function whose body ends in `while` has the value `void`, which the M3 checker would reject against the declared `Result` return type. `comparison` loops `while isCompareOp(t) || t is the keyword is`, `postfix` loops `while isPostfixOp(t)` (`.`, `?.`, `[`, `(`) and re-peeks at the end of each iteration; the `(` branch is the `else`, since it is the only remaining operator. Same tokens consumed, same `peek` side effects.
2. **`keyword(p, t)` holds the keyword primaries** (`true`/`false`/`null`/`void`, `func`, `if`, `while`, `for`, `go`, `defer`, `return`, `break`, `continue`), split out of `primary` to keep both readable; `endsReturn(op)` names the closers after which `return` is bare.
3. **`aliasItem(p, what)` is shared** by import items (`what` = `export name`) and destructure fields (`field name`); both use `local name` after the `:`, as the reference does.
4. **`keyString(key)` builds the `String` node for a bare identifier key in a map literal** (`"` + name + `"` at the key's position), used by both the `:` and the shorthand case.
5. **The shorthand branch of `braceLit` is `else if key is Ast.Ident { ... } else { error }`** rather than the reference's `if not Ident: error` followed by a use of the key, so the identifier is used only where the checker narrows it.
6. **`main.ant`: `parse --dump` without a file is a usage error** (exit 2), not an attempt to read a file named `--dump`; the two-argument form requires `args[1] != "--dump"`.
7. **`typeTerm` and `isTarget` use `expectIdent(p)` with its default `what`** (`identifier`), as the reference does; the default parameter works under antpy.

Everything else is as planned: the AST names (`StringLit`, `Module`, `LitEntry`, `TypeDecl`, `PosArg`/`NamedArg`, `FtParam`, `FieldType`, `Alias`), the `typ` field for `type`, the positions of operator-built nodes at the operator token and of a path record literal at the first identifier, the template-part offset rule (lexed at (1, 1), tokens shifted afterwards), `pyRepr` as specified (quote choice, `\\`, `\'`/`\"`, `\n`, `\r`, `\t`, `\xNN` for other ASCII controls), the Python side of the contract copied literally, and the `tests/err` rule in `compare()`.

## Known limitations and divergences from the reference

- **`pyRepr` escapes only ASCII control characters.** Python's `repr` also escapes non-printable code points outside ASCII (`\x80`-`\x9f`, ` `, unassigned code points, ...) as `\xNN`/`\uNNNN`/`\UNNNNNNNN`; antc leaves them raw. The difference shows only in a `got string '...'`/`got template '...'` message whose token holds such a character, which no file in the repository does (`--all`). `work/1.2/probes/got_string_unicode.ant` covers a printable non-ASCII character (`é`), which both leave raw.
- **A lexer error inside a `${...}` body is reported at part-local coordinates** (`work/1.2/probes/tpl_lex_error.ant`: `1:4: unexpected character '@'` for a part that starts at column 14), while a parser error on the part's tokens is at file coordinates (`tpl_parse_error_line2.ant`: `2:6: unexpected ')' in template expression`). This mirrors the reference, which lexes a part before applying the offset (`antparse.py` lines 283-289); it is wrong by design and the oracle checks it. A later step that reports lexer errors inside templates at file positions must change both sides.
- **`x is func` at the end of a line does not end the statement** (`work/1.2/probes/is_func_eol_err.ant`: `var i = a is func` followed by `var j = 1` fails at `2:1: expected newline or ';', got 'var'`). The lexer's newline rule treats `func` as a keyword that cannot end an expression, which is true everywhere except after `is`. Both sides agree; it is a lexer-level reading of ant-lang.md section 2 ("the line ends with a token that cannot end an expression") that the suite never exercises. Parenthesize (`(a is func)`) or continue the line.
- **antpy speed**: `antc parse src/antc/parser.ant` (30 KB, the largest source in the repository) takes 0.9 s wall under antpy; `--all` with `-j` takes about a minute. Not a concern; M2 makes antc native.
- Invalid UTF-8 and `/dev/stderr` truncation: as in the 1.1 report.

## Probes (`work/1.2/probes/`, `work/1.2/probes.log`: 72 files, 0 failures)

Hand-written edge cases, each agreed on both sides (error probes hold one error each, since parsing stops at the first):

- Templates: a lexer error inside `${}` (part-local position), a parser error on the second line of a part (file position), a part followed by junk (`unexpected int '2' in template expression`), an empty `${}` (`unexpected end of file` at the part's position), nested templates with their own parts three levels deep, multi-line parts holding a lambda, a map literal and an array across lines.
- Brace literals: `B{}.x`, `B{ x: 1 }.x.y`, `B{}(1)`, `B{}[0]` (the literal from `primary` continues through `postfix`); `A.B{}.x` (error at the `.`: the literal from `postfix` returns at once); `A.B{}`, `A.B.C{}`; `a.b(1).c{ x: 1 }` (error: a call ends the path); `Map{ name, (name): 1, "k": 2, 3: 4, a.b: 5 }` (a `Field` key is allowed in a map), `X.Map{ a: 1 }` (a record), `Map{}`, a trailing comma; `Map{ ...m }` (error after the `...`).
- Calls: `f(a: 1, b)` (error at the `)`), `f(a.b: 1)` (error at the `:`), `f(x: 1,)`, nested named arguments, `f(\n 1,\n 2,\n)`, `f?.(1)?.x?.[0]`, missing commas in calls and arrays (`expected ',', got int '2'`).
- `return`: `x ?? return y + 1`, `[return, 1]`, `(return)`, `{ return }`, `Map{ k: return }`, `return; f(1)`, `return` at the end of the file without a newline (then `expected '}', got end of file`).
- Logic: `a && b || c && d`, `a || b && c`, `a ?? b ?? c`, `a || b || c && d && e`, parenthesized mixes, `!a && -b || c`; the two mix errors (`x ?? y && z`, `x || y ?? z`, both at column 16).
- `if`/`else` chains on one line, with `else` on its own line, as a value, multi-line; headers with a parenthesized record literal, a lambda argument, an `is` chain with a namespaced target, an assignment (`if count = 5 {`), a record literal inside a call, a map literal inside a call, a field path, an index, unary `-` and `try`; a bare record literal in a header (error at the `:`); a header ending at a newline (error: `expected '{' after header, got newline`).
- Types: function types with named and unnamed parameters, `func(T): A | B` (the union is the return type), `(func(T): A) | B`, a function returning a function, a parenthesized union return type, type arguments across lines, `(int)` and `((int | string))` (no wrapper node), `A.B.C[A.D]`; `func(a.b: int): int` and `func(Array[int]: int): int` (errors at the `:`).
- Declarations: `import` with a trailing comma, an empty item list, items across lines, a namespace import ended by `;`, `var` with a type, destructures (including empty), `export var` with a union type, default parameters with a trailing comma, bounded type parameters with a trailing comma; errors: `import "./a.ant" { x: }`, `import 42 X`, `export 1`, `type`/`export`/`import` inside a block, `import` after a declaration, a named `func` inside a block.
- Loops: `for { a: b, c } in m`, `for { } in m`, `for x in [1, 2]`; `for { a: { b } } in m` and `for 1 in xs` (errors).
- Unary and binary: `try -x`, `!!a`, `-(-a)`, `- -a`, `try f(x) | g()`, `!a == b`, precedence chains, `a is null`, `(a is func)`, `a is A.B`, `a is func && b`; `x is void` and `x is {}` (errors); every comparison and arithmetic operator in one expression, compound assignments to fields and elements; `a = b = c` and `a += b -= c` (errors at the second operator).
- Pipes: `xs | { x }()` (G23), a chain with namespaced calls, a leading-`|` continuation, `xs | f()(1)`; `xs | f` and `xs | f()[0]` and `xs | f?.()` (errors: the right side is not a `Call`).
- Newlines: a line starting with `(`, `[`, `.`, `?.`, `|`, a trailing binary operator, comment lines between an operator and its operand; semicolons in every position; `1 +` at the end of the file (`unexpected newline`); newlines inside every list form; lambdas in every position; keyword expressions as values and as arguments (`var y = while false {}`, `f(if c { 1 } else { 2 }, go {}, defer {}, func() {}, {})`); 30 levels of parentheses, brackets and lambdas.
- `describe`/`pyRepr`: `got string` with a single quote inside (`"it's"` printed with Python's double quotes), with escaped double quotes, with both quote kinds, with a backslash and a tab, a non-ASCII letter, a template holding a newline (`\n`), `got float`, `got template`, `got int`, `got newline`, `got end of file`, `got 'else'`, `got 'in'`.

## Refactor

A single-pass simplify review (the `simplify` recipe run inline, without its four-agent fan-out) over the step's diff, then the reviewer's three suggestions, each without a change in behavior. `src/antc/parser.ant` is 950 lines (was 947: four helpers added, their fourteen duplicate sites removed), `src/antc/dump.ant` 194 (was 193). `ast.ant`, `main.ant` and `tools/compare_frontend.py` are unchanged by this pass.

Simplify pass (`parser.ant`):

- `isWord(t, value)`, the "operator or keyword with this value" predicate, now backs both `at` and `expect`; `expect` no longer re-spells it negated.
- `failGot(t, message)` builds the `<message>, got <token>` form of every "expected ..." error; its ten inline copies (`expect`, `expectIdent`, `expectTerm`, `parseFile`, `importDecl`, `declaration`, `varDecl`, `typeTerm`, `isTarget`, `headerExpr`) call it, and the three that peeked twice (`fail(p, ... describe(peek(p)))`) now peek once.
- `lambda` is `block` re-tagged (`Ast.Lambda{ line, col, stmts }` from the parsed `Ast.Block`) instead of a line-for-line copy of it.
- `indexExpr(p)` parses `[ expression ]` with newlines ignored, shared by the `?.[` and `[` branches of `postfix`.
- Skipped, noted: the list-with-separators shape repeated across ten grammar functions mirrors the reference method by method (the header's rule); the two `is Ok`-or-exit unwraps in `main.ant` would need a bounded generic to share; `hex2`'s digit table is rebuilt per call but only on an error path.

Reviewer suggestions:

1. `dump.ant`: `optTyp(t: Ast.TypeExpr | null)` next to `optExpr`; `varDecl`, `tparam` and `signature` use it, `param` uses `optExpr`; `alias` keeps its inline form since it quotes.
2. `parser.ant`: `identPath(p)` (first name via `next(p).value`, then `. IDENT` pairs through `expectIdent`) shared by `typeTerm` and `isTarget`; `postfix` keeps its own loop since it interleaves `Field` nodes and the header check.
3. `parser.ant` `expression`: the assignment-operator test is `atAssign(p)` (the position still comes from `var t = peek(p)`), so `ASSIGN_OPS` is consulted in one place.

Checks: the subset `python3 -I tools/compare_frontend.py parse tests/ok tests/err src/antc` (48 files, antc parsing its own refactored sources among them) after the simplify pass and after the reviewer edits, `0 failures` each time; the acceptance command `python3 -I tools/compare_frontend.py parse --sample` at the end (`work/1.2/refactor.log`, exit 0), last line `85 files, 0 failures`; and the probe directories `work/1.2/probes work/1.1/probes`, which exercise the error messages `failGot` now builds, `127 files, 0 failures`.
