# Plan for step 1.1 "antc-lexer"

Acceptance: `python3 -I tools/compare_frontend.py lex --sample` exits 0 and its
last line reports `0 failures`.

Deliverable: `src/antc/lexer.ant` (the lexer in Ant, run under antpy),
`src/antc/main.ant` (the `antc` CLI with the `lex` command), and
`tools/compare_frontend.py` with the `lex` mode. The oracle is the `Lexer`
class in `tools/antparse.py` (lines 56-280): the Ant lexer mirrors it function
by function, including where each error is positioned. Nothing is decoded:
token values are raw source text, exactly as the reference keeps them.

Facts gathered while planning (so the implementer need not re-measure):

- antpy lexes a 33 KB file char by char in 0.14 s wall clock (startup 0.05 s),
  `String.chars` indexing is faster than `String.toBytes`. Performance is not
  a concern; keep the code plain.
- 665 `.ant` files in the repo; 577 under `src/tests` (`--sample` picks 42 of
  them plus 8 `tests/ok` and 35 `tests/err` files = 85). The reference lexer
  rejects exactly 7 files, all in `tests/err`: bad_escape (2:11),
  match_reserved (3:8), octal (2:10), reserved_word (2:10), string_newline
  (2:9), underscore (2:6), unterminated_block_comment (2:1). Every other
  file lexes; no file has CR, tabs, or a non-ASCII identifier; non-ASCII
  text occurs only inside strings and comments. Templates have at most 3
  parts; two templates nest a backtick template inside `${}`.
- Verified under antpy: `try` inside `while` in a function returning
  `Result[void, LexError]` with a user record as the error type; record
  fields mutated through helper functions (`lx.pos += 1`); `Array.pop`;
  `String.slice` by code point; `String.replace` of backslashes;
  `File.create("/dev/stderr")` + `File.writeLine` reaches stderr, also when
  the process is driven by `subprocess` with pipes; `Os.exit(1)` after
  `print` keeps stdout.

## Files

| File | Action |
|---|---|
| `src/antc/lexer.ant` | create: tokens, lexer state, scanners, `tokenize` |
| `src/antc/main.ant` | create: `antc lex FILE`, token dump, error report; `parse` stub |
| `tools/compare_frontend.py` | create: `lex` mode, `--sample`/`--all`/paths, parallel runs, diff report; `parse` mode stub for 1.2 |
| `work/1.1/report.md` | write at the end |

Do not touch `tools/antparse.py` (read-only); import it.

## The shared dump contract

Both sides print one line per token, in order, nothing else on stdout:

```
<line>:<col> <KIND>            when the value is empty (EOF)
<line>:<col> <KIND> <value>    otherwise, value escaped
  part <line>:<col> <text>     one per `${...}` part, right after its TEMPLATE token
```

