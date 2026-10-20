# Report for step 1.G "frontend gate"

Acceptance command, verbatim: `python3 -I tools/compare_frontend.py lex --all && python3 -I tools/compare_frontend.py parse --all` (`work/1.G/test.log`, exit 0; last lines `797 files, 0 failures` for each mode). antc lexes and parses every `.ant` file in the repository, its own sources included, identically to `tools/antparse.py`.

## Milestone simplify

Four reviewers (reuse, simplification, efficiency, altitude) covered M1's code; their 29 findings reduce to 17 mechanisms. Work went in two groups, each followed by the full gate: the shared-rule consolidations (`work/1.G/simplify-group1.log`: `797 files, 0 failures` twice), then the efficiency changes (`work/1.G/test.log`, the final tree: `797 files, 0 failures` twice). Every token dump, AST dump, diagnostic message and position, and exit status is unchanged: the gate is the proof for the 797 files, and the usage error (exit 2), the missing-file diagnostic at `1:1` (exit 1), `parse` without `--dump` (silent, exit 0) and a lexer error were checked by hand. `src/antc` went from 1708 to 1587 lines (lexer 391 to 372, parser 950 to 859, dump 194 to 206, main 80 to 57).

### Applied: shared rules written once

| Findings | Change |
|---|---|
| 2, 10, 11, 22 | One diagnostic record: `Lex.Diagnostic { line, col, message }` (lexer.ant, renaming `LexError`) is the error type of `tokenize`, `parse`, `parsePart` and every parser function. `ParseError`, `fromLex` and the two `Result.mapErr` wraps are gone. main.ant has one `orExit[T](path, r: Result[T, Lex.Diagnostic]): T` holding report-and-exit; `readSource` maps its `File.read` error to a `Diagnostic` at 1:1 and uses it too, so the three unwrap blocks are one. `report(path, d: Lex.Diagnostic)`. |
| 1, 14, 21 | `delimited[T](p, open, close, item: func(Parser): Result[T, Lex.Diagnostic])` (expect open, enter list scope, items with the trailing-comma rule, expect close, leave) replaces the ten spelled-out lists; `grouped[T](p, open, close, inner)` the three single-item brackets (`indexExpr`, the parenthesized type, `Paren`). Item parsers are named (`typeParam`, `param`, `fieldType`, `ftParam`, `elem`, `litEntry(p, isMap)`, `typeExpr`) or lambdas (`{ aliasItem(x, "export name") }`); `callArgs` passes an anonymous `func` that captures `seenNamed`. Sites that need the opener's position take `peek(p)` first. The helpers make the same `peek`/`expect` calls in the same order, so positions and messages are unchanged (the one drifted copy, `typeTerm`'s `next` for `[`, now goes through `expect`, equivalent after the `at` check). The header comment of parser.ant says so. |
| 5 | `optionalAfter[T](p, sep, parse): Result[T \| null, ...]` for the five `: type` / `: local` / `= default` suffixes. |
| 4, 19 | `binLevel(p, ops, operand)` with one `peek` per iteration and `p.i += 1` on the peeked token; `equality`, `additive`, `multiplicative` are one-line calls with `EQUALITY_OPS`, `ADDITIVE_OPS`, `MULTIPLICATIVE_OPS`. `expression` tests the token it already peeked (`atAssign` gone); `pipeOperand` peeks once before branching on `??` vs `&&`/`\|\|`; `comparison` advances with `p.i += 1`. |
| 3, 13, 28 | `atOp` and `atKw` deleted (their ten sites call `at`); the eleven inline `t.kind == "OP" && t.value == x` forms are `isWord(t, x)`; the multi-word tests go through `isAny(t, words)` over tables beside `ASSIGN_OPS` (`COMPARE_OPS`, `UNARY_OPS`, `POSTFIX_OPS`, `RETURN_ENDERS`, `DECL_KWS`, `LITERAL_KWS`), so `isCompareOp`, `isPostfixOp` and `endsReturn` are gone. Equivalent because the lexer gives the token kinds disjoint value sets. |
| 29 | `expectKind(p, kind, what)` holds the kind test, `failGot` and the advance; `expectIdent` is its IDENT case, `importDecl` its STRING case, and `typeTerm`/`isTarget` use it for their first identifier, with `identPath(p, first)` parsing the rest of a path. |
| 16 | `enter(p, nl)` replaces `enterList`/`enterBlock`: `block` enters with 0, `delimited`/`grouped` with `p.nl + 1`. |
| 7, 12 | The token dump is `Dump.tokens(toks): string` in dump.ant beside `module`; `esc` lives there and `quote` is `esc` plus the `"` escape (byte-identical: `esc` doubles backslashes before the quote step adds one, and none of the four replacements produces another's target). main.ant is only the CLI. compare_frontend.py's `quote` is likewise `esc` plus `\"`, and its docstring says so. |
| 8, 25 | `orNil[T](x: T \| null, render)` is the one null-to-`nil` rule; `opt`, `optExpr`, `optTyp`, the inline form in `alias` and the tail of `ifExpr` (now `orNil(e.otherwise, elseBranch)`) are gone. |
| 9 | `tokenize(src)` lexes from (1, 1); the parser's post-shift of a part's tokens is the one place the template offset lives. |
| 6, 15, 18 | `isIdentChar(c)` is the identifier-continuation rule (the three ASCII ranges plus `_`, written once; `isAlnum` and the three guarded spellings are gone). `scanWhile(lx, pred): int` steps over a run of characters directly (`lx.pos += 1`, one column bump for the run) and serves `identifier`, `decDigits`, the prefixed digits of `number`, the `\u{` hex digits of `escape` and the `//` skip. |
| 23 (supersedes 17) | The continuation rule moved into `emit`: a `.`, `?.`, `\|`, `\|\|` or `else` token that follows a NEWLINE replaces it (`CONTINUING` table), and `tokenize` emits a NEWLINE on `!lastContinues` alone. `nextLineContinues`, the second trivia scanner, is deleted, which also removes the quadratic rescan of a comment gap before a continuation line: a 200-line gap went from 0.48 s to 0.09 s and an 800-line gap from 6.24 s to 0.11 s under antpy. Stream equivalence: a NEWLINE is never emitted twice in a row, `...` is its own token, `elsewhere`/`else_` are IDENTs, no OP is emitted from inside a template, and an unterminated comment or stray `?` after the newline errors at the same position either way. |
| 24 | Scanning and emitting are separate: `scanToken` takes a `Mark { pos, line, col }` before dispatching, each scanner steps over its lexeme and returns its kind (`number`, `identifier`) or nothing (`stringLit`, `operator`; `templateLit` returns its parts), and `scanToken` emits once from the mark. `embedded` calls the string and template scanners with nothing to pop; errors at a token's start use `errAt(m, ...)`. `charAt` folded into `peekc` (its only remaining user). |

### Applied: efficiency

| Findings | Change |
|---|---|
| 18, 19 | Covered above: direct stepping in `scanWhile`, `isIdentChar` as one predicate, one `peek` per binary-operator iteration. |
| 20 | `step(lx)` is the loop-free one-character advance (20 sites); `advance(lx, k)` loops over it for the 2- and 3-character cases. `isAlnum`'s two calls are gone with it. `peekc` keeps the one bounds check (now its own body). |

Measured under antpy on `src/antc/parser.ant` (wall, best of two): `antc lex` 0.63 s before, 0.50 s after; `antc parse --dump` 1.02 s before, 0.82 s after.

### Skipped

- **26 (option parsing in main.ant).** The next commands' shapes do not fit the proposed scheme (`build ... FILE -o OUT` takes a valued option after the positional), so an option parser written now would be rewritten by M2 with the commands that need it; today's dispatch is three conditions. Left for 2.1.
- **27 (one depth for Python-repr normalisation).** Both proposed routes change antc's stderr for tokens holding a quote or control character (the Python-side route makes `describe` print raw values; the Ant-side route makes `unexpected character` print Python's repr). The pass keeps every message byte-identical, and the 1.2 report documents the remaining divergence.
- **17 (cache in `nextLineContinues`).** Superseded by 23, which deletes the function.
- **21, the `withHeader` part.** `headerExpr`'s save/restore of `header` has one site; a helper would add a name without removing a copy.
