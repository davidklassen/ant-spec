# Report for step 1.1 "antc-lexer": the lexer in Ant, `antc lex`, `compare_frontend.py lex`

## What was built

| File | Lines | Content |
|---|---|---|
| `src/antc/lexer.ant` (new) | 393 | the exported types `Token`, `Part`, `LexError`; the private `Lexer` state record; the word tables `KEYWORDS`, `RESERVED`, `ENDING_KW`, `OPS2`, `OPS1` copied from `tools/antparse.py`; `tokenize(src, startLine, startCol): Result[Array[Token], LexError]`; the state helpers `charAt`, `peekc`, `advance`, `emit`, `text`, `errAt`, `err`; the ASCII character classes `isDigit`, `isAlpha`, `isAlnum`, `isHex`; the newline rule `lastContinues` and `nextLineContinues`; the scanners `blockComment`, `number`, `decDigits`, `identifier`, `operator`, `escape`, `stringLit`, `templateLit`, `embedded` |
| `src/antc/main.ant` (new) | 69 | `main` dispatching `lex FILE` (and refusing `parse` with exit 2 until step 1.2, any other usage with exit 2), `lexCommand`, the token `dump`, `esc`, `report` (the single `path:line:col: message` diagnostic) and `eprint` (stderr through `File.create("/dev/stderr")`) |
| `tools/compare_frontend.py` (new) | 180 | the `lex` mode: selection (`--sample`, `--all`, paths and directories), the reference dump from `antparse.Lexer(src).tokenize()`, the antc run under antpy in a thread pool (`-j N`), the verdict rule, unified diffs on a mismatch, `-v`, the last line `<N> files, <M> failures`; `parse` mode exits 2 with `parse mode arrives in step 1.2` |
| `work/1.1/probes/*.ant`, `work/1.1/probes.log` | 53 files | hand-written edge cases (see Probes); all agree |

The lexer mirrors `tools/antparse.py`'s `Lexer` function by function, so a reader can hold the two side by side. Scanners return `Result[void, LexError]` (`embedded` returns `Result[Part, LexError]`) and the main loop joins them with `try`; every error record carries the position the reference reports, which differs by kind: reserved words and `_` after the word, unterminated strings and templates at the opening quote, an unterminated `${` just after it, a block comment at its `/*`, a bad escape at the character after the backslash, a malformed number and an unexpected character at the current position. Token values are raw source text (nothing decoded), and a TEMPLATE token carries its `${...}` bodies as `Part{ line, col, text }` with the position just after the `${`.

The dump contract shared by `main.ant`'s `dump` and `compare_frontend.py`'s `lex_dump`: one line per token, `<line>:<col> <KIND>` plus ` <value>` when the value is non-empty, then `  part <line>:<col> <text>` per template part; backslash, LF and CR escaped as `\\`, `\n`, `\r`, nothing else.

Work followed the plan's order: the compare tool with the reference side first (every file failed as a crash before `antc` existed, which fixed the contract), then whitespace/comments/identifiers/operators, numbers, strings and escapes, templates and `embedded`, the seven error files, `--sample`, `--all`, probes.

## Acceptance

Command, verbatim: `python3 -I tools/compare_frontend.py lex --sample` (output in `work/1.1/test.log`, exit 0). Last line:

```
85 files, 0 failures
```

(8 `tests/ok` files, 35 `tests/err` files, 42 suite files = every 14th of the 577 under `src/tests`.)

The 1.G gate command for this mode, run early as the plan asked (`work/1.1/all.log`, exit 0), last line:

```
720 files, 0 failures
```

(665 `.ant` files in the repo before this step, plus `src/antc/lexer.ant` and `src/antc/main.ant`, plus the 53 probes under `work/1.1/probes/`; antc lexes its own sources identically to the reference.)

## Test changes

None. No test contradicted the spec; nothing under `src/tests` or `tests/` was touched.

## Decisions and deviations from the plan