KIND is the reference's kind name: `OP KW IDENT INT FLOAT STRING TEMPLATE
NEWLINE EOF`. Escaping (`esc`) replaces `\` with `\\`, LF with `\n`, CR with
`\r`, in that order, nothing else; so a NEWLINE token prints as
`12:7 NEWLINE \n` and a multi-line template stays on one line. No trailing
spaces. Example for `var x = 42`:

```
1:1 KW var
1:5 IDENT x
1:7 OP =
1:9 INT 42
1:11 NEWLINE \n
1:11 EOF
```

On a lexer error `antc lex` prints nothing on stdout, writes
`<path>:<line>:<col>: <message>` to stderr, and exits 1. The reference side
is `ParseError(line, col, msg)` from `Lexer(src).tokenize()`.

Comparison rule for `lex` mode (the roadmap's "files under tests/err must be
rejected by both" is a parse-mode statement; a lexer accepts most of them):
for every selected file, if the reference lexer raises, `antc lex` must exit
1 and report the same `line:col` (messages are printed in the report but not
compared, because the reference's `unexpected character {c!r}` uses Python's
repr); otherwise `antc lex` must exit 0 and print exactly the reference dump.
Anything else (exit 2 = antpy panic, timeout, extra stderr) is a failure.

## `src/antc/lexer.ant`

Types (exported, the parser of 1.2 uses them):

```ant
export type Token = { kind: string, value: string, line: int, col: int, parts: Array[Part] }
export type Part = { line: int, col: int, text: string }       // one ${...} body: position after "${", raw text up to the matching "}"
export type LexError = { line: int, col: int, message: string }
```

Kinds are the reference strings (`"OP"`, `"KW"`, ...). `parts` is `[]` for
every kind but TEMPLATE. Private state:

```ant
type Lexer = { src: string, chars: Array[string], n: int, pos: int, line: int, col: int, toks: Array[Token] }
```

`chars = String.chars(src)` (one code point each, so columns count code
points exactly like Python's `str`); `src` is kept for `String.slice(src,
start, pos)`, which is the token text. Tables as top-level `var`s:
`KEYWORDS`, `RESERVED`, `ENDING_KW`, `OPS2` as `Map{ "word": true, ... }`
queried with `Map.has`; `OPS1 = "+-*/%<>=!|.,:;()[]{}"` queried with
`String.contains(OPS1, c)` (only with a non-empty `c`: `String.contains(s,
"")` is true). Copy the word lists from `antparse.py` lines 11-23 verbatim.

Entry point:

```ant
export func tokenize(src: string, line: int, col: int): Result[Array[Token], LexError]
```

`line`/`col` are the starting position (1, 1 for a file; the 1.2 parser
passes a part's position so template bodies get file coordinates, which is
what `Parser.__init__`'s offset loop computes in the reference). On success
`Result.ok(lx.toks)`.

Helpers, all taking `lx: Lexer` first:

- `peekc(lx, k): string`: `lx.chars[lx.pos + k]` or `""` past the end.
- `advance(lx, k): void`: k times: if `pos < n`: `"\n"` bumps `line` and
  resets `col` to 1, anything else bumps `col`; `pos += 1`.
- `emit(lx, kind, value, line, col, parts): void`: `Array.push(lx.toks, Token{...})`.
- `errAt(line, col, message): LexError` and `err(lx, message)` = at the
  current position. Scanners return `Result[void, LexError]` and the main
  loop and callers use `try`; the error record carries the position the
  reference would report (listed below, read `antparse.py` next to each).
- Character classes, ASCII only, comparing one-code-point strings:
  `isDigit(c)` = `c >= "0" && c <= "9"`; `isAlpha(c)` = a-z, A-Z;
  `isAlnum(c)`; `isHex(c)`. Each is false for `""`. (The reference uses
  Python's Unicode-aware `isdigit`/`isalpha`; the spec says ASCII and no
  file in the repo has a non-ASCII identifier, so ASCII is the right
  reading; note the divergence in the report.)

The main loop (`tokenize`), mirroring `Lexer.tokenize`:

```
while lx.pos < lx.n:
  c = chars[pos]; line = lx.line; col = lx.col
  "\n"                      -> if !lastContinues(lx) && !nextLineContinues(lx) { emit NEWLINE "\n" at (line, col) }; advance 1
  " " "\t" "\r"             -> advance 1
  "/" followed by "/"       -> advance while pos < n && chars[pos] != "\n"
  "/" followed by "*"       -> advance 2, then advance until peekc(0) == "*" && peekc(1) == "/"; if peekc(0) == "" first -> error "unterminated block comment" at (line, col) of the "/*"; then advance 2
  isDigit(c)                -> try number(lx)
  isAlpha(c) || c == "_"    -> try identifier(lx)
  "\""                      -> try string(lx)
  "`"                       -> try template(lx)
  else: three = c + peekc(1) + peekc(2); two = c + peekc(1)
        three == "..."      -> emit OP "..." ; advance 3
        Map.has(OPS2, two)  -> emit OP two ; advance 2
        String.contains(OPS1, c) -> emit OP c ; advance 1
        else                -> error `unexpected character '${c}'` at the current position
after the loop: if toks is non-empty and the last kind != "NEWLINE" { emit NEWLINE "\n" at (lx.line, lx.col) }
emit EOF "" at (lx.line, lx.col)
```

`lastContinues(lx): bool` (reference `last_continues`): no tokens -> true;
last kind NEWLINE -> true; OP -> value not one of `) ] } ; ...`; KW -> value
not in ENDING_KW (`return break continue true false null void`); any other
kind -> false.

`nextLineContinues(lx): bool` (reference `next_line_continues`): scan with a
local index `i` from `lx.pos` without advancing: skip `" " "\t" "\r" "\n"`;
skip `//` to the next `"\n"` (or end); skip `/*` to just after the next `*/`
(or to the end when unterminated); stop at anything else. Then with
`at(i)` = char at `i` or `""`: if the next three chars are `...` -> false;
if `at(i) == "."` or `at(i) == "|"` or (`at(i) == "?"` and `at(i+1) == "."`)
-> true; if the next four chars are `else` and `at(i+4)` is `""` or not
(alnum or `_`) -> true; else false. Note `|` includes `||` (the reference
tests `startswith("|")`); keep that.