1. **The lex-mode reading of the roadmap's "files under `tests/err` must be rejected by both".** That sentence describes parse mode: a lexer accepts most of those files (the reference lexer accepts 28 of the 35 `tests/err` files and rejects exactly 7: `bad_escape` 2:11, `match_reserved` 3:8, `octal` 2:10, `reserved_word` 2:10, `string_newline` 2:9, `underscore` 2:6, `unterminated_block_comment` 2:1). So in lex mode the tool applies one rule to every file regardless of its directory: if the reference lexer raises, antc must exit 1 with a diagnostic at the same `line:col`; otherwise antc must exit 0 and print the identical dump. Parse mode (step 1.2) is where the rejection of every `tests/err` file is enforced.
2. **`Lexer` keeps no `src` field; token text is `String.concat(Array.slice(lx.chars, start, lx.pos))`** (plan: keep `src` and use `String.slice(src, start, pos)`). The lexer already walks `String.chars(src)`, so the token text is the join of those code points; this avoids holding the source twice and a second code-point walk per token, and costs the same under antpy and under the M2 runtime. Columns still count code points, as Python's `str` does in the reference.
3. **`report()` delegates to a separate `eprint()`.** `report` stays the one diagnostic shape (`path:line:col: message`) for M3's `antc check` to reuse, while `eprint` is the plain "a line on stderr" primitive that the usage message and the `parse` stub also need. `eprint` opens `/dev/stderr` with `File.create` (the only way the spec'd library reaches stderr) and falls back to `print` if that fails.
4. **The scanners are named `stringLit`/`templateLit`/`blockComment`** instead of the plan's `string`/`template`: `string` would shadow nothing but reads like a type name; `blockComment` factors the `/* */` skip out of the main loop so the loop stays one branch per token class.
5. **`tokenize` takes `(src, startLine, startCol)`** as planned; `main.ant` passes `(1, 1)`. See the note for step 1.2 below before using the start position for template parts.

Everything else is as planned: ASCII character classes (`isDigit`, `isAlpha`, `isAlnum`, `isHex`, each false for `""`); every "is this char in the set" test excludes `""` first because `String.contains(s, "")` is true; `0x`/`0b` prefixes lowercase only (`0X1` is INT `0` then a malformed number); `0x` at end of file is accepted as INT `0x`, mirroring the reference's `"" not in digits`; `\u{...}` escapes are counted, not validated (G2 belongs to the checker); a nested template's own parts are discarded inside `${}`; comments are not recognized inside `${}`; the final NEWLINE before EOF only when the last token is not already one and at least one token exists.

## Known limitations and divergences from the reference

- **Error messages are compared with one normalisation** (since the refactor below): the reference's `unexpected character {c!r}` uses Python's `repr`, which antc cannot reproduce for every character, so the tool decodes it and compares it against antc's `unexpected character '<c>'`; every other message must match verbatim. The tool still prints both messages under `-v` (`work/1.1/probes.log`).
- **ASCII-only identifiers and digits.** The reference uses Python's Unicode-aware `str.isalpha`/`isdigit`/`isalnum` (`antparse.py` lines 113, 136-138, 171-194); ant-lang.md section 2 ("An identifier starts with an ASCII letter or `_` and continues with ASCII letters, digits, or `_`") sides with ASCII, so a non-ASCII letter is an `unexpected character` for antc and the start of an identifier for the reference. No file in the repo has a non-ASCII character outside strings and comments (verified by `--all`), so the divergence is invisible to the gate; a future test with a non-ASCII identifier would show it, and antc is the side that follows the spec.
- **Invalid UTF-8 is rejected by antc and accepted by the tool.** `File.read` returns `Err invalid UTF-8: <path>`, which `antc lex` reports as `<path>:1:1: invalid UTF-8: <path>` with exit 1, while `compare_frontend.py` decodes with `errors="replace"` and the reference lexes the replacement characters. No repo file has invalid UTF-8; the tool reports such a file as a failure, which is the right signal.
- `File.create("/dev/stderr")` reopens fd 2 with `O_TRUNC`, harmless under pipes and terminals (how every runner uses antc); redirecting `antc` with `2>file` would truncate that file at each diagnostic.
- Performance is not a concern under antpy (33 KB in 0.14 s wall clock), but `advance` is a loop of single steps; the M2 native build makes this moot.

## Note for step 1.2: template parts and error coordinates

The reference reports a lexer error inside a `${...}` body at **part-local** coordinates, not file coordinates: `python3 -I tools/antparse.py` on a file holding

```
var t = `a ${ 1 @ 2 } b`
```