`number(lx)` (reference `number`, read it line by line):

- start = pos, line/col saved; kind = "INT".
- If `c == "0"` and `peekc(1)` is `"x"` or `"b"` (lowercase only): digits =
  `"0123456789abcdefABCDEF_"` or `"01_"`; advance 2; `d = peekc(0)`; error
  "malformed number" at the current position when `d == "_"` or (`d != ""`
  and not `String.contains(digits, d)`) (the reference's `"" not in digits`
  is false, so `0x` at end of file is accepted as INT `0x`; mirror it);
  then advance while `d != ""` and `String.contains(digits, d)`.
- Else `decDigits(lx)` (advance while isDigit or `_`); if `peekc(0) == "."`
  and `isDigit(peekc(1))`: kind = "FLOAT", advance 1, decDigits; if
  `peekc(0)` is `"e"` or `"E"`: kind = "FLOAT", advance 1, optional `+`/`-`
  advance 1, then if `!isDigit(peekc(0))` error "malformed number" at the
  current position; decDigits.
- Then `nxt = peekc(0)`: if `nxt != ""` and (isAlnum(nxt) or `nxt == "_"`)
  -> error "malformed number" at the current position (this is what rejects
  `0o17`, `1x`, `0b12`, `1.5f`).
- emit kind, `String.slice(src, start, pos)`, at the saved position.

`identifier(lx)`: advance while isAlnum or `_`; word = slice; if
`Map.has(RESERVED, word)` -> error `reserved word '${word}'` at the current
position (after the word: `var class` reports 2:10, not 2:5); if `word ==
"_"` -> error `'_' is not an identifier` (also after); emit KW if
`Map.has(KEYWORDS, word)` else IDENT, at the saved start position.

`escape(lx)` (reference `escape`; called at a backslash, in both strings and
templates): advance 1; `c = peekc(0)`; if `c` is one of `n t r \ " ` $` ->
advance 1; else if `c == "u"` and `peekc(1) == "{"` -> advance 2, count hex
digits while advancing, then if count is 0 or `peekc(0) != "}"` -> error
`bad \u escape` at the current position, else advance 1; else error
`bad escape '\${c}'` at the current position (the char after the backslash;
`c` may be `""` at end of file). No code point validation here: `\u{D800}`
lexes (G2 belongs to the checker).

`string(lx)`: save line/col/start; advance 1; loop: `c = peekc(0)`; `""` or
`"\n"` -> error "unterminated string" at the saved start (not the current
position); `"\""` -> advance 1 and stop; `"\\"` -> `try escape(lx)`; else
advance 1. Emit STRING with the raw text including both quotes. `\r` inside
a string is allowed.

`template(lx)`: save line/col/start; `parts: Array[Part] = []`; advance 1;
loop: `""` -> error "unterminated template string" at the saved start;
backtick -> advance 1 and stop; `"\\"` -> `try escape(lx)`; `"$"` with
`peekc(1) == "{"` -> advance 2, `Array.push(parts, try embedded(lx))`; else
advance 1 (newlines are ordinary characters here). Emit TEMPLATE with the
raw text and `parts`.

`embedded(lx): Result[Part, LexError]`: save line/col/start (the position
right after `${`); depth 0; loop: `""` -> error `unterminated ${ in template
string` at the saved position; `"{"` -> depth += 1, advance; `"}"` -> if
depth == 0: text = slice(start, pos), advance 1, return `Result.ok(Part{
line, col, text })`, else depth -= 1, advance; `"\""` -> `try string(lx)`
then `Array.pop(lx.toks)`; backtick -> `try template(lx)` then `Array.pop(lx.toks)`;
else advance 1. Comments are not recognized inside `${}` (a `}` in a `//`
comment there closes the part); strings and nested templates are skipped
whole, and a nested template's own parts are discarded (the 1.2 parser
re-lexes the part text and finds them again). `string`, `template`, and
`embedded` are mutually recursive: annotate every return type (section 4,
Inference: recursion needs annotations).

## `src/antc/main.ant`

```ant
import "./lexer.ant" Lex
```