prints `line 1:4: unexpected character '@'`, not `1:17`. The cause is `Parser.__init__` (`antparse.py` lines 283-289): the part's `(line, col)` offset is applied to the tokens only *after* `Lexer(src).tokenize()` has succeeded, so an error raised by the lexer carries the body's own positions, while the parser's later errors on those tokens carry file positions. A parser that passes the part position into `tokenize(src, line, col)` would therefore disagree with the reference in parse mode on such files. Lex each part at `(1, 1)` and offset the resulting tokens afterwards, exactly as the reference does (`col + partCol - 1` on line 1, `line + partLine - 1` everywhere), or document the divergence in the 1.2 report. In lex mode this cannot show: `antc lex` only records the parts' positions and text (the dump above shows `part 1:14  1 @ 2 ` for that file, same as the reference).

## Probes (`work/1.1/probes/`, `work/1.1/probes.log`: 53 files, 0 failures)

Hand-written edge cases, each agreed on both sides: an empty file and a comment-only file; `/*` unterminated at end of file (in column 1 and mid-line) and a `//` ending at EOF; the newline rule in `continuation.ant` (a line continued by `.`, `?.`, `|`, `||`, `else`, with blank and comment-only lines in between; `elsewhere` and `...` not continuing; the ending keywords `return true false null void break continue` each ending a line; trailing binary operators continuing; `x ?` then `.y`, an `unexpected character '?'`), runs of newlines, no trailing newline, CR line endings and a CR inside a string, tabs, semicolons, a closing `)`/`]`/`}` followed by a line opening with `(`, `[` or `{`; deep brace nesting inside `${}`; every keyword and the identifiers `int string never print _x x_ _1 __ A1 break2` (`keywords.ant`), a reserved word mid-line (`await`) and `_` mid-line, both reported after the word; every operator and the unexpected characters `& @ # ?` and `—` (non-ASCII, reported at its code-point column); numbers in every form and the malformed cases `0X1`, `0b12`, `1e`, `1e+`, `1e` at end of file, `1.5f`, `0x_1`; strings with every escape, `\q`, a backslash at EOF, `\u{}`, `\u{` without its `}`, a long `\u{000000000041}` and the unvalidated `\u{D800}`; a string holding `${`; a Unicode string; templates with escapes, multi-line parts, a part that is the whole template, a nested backtick template, a part holding a string with a newline (an `unterminated string` at the string), a part ending at EOF inside a string, a `}` inside a `//` comment in a part (which closes the part, then the template is unterminated), unterminated templates and parts.

## Refactor

A `simplify` pass (single pass, inline; the four-agent fan-out was unavailable) over the three new files, then the reviewer's suggestion, with the subset `lex tests/ok tests/err work/1.1/probes` rerun after each batch and `--sample` and `--all` at the end.

- `src/antc/lexer.ant` `tokenize`: the per-iteration `line`/`col` copies are gone; the NEWLINE token is emitted before `advance`, so `lx.line`/`lx.col` are its position.
- `src/antc/main.ant` `eprint`: `File.write("/dev/stderr", text + "\n")` replaces create/writeLine/close (the same truncating open, the same stdout fallback).
- `tools/compare_frontend.py`: `reference()` lost its dead `raise SystemExit` (`main()` already refuses parse mode); `compare()` builds the `reference:`/`antc:` detail once for the error and crash branches.
- Reviewer suggestion: in the both-error branch the messages must now be equal as well as the positions. `same_message()` normalises only the reference's `unexpected character {c!r}` (Python repr, decoded with `ast.literal_eval`) to antc's `'<c>'` form, so the character is still compared; every other message must match verbatim. The `-v` echo of both messages is unchanged. Two probes were added for the characters whose repr differs most from antc's quoting: `unexpected_quote.ant` (`"'"` against `'''`) and `unexpected_backslash.ant` (`'\\'` against `'\'`); 55 probes now.
- Considered and skipped: removing the counting loop in `advance` and the `charAt` call in `peekc` (hot path, but `antc lex` on the largest file in the repo, 12 KB, takes 0.27-0.33 s wall, almost all interpreter start-up, and both would drift from the reference's shape); a default `k = 1` on `advance` (style only, about 25 call sites); inlining `errAt` (no net saving).

After the refactor: `python3 -I tools/compare_frontend.py lex --sample` prints `85 files, 0 failures` and exits 0 (`work/1.1/test.log`); `--all` prints `722 files, 0 failures` and exits 0 (`work/1.1/all.log`: the 720 before plus the two probes).