`func main()`: `args = Os.args()` (antpy gives only the arguments after
`main.ant`, so `lex FILE` arrives as `["lex", FILE]`). Dispatch on
`args[0]`: `lex` -> `lexCommand(path)`; `parse` -> report `parse: not
implemented until step 1.2` and `Os.exit(2)`; anything else or a missing
argument -> print a one-line usage (`usage: antc lex FILE | antc parse
[--dump] FILE`) to stderr and `Os.exit(2)`.

`lexCommand(path)`: `var read = File.read(path)`; `var text = if read is Ok
{ read.value } else { report(path, 1, 1, read.error.message); Os.exit(1) }`
(write it as an `if` expression whose else branch ends in `Os.exit`, which
has type `never`, so the checker of M3 sees `text: string`; do not rely on
narrowing after an `if` statement). Then `var r = Lex.tokenize(text, 1, 1)`
and the same shape for `r`; on `Err` report `e.line, e.col, e.message` and
exit 1; on `Ok` call `dump(r.value)`, return normally (exit 0).

`dump(toks: Array[Token]): void`: for each token `print` the line described
in the contract; `esc(s)` = three nested `String.replace` calls (`\\` first).
Build the line as `` `${t.line}:${t.col} ${t.kind}` `` + (`""` if the value
is empty else `" " + esc(t.value)`), then one `  part ...` line per entry of
`t.parts`.

`report(path, line, col, message): void`: `File.create("/dev/stderr")`; on
`Ok` write `` `${path}:${line}:${col}: ${message}` `` with `File.writeLine`
and `File.close`; on `Err` fall back to `print`. This is the one way an Ant
program can reach stderr with the spec'd library; the C runtime of M2 gets it
for free through `fopen`. Keep it the single diagnostic function so M3's
`antc check` reuses it.

Write checkable Ant throughout (3.4 runs `bin/antc check src/antc/main.ant`):
every parameter annotated, every function's return type written, empty
arrays annotated (`var parts: Array[Part] = []`), no lambda without an
expected type, `is` only on `Result` values, no name shadowing an import or
prelude name (`Lexer` as the private record type is fine, `Lex` is the
namespace; do not name anything `String`, `Array`, `Map`, `Error`, `Result`).

## `tools/compare_frontend.py`

Plain Python, standard library only, runs under `python3 -I`: put its own
directory on `sys.path` (as `tests/run_tests.py` does) and `import
antparse`. `ROOT` = the repository root from `__file__`; every path is
resolved against it and antc runs with `cwd=ROOT`.

CLI: `compare_frontend.py lex|parse (--sample | --all | PATHS...)`,
optional `-j N` (default `os.cpu_count()`), `-v` (print `ok <path>` lines).

Selection, sorted, as relative paths:

- `--sample`: `tests/ok/*.ant`, `tests/err/*.ant`, and
  `sorted(all .ant under src/tests)[::14]` (42 files).
- `--all`: every `.ant` under ROOT, skipping `.git` (includes `src/antc/`
  itself and `work/*/probes`; those must agree too, and 1.G relies on it).
- PATHS: files as given; a directory expands to its `.ant` files recursively.

Per file: `reference(mode, src)` returns either `("ok", dump_text)` or
`("error", line, col, message)`: for `lex`, `Lexer(src).tokenize()` rendered
with the contract above (`t.parts` may be `None`; print `esc` as
`s.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")`), or the
`ParseError`'s `line`, `col`, `msg`. `antc(mode, path)` runs
`[sys.executable, "-I", "bootstrap/antpy.py", "run", "src/antc/main.ant",
"lex", path]` with `subprocess.run(capture_output=True, timeout=120)`,
decodes UTF-8 with `errors="replace"`, and returns `("ok", stdout)` on exit
0, `("error", line, col, message)` when exit 1 and the first stderr line
matches `^(.*):(\d+):(\d+): (.*)$`, else `("crash", returncode, stderr)`.

Verdict: both ok and dumps equal -> pass; both error with equal `line:col`
-> pass (print both messages under `-v`); otherwise fail and print the file,
what each side did, and for two dumps the first 30 lines of
`difflib.unified_diff(reference, antc, "reference", "antc")`; for a crash
the first 10 stderr lines. Run files through
`concurrent.futures.ThreadPoolExecutor(max_workers=jobs)` (the work is in
subprocesses); print results in selection order. Last line, always:
`<N> files, <M> failures`; exit 0 iff `M == 0`. `parse` mode: `sys.exit(2)`
with `parse mode arrives in step 1.2` (leave `reference`/`antc` keyed by
mode so 1.2 adds `parse(src)` + an S-expression renderer and the `parse
--dump` argument without restructuring).

## Order of work

1. `tools/compare_frontend.py` first, with the reference side complete; run
   it with `--sample` before `antc` exists: every file must fail as a crash
   with a readable message. This fixes the dump contract before the Ant
   side is written.
2. `src/antc/main.ant` with `dump`, `report`, and a `tokenize` that handles
   only newlines, whitespace, comments, identifiers, keywords, and the
   operator table. Run the compare tool on `tests/ok/newlines.ant` and
   `tests/ok/braces.ant`; the diffs show what is missing next.
3. Numbers (`tests/ok/literals.ant`, `tests/err/octal.ant`,
   `float_leading_dot.ant`, `float_trailing_dot.ant`).
4. Strings and escapes (`literals.ant`, `tests/err/bad_escape.ant`,
   `string_newline.ant`, `src/tests/run/lexical/lex-string-escapes.ant`).
5. Templates and `embedded` (`literals.ant`, `src/tests/run/lexical/lex-template.ant`;
   the two nested-backtick templates are in `literals.ant` and `lex-template.ant`).
6. The seven error files: positions must match exactly (list above).
7. `--sample`, then run `--all` once as well (the 1.G gate needs it; fixing a
   divergence now is cheaper than in the gate), note the result in the report.

## Rules that are easy to get wrong

- A NEWLINE is decided by the previous *token* and the next *line*, and
  blank lines and comment-only lines between them are skipped; the NEWLINE
  token sits at the position of the `\n` character itself, and leading
  newlines (no token yet) and runs of newlines emit nothing extra.
- The final NEWLINE before EOF appears only when the last token is not
  already a NEWLINE and there is at least one token; an empty or
  comment-only file yields just EOF. Its position is the end of file
  (after the last character), as is EOF's.
- `...` ends an expression (newline after it is emitted) and a line starting
  with `...` does not continue the previous one; `.` and `?.` and `|` do.
  `else` continues only when followed by a non-identifier character or end
  of file (`elsewhere` does not).
- `0X` and `0B` are not prefixes: `0X1` lexes `0` then fails as malformed.
  `1.` followed by a non-digit is INT `1` then OP `.`; `.5` is OP `.` then
  INT `5` (the parser rejects both). `1e` without digits is an error at the
  position after `e` (or after the sign).
- Error positions differ by kind: reserved word and `_` are reported *after*
  the word; unterminated string/template at the opening quote; unterminated
  `${` just after the `${`; block comment at the `/*`; bad escape at the
  character after the backslash; malformed number at the current position.
- `\` escapes are the same set inside strings and templates (`\"` is legal
  in a template, `` \` `` and `\$` in a string). Hex digits after `\u{` are
  unbounded in count; the lexer never checks the value.
- Inside `${ ... }` only braces, strings and backtick templates are
  structural; everything else, including comment markers, is skipped
  character by character, and a string there that hits a newline is an
  "unterminated string" error even though the template allows newlines.
- `String.contains(s, "")` is true: every "is this char in the set" check
  must first exclude `""` (end of input), as the reference's `self.peekc()
  and ...` does.
- Column counting is per code point and `\r` is skipped as whitespace but
  still advances the column; a `\r` inside a string is kept.
- The KW/IDENT split uses the exact keyword list; `int`, `string`, `Array`,
  `never`, `print` are IDENT. `in` and `is` are keywords (`for x in xs`).

## Risks

- antpy feature gaps on an Ant program this shape (a 500-line module with
  records mutated through helpers, `try` in loops, mutual recursion): the
  probes in this plan passed, but an unmet corner shows up as an exit-2
  trace in the compare report; fix the Ant code around it and note it, do
  not edit `bootstrap/` (outside this step's directories).
- `/dev/stderr` through `File.create` reopens fd 2 with `O_TRUNC`; harmless
  under pipes and terminals, which is how every runner uses antc. Noted so
  nobody redirects `antc` with `2>file` and wonders.
- Python's Unicode-aware `isalpha`/`isdigit` versus the ASCII rule: invisible
  on the repo's files; a future test with a non-ASCII identifier would
  disagree, and the spec sides with ASCII.
- The compare tool's error check compares positions only; a lexer that
  rejects for the wrong reason at the right place passes. Mirror the
  messages anyway; they are what users read.
- `--all` includes `src/antc/*.ant` itself: a construct the lexer uses but
  mishandles (for instance a template containing a backtick escape) shows
  up only there; step 7 above runs it early.
