# Plan for step 1.2 "antc-parser"

Acceptance: `python3 -I tools/compare_frontend.py parse --sample` exits 0 and
its last line reports `0 failures`.

Deliverable: `src/antc/ast.ant` (the AST as tagged unions of records),
`src/antc/parser.ant` (the parser in Ant, run under antpy), `src/antc/dump.ant`
(the S-expression renderer of the AST), `antc parse [--dump] FILE` in
`src/antc/main.ant`, and the `parse` mode of `tools/compare_frontend.py`.
The oracle is the `Parser` class in `tools/antparse.py` (lines 282-972). The
Ant parser mirrors it method by method, including every error message and
the position each is reported at. Everything the reference keeps as raw
source text (number, string and template literal text, the import path with
its quotes) stays raw text; nothing is decoded.

## Facts gathered while planning (the implementer need not re-measure)

- `--sample` is 85 files: 8 `tests/ok`, 35 `tests/err`, 42 suite files.
  The reference parser rejects exactly the 35 `tests/err` files; their
  positions and messages are listed under "The 35 error files" below. It
  accepts every other `.ant` file in the repository (722 files with the 1.1
  probes), except 36 probes under `work/1.1/probes/` that the 1.G gate's
  `--all` will also select: 30 fail in the lexer with the positions 1.1
  already reproduces, five fail at the top level with `expected declaration,
  got ...` (`block_comment_col` 2:7 `ident 'x'`, `close_then_open` 1:1
  `ident 'f'`, `else_eof` and `else_only_ws` 1:1 `'if'`, `keywords` 1:1
  `'in'`), and `work/1.1/probes/tpl_nested.ant: 1:47: expected newline or
  ';', got ':'` exercises the template-part offset rule (see "Template
  parts").
- Node kinds in use over the whole repo, most frequent first: Ident, Pos,
  Call, Field, String, Int, TPath, Block, Name, Var, Func, Elem, Bin, Entry,
  File, Float, ArrayLit, RecordLit, If, Lit, Type, Import, Namespace, Assign,
  Unary, TRecord, Is, Index, Lambda, Union, Template, For, Nullish, Return,
  MapLit, Paren, TFunc, Pipe, Export, Defer, AnonFunc, And, Or, While, Go,
  Named, Names, Spread, Destructure, Break, OptField, Continue, OptCall,
  OptIndex. `Placeholder` never occurs: it needs `--spec`, which the compare
  tool does not use. The deepest nesting of any file is 24 levels.
- The reference AST holds only tuples, lists, strings and `None`: no Python
  ints or booleans (literals keep their text, `export` becomes an `Export`
  wrapper). The dump form therefore needs no number or boolean cases.
- `antc lex src/antc/lexer.ant` under antpy: 0.25 s wall (12 KB). The parser
  roughly doubles that; `--all` runs files in parallel (`-j`). Performance is
  not a concern; keep the code plain and shaped like the reference.
- Verified under antpy (probe in this planning session): `try` inside a
  record literal field (`Call{ callee: try ident(p), ... }`), inside an
  array literal, inside an `if` branch used as a value, and in a `var`
  initializer; `Result.mapErr(r, f)` with a named function; record literals
  and `is` tests through a namespace import (`Ast.Ident{ ... }`,
  `e is Ast.Ident`); a record field named `default`; default parameters;
  string comparison by code point (`"\u{1}" < " "`); `String.toLower`;
  `String.toBytes(c)[0]`; `for c in s`. A Python-repr emulation (`pyRepr`
  below) printed `'abc'`, `"it's"`, `'"it\'s"'`, `'a\\b\n\t\x01'`,
  `'héllo'`, each identical to Python's `repr`.
- Template parts (verified against the reference): a *lexer* error inside a
  `${...}` body is reported at part-local coordinates (`` var t = `a ${ 1 @ 2 } b` ``
  gives `1:4: unexpected character '@'`), while a *parser* error on the
  part's tokens is reported at file coordinates (a `)` on the part's second
  line gives `2:4: unexpected ')' in template expression`). The cause is
  `Parser.__init__` (antparse.py 283-289): the offset is applied to the
  tokens after `Lexer(src).tokenize()` succeeded.
- Brace literals after a path (verified): `B{ x: 1 }.x` parses (the literal
  is built in `primary`, and `postfix` continues), but `A.B{ x: 1 }.x` fails
  with `expected newline or ';', got '.'` (the literal is built inside
  `postfix`'s `.` branch, which returns at once). Mirror both.
- `return` followed by a raw NEWLINE, `;`, `}`, `)`, `]`, `,` or EOF is a
  bare `Return`; `[return, 1]` and `{ return }` parse.

## Files

| File | Action |
|---|---|
| `src/antc/ast.ant` | create: every node type below, all exported |
| `src/antc/dump.ant` | create: `module(m: Ast.Module): string`, the S-expression renderer |
| `src/antc/parser.ant` | create: `parse(src: string): Result[Ast.Module, ParseError]`, the port of `Parser` |
| `src/antc/main.ant` | change: the `parse [--dump] FILE` command, usage line, header comment |
| `tools/compare_frontend.py` | change: `parse` mode (reference dump, `antc parse --dump`, the `tests/err` rule), docstring |
| `work/1.2/probes/*.ant`, `work/1.2/probes.log` | hand-written edge cases, all agreeing (listed under "Probes") |
| `work/1.2/report.md`, `work/1.2/test.log` | write at the end |

`src/antc/lexer.ant` needs no change (`tokenize(src, startLine, startCol)`,
`Token`, `Part`, `LexError` are what the parser consumes). Do not touch
`tools/antparse.py`, `bootstrap/`, or anything under `src/tests` or `tests/`.

## The shared dump contract

`antc parse --dump FILE` prints the AST in this canonical text form, which
`compare_frontend.py` also renders from the reference tuples:

- `None` is `nil`.
- A string is quoted: `"` + text with `\` as `\\`, `"` as `\"`, LF as `\n`,
  CR as `\r` (in that order, nothing else) + `"`. Tabs and every other
  character stay raw.
- A list is `[` + elements separated by one space + `]`; empty is `[]`.
- A tuple is `(` + elements separated by one space + `)`. Its first element
  is printed **bare** when it is a string: that covers the node kind of a
  tagged tuple (`(Ident "x")`) and the name of the reference's untagged
  tuples, which is always an identifier: `(name, alias)` of import items
  and destructure fields `(x nil)` / `(x "px")`, `(name, bound)` of type
  parameters `(T nil)`, `(name, typ, default)` of parameters
  `(n (TPath ["int"] []) nil)`, `(name, typ)` of record-type fields. A
  first element that is not a string is rendered normally: the signature
  tuple `(params, ret)` prints as `([...] nil)` and a function-type
  parameter `(None, typ)` as `(nil (TPath ["int"] []))`. Every other string
  is quoted. `("Break",)` prints `(Break)`.
- The top-level `File` is the only multi-line node, so a diff points at one
  declaration:

```
(File
  [
    (Import "\"./lib.ant\"" (Namespace "L"))
  ]
  [
    (Var (Name "x") nil (Int "1"))
    (Func "main" [] ([] nil) (Block [(Call (Ident "print") [(Pos (Ident "x"))])]))
  ]
)
```

  Lines: `(File`, `  [`, one `    <import>` line each, `  ]`, `  [`, one
  `    <decl>` line each, `  ]`, `)`. Both sides end the text with exactly one
  newline (`main.ant` builds the lines and prints them; the tool joins with
  `"\n"` and appends one).

Python side (`compare_frontend.py`), to copy literally:

```python
def quote(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r") + '"'

def sexpr(node):
    if node is None:
        return "nil"
    if isinstance(node, str):
        return quote(node)
    if isinstance(node, list):
        return "[" + " ".join(sexpr(x) for x in node) + "]"
    head = node[0] if isinstance(node[0], str) else sexpr(node[0])
    return "(" + " ".join([head] + [sexpr(x) for x in node[1:]]) + ")"

def ast_dump(tree):
    _, imports, decls = tree
    lines = ["(File", "  ["] + ["    " + sexpr(i) for i in imports] + ["  ]", "  ["] \
        + ["    " + sexpr(d) for d in decls] + ["  ]", ")"]
    return "".join(line + "\n" for line in lines)
```

On a parse error `antc parse` prints nothing on stdout, writes
`<path>:<line>:<col>: <message>` to stderr and exits 1, exactly as `antc lex`
does. Without `--dump` it prints nothing and exits 0 on success.

## `src/antc/ast.ant`

Types only, every one exported; no functions. Every record carries `line`
and `col` (the dump ignores them; M3's `antc check` needs them, and adding
them later means touching every constructor). The position is that of the
node's first token, except for the operator-built nodes `Assign`, `Pipe`,
`Nullish`, `And`, `Or`, `Bin`, `Is`, `Field`, `OptField`, `Index`,
`OptIndex`, `Call`, `OptCall`, which take the operator token (`=`, `|`,
`??`, `&&`, `||`, the binary operator, `is`, `.`, `?.`, `[`, `(`), and
`RecordLit` built from a path, which takes the first identifier of the path.
Say this in a comment at the top of the file. Names avoid every prelude name
(`String`, `File`, `Entry`, `Error`, `Result`, `Record`, `Array`, `Map`,
`Ok`, `Err`, `Indexed`, `Seq`), since 3.4 runs `antc check` on these sources
and a top-level name shadowing the prelude is `redeclared`. The field `typ`
is used where the reference says `type` (a keyword).

```ant
export type Module = { imports: Array[Import], decls: Array[Decl] }
export type Import = { line: int, col: int, path: string, pattern: ImportPattern }   // path: the raw STRING text, quotes included
export type ImportPattern = Namespace | Names
export type Namespace = { line: int, col: int, name: string }
export type Names = { line: int, col: int, items: Array[Alias] }
export type Alias = { line: int, col: int, name: string, alias: string | null }       // import item, destructure field

export type Decl = VarDecl | FuncDecl | TypeDecl
export type VarDecl = { line: int, col: int, exported: bool, target: Target, typ: TypeExpr | null, value: Expr }
export type FuncDecl = { line: int, col: int, exported: bool, name: string, tparams: Array[TypeParam], params: Array[Param], ret: TypeExpr | null, body: Block }
export type TypeDecl = { line: int, col: int, exported: bool, name: string, tparams: Array[TypeParam], body: TypeExpr }
export type TypeParam = { line: int, col: int, name: string, bound: TypeExpr | null }
export type Param = { line: int, col: int, name: string, typ: TypeExpr, default: Expr | null }
export type Target = Name | Destructure
export type Name = { line: int, col: int, name: string }
export type Destructure = { line: int, col: int, fields: Array[Alias] }

export type TypeExpr = TPath | Union | TRecord | TFunc
export type TPath = { line: int, col: int, path: Array[string], args: Array[TypeExpr] }   // also null and void: path ["null"]
export type Union = { line: int, col: int, terms: Array[TypeExpr] }
export type TRecord = { line: int, col: int, fields: Array[FieldType] }
export type FieldType = { line: int, col: int, name: string, typ: TypeExpr }
export type TFunc = { line: int, col: int, params: Array[FtParam], ret: TypeExpr }
export type FtParam = { line: int, col: int, name: string | null, typ: TypeExpr }

export type Block = { line: int, col: int, stmts: Array[Stmt] }
export type Stmt = VarDecl | Expr
export type Expr = Assign | Pipe | Nullish | And | Or | Bin | Is | Unary
  | Field | OptField | Index | OptIndex | Call | OptCall
  | Int | Float | StringLit | Template | Ident | Lit
  | AnonFunc | If | While | For | Go | Defer | Return | Break | Continue
  | Lambda | ArrayLit | Paren | MapLit | RecordLit

export type Assign = { line: int, col: int, op: string, target: Expr, value: Expr }   // op: = += -= *= /= %=
export type Pipe = { line: int, col: int, left: Expr, right: Expr }                   // right is always a Call
export type Nullish = { line: int, col: int, left: Expr, right: Expr }
export type And = { line: int, col: int, left: Expr, right: Expr }
export type Or = { line: int, col: int, left: Expr, right: Expr }
export type Bin = { line: int, col: int, op: string, left: Expr, right: Expr }        // == != < <= > >= + - * / %
export type Is = { line: int, col: int, value: Expr, target: Array[string] }          // ["null"], ["func"], or an ident path
export type Unary = { line: int, col: int, op: string, operand: Expr }                // - ! try
export type Field = { line: int, col: int, object: Expr, name: string }
export type OptField = { line: int, col: int, object: Expr, name: string }
export type Index = { line: int, col: int, object: Expr, index: Expr }
export type OptIndex = { line: int, col: int, object: Expr, index: Expr }
export type Call = { line: int, col: int, callee: Expr, args: Array[Arg] }
export type OptCall = { line: int, col: int, callee: Expr, args: Array[Arg] }
export type Arg = PosArg | NamedArg
export type PosArg = { line: int, col: int, value: Expr }
export type NamedArg = { line: int, col: int, name: string, value: Expr }
export type Int = { line: int, col: int, text: string }
export type Float = { line: int, col: int, text: string }
export type StringLit = { line: int, col: int, text: string }                         // raw, quotes included
export type Template = { line: int, col: int, text: string, parts: Array[Expr] }      // raw text, backticks included
export type Ident = { line: int, col: int, name: string }
export type Lit = { line: int, col: int, text: string }                               // true false null void
export type AnonFunc = { line: int, col: int, params: Array[Param], ret: TypeExpr | null, body: Block }
export type If = { line: int, col: int, cond: Expr, then: Block, otherwise: Block | If | null }
export type While = { line: int, col: int, cond: Expr, body: Block }
export type For = { line: int, col: int, target: Target, iter: Expr, body: Block }
export type Go = { line: int, col: int, body: Block }
export type Defer = { line: int, col: int, body: Block }
export type Return = { line: int, col: int, value: Expr | null }
export type Break = { line: int, col: int }
export type Continue = { line: int, col: int }
export type Lambda = { line: int, col: int, stmts: Array[Stmt] }
export type ArrayLit = { line: int, col: int, elems: Array[Elem | Spread] }
export type Elem = { line: int, col: int, value: Expr }
export type Spread = { line: int, col: int, value: Expr }                             // in arrays and record literals
export type Paren = { line: int, col: int, inner: Expr }
export type MapLit = { line: int, col: int, entries: Array[LitEntry | Spread] }
export type RecordLit = { line: int, col: int, path: Array[string], entries: Array[LitEntry | Spread] }
export type LitEntry = { line: int, col: int, key: Expr, value: Expr }
```

`exported` replaces the reference's `("Export", d)` wrapper; the dump
re-wraps. `Block` is not an `Expr` (a bare `{...}` expression is a
`Lambda`); it appears only as a body or branch.

## `src/antc/dump.ant`

```ant
import "./ast.ant" Ast
export func module(m: Ast.Module): string
```

returns the multi-line text of the contract **without** the trailing
newline (`String.join(lines, "\n")`; `main.ant` prints it, which adds the
newline). Private renderers, one per union, each an `is` chain over the
members with a final `else { "?" }` branch that can never run (the checker
later sees the chain complete): `decl`, `target`, `typ`, `stmt`, `expr`,
`arg`, `elem`, `entry`, `block`, plus `alias`, `tparam`, `param`, `sig`,
`fieldType`, `ftParam`, `quote(s)` (the escaping above, `\` first), `strs`
(a list of quoted strings), and `list(xs: Array[string]): string` =
`"[" + String.join(xs, " ") + "]"`. Build every node with template strings.
Exact output per record (`<x>` is the recursive render, `<x|nil>` is `nil`
for `null`):

| Record | Text |
|---|---|
| Import | `(Import <quote path> <pattern>)` |
| Namespace | `(Namespace "<name>")` |
| Names | `(Names [<alias>...])` |
| Alias | `(<name> nil)` or `(<name> "<alias>")`, name bare |
| VarDecl | `(Var <target> <typ|nil> <value>)`, then `(Export <that>)` when `exported` |
| FuncDecl | `(Func "<name>" [<tparam>...] ([<param>...] <ret|nil>) <block>)`, `Export`-wrapped when exported |
| TypeDecl | `(Type "<name>" [<tparam>...] <body>)`, `Export`-wrapped when exported |
| TypeParam | `(<name> <bound|nil>)`, name bare |
| Param | `(<name> <typ> <default|nil>)`, name bare |
| Name | `(Name "<name>")` |
| Destructure | `(Destructure [<alias>...])` |
| TPath | `(TPath ["a" "b"] [<typ>...])` |
| Union | `(Union [<typ>...])` |
| TRecord | `(TRecord [<fieldType>...])`; FieldType `(<name> <typ>)`, name bare |
| TFunc | `(TFunc [<ftParam>...] <ret>)`; FtParam `(nil <typ>)` or `(<name> <typ>)`, name bare |
| Block | `(Block [<stmt>...])` |
| Lambda | `(Lambda [<stmt>...])` |
| Assign | `(Assign "<op>" <target> <value>)` |
| Pipe / Nullish / And / Or | `(Pipe <left> <right>)` and likewise |
| Bin | `(Bin "<op>" <left> <right>)` |
| Is | `(Is <value> ["a" "b"])` |
| Unary | `(Unary "<op>" <operand>)` |
| Field / OptField | `(Field <object> "<name>")`, `(OptField ...)` |
| Index / OptIndex | `(Index <object> <index>)`, `(OptIndex ...)` |
| Call / OptCall | `(Call <callee> [<arg>...])`, `(OptCall ...)` |
| PosArg / NamedArg | `(Pos <value>)`, `(Named "<name>" <value>)` |
| Int / Float / StringLit / Lit | `(Int "<text>")`, `(Float "<text>")`, `(String <quote text>)`, `(Lit "<text>")` |
| Template | `(Template <quote text> [<expr>...])` |
| Ident | `(Ident "<name>")` |
| AnonFunc | `(AnonFunc ([<param>...] <ret|nil>) <block>)` |
| If | `(If <cond> <then> <otherwise|nil>)`, otherwise rendered as a Block or an If |
| While / For | `(While <cond> <body>)`, `(For <target> <iter> <body>)` |
| Go / Defer | `(Go <body>)`, `(Defer <body>)` |
| Return | `(Return <value|nil>)` |
| Break / Continue | `(Break)`, `(Continue)` |
| ArrayLit | `(ArrayLit [<elem>...])`; Elem `(Elem <value>)`; Spread `(Spread <value>)` |
| Paren | `(Paren <inner>)` |
| MapLit | `(MapLit [<entry>...])`; LitEntry `(Entry <key> <value>)` |
| RecordLit | `(RecordLit ["A" "B"] [<entry>...])` |

Names, operators and kinds contain no characters that need escaping, so
they are interpolated directly; `quote` is for literal texts, the import
path, and the strings inside `strs` lists (TPath paths, `Is` targets,
RecordLit paths are identifiers, but render them through `strs` anyway so
one function does all string lists).

## `src/antc/parser.ant`

```ant
import "./lexer.ant" Lex
import "./ast.ant" Ast

export type ParseError = { line: int, col: int, message: string }
export func parse(src: string): Result[Ast.Module, ParseError]
```

State, mutated through helpers exactly as the lexer's `Lexer` is:

```ant
type Parser = { toks: Array[Lex.Token], i: int, nl: int, header: bool }
type Saved = { nl: int, header: bool }
```

`parse`: `var toks = try Result.mapErr(Lex.tokenize(src, 1, 1), fromLex)`
(`fromLex(e: Lex.LexError): ParseError` copies the three fields), then
`parseFile(Parser{ toks, i: 0, nl: 0, header: false })`. A lexer error thus
carries the lexer's position, as the reference's `Parser.__init__` does.

Every parsing function takes `p: Parser` first and returns
`Result[X, ParseError]`; callers unwrap with `try`. Annotate every return
type (the functions are mutually recursive). Errors are built by
`fail(p, message): ParseError` = `failAt(peek(p), message)` and
`failAt(t: Lex.Token, message): ParseError`; return them as
`Result.err(fail(p, ...))`. Note that `fail` calls `peek`, which can skip
newlines when `nl > 0` (as the reference's `error()` does).

Port each method of `Parser` in file order; keep the reference open and
match it line by line. Ant names and the points that need care:

Token helpers (antparse.py 295-344):

- `raw(p)`: `p.toks[p.i]`. `peek(p)`: `while p.nl > 0 && p.toks[p.i].kind == "NEWLINE" { p.i += 1 }` then `p.toks[p.i]`. **`peek` mutates `p.i`**; this side effect is part of the grammar (a newline inside a list is skipped for good) and `raw` sees the position after it.
- `next(p)`: `var t = peek(p); p.i += 1; t`.
- `at(p, value)`: `t.value == value && (t.kind == "OP" || t.kind == "KW")` on `peek`. `atKw(p, value)`: `t.kind == "KW" && t.value == value` (the reference's `at(v, "KW")`; used for `import`, `export`, `else`, `if`). `atKind(p, kind)`.
- `expect(p, value): Result[Lex.Token, ParseError]`: on mismatch `expected '<value>', got <describe(t)>`; the token is `peek`.
- `expectIdent(p, what: string = "identifier"): Result[string, ParseError]`: `expected <what>, got <describe(t)>`. The `what` strings used: `export name`, `local name`, `function name`, `type name`, `type parameter`, `parameter name`, `field name`, `loop variable`, and the default `identifier` (in `type_term`'s path, `postfix`'s `.`/`?.`, `is_target`'s path).
- `skipTerms(p): int`: count and skip while `raw` is NEWLINE or OP `;`.
- `expectTerm(p): Result[void, ParseError]`: `t = raw(p)`; return ok when `t` is EOF or OP `}`; else if `skipTerms(p) == 0` → `failAt(t, "expected newline or ';', got <describe(t)>")`. Position is the **raw** token.
- `enterList(p): Saved` (save, `nl += 1`, `header = false`), `enterBlock(p): Saved` (save, `nl = 0`, `header = false`), `leave(p, saved)` restores both.

`describe(t: Lex.Token): string`, the reference's `Token.__repr__`:
OP/KW → `'<value>'`; NEWLINE → `newline`; EOF → `end of file`; otherwise
`<kind lowercased> <pyRepr(value)>` (`ident 'y'`, `int '2'`,
`string '"a"'`, `template '`x`'`, `float '1.5'`). `pyRepr(s)` is Python's
`repr` of a `str`: quote with `"` when `s` contains `'` and no `"`, else
with `'`; inside, `\` → `\\`, the chosen quote → `\'` or `\"`, LF → `\n`,
CR → `\r`, tab → `\t`, any other code point below `" "` or equal to
`"\u{7f}"` → `\x` + two lowercase hex digits (`int(String.toBytes(c)[0])`
indexed into `"0123456789abcdef"`); everything else, including non-ASCII,
as is. (Python escapes non-printable non-ASCII code points too; none can
occur in the repo's token values; note the divergence in the report.)

File and declarations (363-494): `parseFile`, `importDecl`, `declaration`,
`varDecl`, `funcDecl`, `typeDecl`, `typeParams`, `signature`.

- `parseFile`: `skipTerms`; while `atKw(p, "import")`: `importDecl` (which calls `expectTerm` itself), `skipTerms`. Then while not EOF: `atKw(p, "import")` → `import must come before declarations`; a KW token whose value is `export`, `var`, `func` or `type` → `declaration`; else `expected declaration, got <t>`; then `expectTerm`, `skipTerms`. Returns `Ast.Module{ imports, decls }`.
- `importDecl`: `expect("import")`; `peek` must be kind STRING else `expected module path string, got <t>`; `p.i += 1`; an IDENT next → `Namespace{ name }` via `next`; else `expect("{")`, `enterList`, items `name [: local]` separated by `,` (the reference's `if not at("}"): expect(",")` shape, which allows a trailing comma), `expect("}")`, `leave` → `Names`; `expectTerm`. The `Import` keeps `path.value` raw (quotes included) and the position of the `import` token.
- `declaration`: optional `export` (sets `exported`); then by `peek().value` (value only, as the reference): `var`/`func`/`type`; else `expected var, func, or type after export, got <t>` at `peek`. Set `exported` on the node (`VarDecl`/`FuncDecl`/`TypeDecl` get an `exported: bool` field; `varDecl` is also called from `statement`, where it passes `false`).
- `varDecl(p, exported)`: `expect("var")`; `at("{")` → `destructure`, no type; IDENT → `Name`, then optional `: type`; else `expected identifier or '{' after var, got <peek>`; `expect("=")`; `expression`.
- `funcDecl`: `func`, `expectIdent("function name")`, `typeParams` if `at("[")`, `signature`, `block`.
- `typeDecl`: `type`, `expectIdent("type name")`, optional `typeParams`, `expect("=")`, `typeExpr`.
- `typeParams`: `[`, `enterList`, `name [: type]` list, `]`, `leave`.
- `signature(p): Result[Sig, ParseError]` with a private `type Sig = { params: Array[Ast.Param], ret: Ast.TypeExpr | null }`: `(`, `enterList`, params `name : type [= expression]` (the default is parsed with `nl > 0`), `)`, `leave`, then optional `: type` (parsed **after** `leave`, so at the outer `nl`).

Types (497-573): `typeExpr` (the reference's `type`), `typeTerm`,
`recordType`, `funcType`.

- `typeExpr`: terms separated by `|`; one term is returned as is, two or more as `Union`. In a block `nl == 0`, so the continuation lines of `type Shape = Circle\n  | Rect` rely on the lexer's newline rule, not on the parser.
- `typeTerm`: KW `null`/`void` → `TPath{ path: [value], args: [] }`; OP `{` → `recordType`; KW `func` → `funcType`; OP `(` → `enterList`, inner `typeExpr`, `)`, `leave`, return the inner type unchanged (no node); IDENT → path with `.` + `expectIdent()`, then optional `[` args `]` under `enterList`; anything else `expected type, got <t>`.
- `funcType`: `func`, `(`, `enterList`; each param: `typ = typeExpr`; if `at(":")`: when `typ is Ast.TPath && Array.length(typ.path) == 1 && Array.isEmpty(typ.args)` the name is `typ.path[0]`, consume `:`, `typ = typeExpr`; else `expected ',' or ')' in function type` at `peek` (the `:`); `)`, `leave`, `expect(":")`, `ret = typeExpr`.

Blocks and statements (576-606): `block`, `statements`, `statement`,
`lambda`.

- `block`: `expect("{")` (its position is the Block's), `enterBlock`, `statements`, `expect("}")`, `leave`.
- `statements`: `skipTerms`; while `!at(p, "}")`: `atKind("EOF")` → `expected '}', got end of file`; push `statement`; `if at("}") { break }`; `expectTerm`; `skipTerms`.
- `statement`: KW `var` → `varDecl(p, false)`; KW `type` → `type declarations are only allowed at the top level`; KW `export`/`import` → `'<value>' is only allowed at the top level`; else `expression`.
- `lambda`: like `block` but returns `Lambda{ stmts }`.

Expressions (609-706): `expression`, `pipe`, `pipeOperand`, `equality`,
`comparison`, `isTarget`, `additive`, `multiplicative`, `unary`.

- `expression`: `left = pipe`; if `peek` is an OP in `= += -= *= /= %=`: `next`, `right = pipe`, then if `peek` is again an assignment OP → `assignment cannot be chained` (position: that second operator); `Assign{ op, target: left, value: right }` at the operator.
- `pipe`: `left = pipeOperand`; while `at("|")`: `next`, `right = postfix(p)` (postfix, not expression); `if !(right is Ast.Call)` → `pipe right side must be a call` at `peek` (the token **after** the right side; `tests/err/pipe_not_call.ant` expects 2:15); `Pipe` at the `|`.
- `pipeOperand`: `left = equality`; if `at("??")`: loop `Nullish`, then if `at("&&") || at("||")` → `'??' cannot mix with '&&' or '||' without parentheses` at `peek`; return. Else if `at("&&") || at("||")`: while `&&` → `And`; while `||`: `right = equality`, then while `&&` → `right = And{ right, equality }`; `left = Or{ left, right }`; afterwards if `at("??")` → the same mix error. Copy the reference's loop structure exactly; `a && b || c && d` is `(a && b) || (c && d)`.
- `equality`: `==`/`!=` → `Bin`. `comparison`: `<`/`<=`/`>`/`>=` → `Bin`; KW `is` → `Is{ value: left, target: isTarget(p) }` at the `is` token; loop until neither.
- `isTarget`: KW `null`/`func` → `[value]`; IDENT path with `.`; else `expected type name after 'is', got <t>`.
- `additive`, `multiplicative`: `Bin` with the operator text; left associative loops.
- `unary`: OP `-`/`!` or KW `try` → `Unary{ op, operand: unary(p) }`; else `postfix`.

Postfix and calls (708-771): `postfix`, `callArgs`.

- `postfix`: `node = primary`; `isPath = node is Ast.Ident`; `path: Array[string] = if isPath { [node.name] } else { [] }` (keep the first ident's position for a later `RecordLit`). Loop on `peek`: not an OP → return; `.` → `next`, `name = expectIdent()`, `node = Field{...}` at the `.`; if `isPath` { push `name` to `path`; if `at("{") && !p.header` → **return** `braceLit(p, path, line, col)` at once (no further postfix ops) }; `?.` → `next`, `isPath = false`, then `[` → `enterList`, `expression`, `]`, `leave` → `OptIndex`; `(` → `OptCall{ callee: node, args: callArgs }`; else `OptField{ name: expectIdent() }`; `[` → `next`, `isPath = false`, index under `enterList` → `Index`; `(` → `isPath = false`, `Call{ callee: node, args: callArgs }` (`callArgs` does its own `expect("(")`); any other OP → return.
- `callArgs`: `(`, `enterList`, `seenNamed = false`; each: `e = expression`; if `e is Ast.Ident && at(":")` → `next`, `NamedArg{ name: e.name, value: expression }`, `seenNamed = true`; else if `seenNamed` → `positional argument after a named argument` at `peek` (after the expression: `tests/err/positional_after_named.ant` expects 2:18, the `)`); else `PosArg`; `,` between; `)`, `leave`.

Primary (773-836): `primary`, `templateExpression`, `headerExpr`, `ifExpr`,
`forExpr`, `destructure`, `braceLit`, `arrayLit`.

- INT/FLOAT/STRING → `Int`/`Float`/`StringLit` with the raw text. TEMPLATE → `next`, then for each part in order `parsePart(part)` (below), `Template{ text: t.value, parts }`. IDENT → `next`; `if at("{") && !p.header` → `braceLit(p, [name], line, col)`; else `Ident`. KW: `true`/`false`/`null`/`void` → `Lit{ text }`; `func` → `next`; if `atKind("IDENT")` → `a named function is only allowed at the top level; bind it with var` at that IDENT; `sig = signature`, `AnonFunc{ params: sig.params, ret: sig.ret, body: block }`; `if` → `ifExpr`; `while` → `next`, `cond = headerExpr`, `block`; `for` → `forExpr`; `go`/`defer` → `next`, `block`; `return` → `next`, `r = raw(p)`; bare `Return` when `r` is NEWLINE, EOF, or OP in `; } ) ] ,`; else `Return{ value: expression }`; `break`/`continue` → `next`, `Break`/`Continue`; any other KW (`else`, `in`, `is`, `import`, `export`, `var`, `type`) → `unexpected <t>`. OP: `{` → `lambda`; `[` → `arrayLit`; `(` → `next`, `enterList`, `expression`, `)`, `leave`, `Paren`; any other OP, and NEWLINE/EOF → `unexpected <t>`. (`...` is `unexpected '...'` here: `tests/err/placeholder_strict.ant`.)
- `headerExpr`: `savedHeader = p.header`; `p.header = true`; `e = try expression` (restore the flag before returning the error too, or simply restore on both paths; on the error path it does not matter since parsing stops); `p.header = savedHeader`; `if !at("{")` → `expected '{' after header, got <peek>`; return `e`. The flag is only consulted in `primary` (IDENT followed by `{`) and `postfix` (path followed by `{`); `enterList`/`enterBlock` clear it and `leave` restores it, so `if f(Point{ x: 1.0 }) {` and `if u == (User{ name: "a" }) {` parse while `if u == User{ name: "a" } {` fails with `expected newline or ';', got ':'` (`tests/err/header_literal.ant`, 3:21).
- `ifExpr`: `expect("if")`, `cond = headerExpr`, `then = block`; `atKw("else")` → `next`; `atKw("if")` → `otherwise = ifExpr` else `otherwise = block`.
- `forExpr`: `expect("for")`; `at("{")` → `destructure` else `Name{ expectIdent("loop variable") }`; `expect("in")`; `iter = headerExpr`; `block`.
- `destructure`: `{`, `enterList`, `name [: local]` list with `expectIdent("field name")` / `expectIdent("local name")`, `}`, `leave`.
- `braceLit(p, path, line, col)`: `expect("{")`, `enterList`, `isMap = Array.length(path) == 1 && path[0] == "Map"`; each entry: `at("...")` → `next`; if `isMap` → `spread is not allowed in a map literal` at `peek` (after the `...`); else `Spread{ value: expression }`; otherwise `key = expression`; if `at(":")` → `next`, `value = expression`; `!isMap && !(key is Ast.Ident)` → `record field must be an identifier` at `peek` (**after the value**: `tests/err/record_string_key.ant` expects 2:23); `isMap && key is Ast.Ident` → replace the key by `StringLit{ text: "\"" + key.name + "\"" }` at the key's position; push `LitEntry{ key, value }`; without `:`: `!(key is Ast.Ident)` → `shorthand entry must be an identifier` at `peek`; the entry is `LitEntry{ key: (the quoted StringLit for a map, the Ident itself for a record), value: key }`. `,` between; `}`, `leave`; `MapLit{ entries }` when `isMap` else `RecordLit{ path, entries }`.
- `arrayLit`: `[`, `enterList`, `...` → `Spread{ expression }` else `Elem{ expression }`, `,` between, `]`, `leave`.

Template parts (780, 838-843, and the offset in 283-289):

- `parsePart(part: Lex.Part): Result[Ast.Expr, ParseError]`: `var toks = try Result.mapErr(Lex.tokenize(part.text, 1, 1), fromLex)` — lexed at (1, 1), so a lexer error inside the part is reported at **part-local** coordinates, as the reference does; then offset every token (EOF and NEWLINE included) in this order: `if t.line == 1 { t.col = t.col + part.col - 1 }` then `t.line = t.line + part.line - 1`; then a fresh `Parser{ toks, i: 0, nl: 1, header: false }`; `e = try expression`; `if !atKind("EOF")` → `unexpected <peek> in template expression`; return `e`. (A lexer error position inside a part is wrong by design in the reference; mirror it, it is what the oracle checks, and say so in the report.)

## `src/antc/main.ant`

```ant
import "./lexer.ant" Lex
import "./parser.ant" Parser
import "./dump.ant" Dump
```

Dispatch: `lex FILE` as now; `parse FILE` and `parse --dump FILE` →
`parseCommand(path, dump: bool)`; anything else → the usage line
`usage: antc lex FILE | antc parse [--dump] FILE` on stderr, exit 2 (the
`parse: not implemented` stub goes away). `parseCommand`: `File.read` (on
`Err`: `report(path, 1, 1, message)`, exit 1); `Parser.parse(source)` (on
`Err`: `report(path, e.line, e.col, e.message)`, exit 1); on `Ok` and `dump`
print `Dump.module(m)`. Keep `report`, `eprint`, `dump` (the token dump) and
`esc` as they are; update the header comment.

## `tools/compare_frontend.py`

- Docstring: describe the parse mode and the dump form (copy the contract's bullet list).
- `ANTC_ARGS = {"lex": ["lex"], "parse": ["parse", "--dump"]}`; `antc(mode, path)` runs `ANTC + ANTC_ARGS[mode] + [path]`.
- `reference(mode, src)`: `lex` as now; `parse` → `("ok", ast_dump(antparse.parse(src)))`; `ParseError` → `("error", line, col, msg)` for both modes.
- Add `quote`, `sexpr`, `ast_dump` as given above.
- `compare()`: in parse mode a file whose path starts with `tests/err/` must be rejected by both sides; if either side accepts it, the file fails with the detail `tests/err file accepted by <side>` (the roadmap's "files under tests/err must be rejected by both"). Lex mode keeps the 1.1 rule.
- Remove `if args.mode == "parse": sys.exit("parse mode arrives in step 1.2")`.
- `same_message` stays: only the lexer's `unexpected character {c!r}` is normalised; every parser message, including the `got <kind> <repr>` forms, must match verbatim.

## The 35 error files (reference positions and messages; parse mode must reproduce each)

```
tests/err/array_destructure.ant: 2:5: expected identifier or '{' after var, got '['
tests/err/bad_escape.ant: 2:11: bad escape '\q'                                  (lexer)
tests/err/brace_after_literal.ant: 3:5: expected newline or ';', got '{'
tests/err/chained_assign.ant: 3:9: assignment cannot be chained
tests/err/destructure_nested.ant: 2:10: expected local name, got '{'
tests/err/else_alone.ant: 3:3: unexpected 'else'
tests/err/else_without_brace.ant: 3:19: expected '{', got int '2'
tests/err/export_in_block.ant: 3:3: 'export' is only allowed at the top level
tests/err/float_leading_dot.ant: 2:9: unexpected '.'
tests/err/float_trailing_dot.ant: 3:1: expected identifier, got newline
tests/err/func_type_missing_return.ant: 2:19: expected ':', got newline
tests/err/header_literal.ant: 3:21: expected newline or ';', got ':'
tests/err/header_missing_brace.ant: 3:8: expected '{' after header, got ident 'y'
tests/err/import_after_decl.ant: 3:1: import must come before declarations
tests/err/is_record_type.ant: 2:14: expected type name after 'is', got '{'
tests/err/map_shorthand_literal.ant: 2:16: shorthand entry must be an identifier
tests/err/match_reserved.ant: 3:8: reserved word 'match'                          (lexer)
tests/err/mix_nullish.ant: 2:16: '??' cannot mix with '&&' or '||' without parentheses
tests/err/mix_nullish_after.ant: 2:16: '??' cannot mix with '&&' or '||' without parentheses
tests/err/named_func_in_block.ant: 3:8: a named function is only allowed at the top level; bind it with var
tests/err/octal.ant: 2:10: malformed number                                       (lexer)
tests/err/pipe_not_call.ant: 2:15: pipe right side must be a call
tests/err/placeholder_strict.ant: 3:3: unexpected '...'
tests/err/positional_after_named.ant: 2:18: positional argument after a named argument
tests/err/record_shorthand_literal.ant: 2:18: shorthand entry must be an identifier
tests/err/record_string_key.ant: 2:23: record field must be an identifier
tests/err/reserved_word.ant: 2:10: reserved word 'class'                          (lexer)
tests/err/spread_in_map.ant: 2:17: spread is not allowed in a map literal
tests/err/string_newline.ant: 2:9: unterminated string                            (lexer)
tests/err/toplevel_expr.ant: 2:1: expected declaration, got ident 'print'
tests/err/trailing_operator.ant: 3:1: unexpected newline
tests/err/two_statements_one_line.ant: 3:5: expected newline or ';', got ident 'b'
tests/err/type_in_block.ant: 3:3: type declarations are only allowed at the top level
tests/err/underscore.ant: 2:6: '_' is not an identifier                           (lexer)
tests/err/unterminated_block_comment.ant: 2:1: unterminated block comment         (lexer)
```

## Order of work

1. `tools/compare_frontend.py` parse mode first, reference side complete.
   Run `python3 -I tools/compare_frontend.py parse tests/ok` before `antc parse`
   exists: every file must fail as a crash with a readable detail. Check the
   reference dump of `tests/ok/braces.ant` by eye against the contract
   (`-v` does not print dumps; add a temporary print or use `python3 -I -c`).
2. `src/antc/ast.ant`, then `src/antc/dump.ant`, then the `parse` command in
   `main.ant`, then `parser.ant` with the token helpers, `parseFile`,
   declarations, types, blocks, and a `primary` that handles literals and
   identifiers only. Run `parse tests/ok/types.ant` and `tests/ok/modules.ant`.
3. Expressions top-down (`expression` to `unary`), `postfix` and `callArgs`,
   the rest of `primary`, `headerExpr`/`ifExpr`/`forExpr`, `braceLit`,
   `arrayLit`, `lambda`. Run `parse tests/ok/braces.ant`, `headers.ant`,
   `newlines.ant`, `functions.ant`, `literals.ant`, `example.ant`; the diffs
   show what is missing next.
4. `parsePart` with the offset rule; `tests/ok/literals.ant` and
   `src/tests/run/lexical/lex-template.ant` have the nested templates,
   `work/1.1/probes/tpl_nested.ant` the offset error.
5. `parse tests/err`: all 35 must agree on position and message.
6. `--sample`, then `--all` (the 1.G gate for this mode; it includes
   `src/antc/*.ant` itself, so antc parses its own parser, and the 1.1
   probes). Record both results in the report.
7. Probes under `work/1.2/probes/` (below), `probes.log`, then the report.

Run subsets as `python3 -I tools/compare_frontend.py parse tests/ok tests/err work/1.2/probes`.

## Rules that are easy to get wrong

- `peek` skips newlines only when `nl > 0` and advances `p.i` while doing
  so; `raw` never skips. `return`'s bare-form test, `skipTerms` and
  `expectTerm` use `raw`; everything else uses `peek`. Inside `(`, `[` and
  non-block `{` (`enterList`) newlines vanish; inside a block or lambda
  (`enterBlock`) they are terminators again, whatever the enclosing list.
- Error positions are the token `peek` returns **at the moment the error is
  raised**, which is often after a sub-expression was parsed: `pipe right
  side must be a call`, `record field must be an identifier`, `shorthand
  entry must be an identifier`, `positional argument after a named
  argument`, `assignment cannot be chained`, the two `??` mixing errors,
  `expected ',' or ')' in function type`, `spread is not allowed in a map
  literal` (after the `...`). `expected newline or ';'` uses the raw token.
- The header flag: set by `headerExpr` for `if`/`while`/`for`, restored to
  its previous value afterwards (not to `false`), cleared by `enterList`
  and `enterBlock`, restored by `leave`. It only suppresses the brace
  literal after an identifier path; a lambda after an operator in a header
  (`if Array.some(xs, { x > 0 }) {`) is unaffected because `{` after `,`
  reaches `primary`'s OP branch.
- `primary`'s brace literal continues through `postfix` (`B{ x: 1 }.x`),
  `postfix`'s own brace literal (after `a.b`) returns immediately
  (`A.B{ x: 1 }.x` is an error at the `.`). `isPath` is cleared by `?.`,
  `[` and `(`, and the path keeps growing through `.` names even when a
  brace never comes.
- `Map` is a map literal only as the one-element path `["Map"]`; `X.Map{}`
  is a record literal with the path `["X", "Map"]`. A bare identifier key
  in a map literal becomes a `String` node whose text is the name in double
  quotes, both with and without `:`; in a record literal the key stays an
  `Ident`. Spread is rejected in maps after the `...` is consumed.
- `typeTerm`'s parenthesized type returns the inner type with no wrapper
  node; `typeExpr` with one term returns the term, not a one-element
  `Union`. `func(T): A | B` has return type `A | B` because the return type
  is parsed by `typeExpr`.
- `signature`'s return type is parsed after `leave`, so a newline before
  `: int` would be a terminator (nobody writes that; just keep the order).
- Named-argument detection reads a full `expression` first and reinterprets
  a lone `Ident` followed by `:`; `f(a.b: 1)` is therefore a `:` error, not
  a named argument.
- `isTarget` accepts `null` and `func` keywords and identifier paths only;
  `x is void` and `x is {}` are errors at the token after `is`.
- `return` consumes the whole expression to its right (`x ?? return y + 1`
  returns `y + 1`); a `,`, `)`, `]`, `}` or `;` right after it makes it
  bare. `break`/`continue` take nothing.
- The `else` keyword never reaches `primary` legitimately; it is
  `unexpected 'else'`. `else` on its own line works because the lexer
  emitted no NEWLINE before it.
- Messages: copy them character for character, including `expected '{',
  got ...` (Python's `'{{'` is one brace), the apostrophes in `'??' cannot
  mix with '&&' or '||' without parentheses` and `';'`, and the
  `<kind> <repr>` forms from `describe`. An IDENT, INT or FLOAT value never
  needs escaping, so `pyRepr` of those is `'<value>'`; STRING and TEMPLATE
  values can contain quotes, backslashes and newlines, where the quote
  choice and escapes matter.
- Lexer errors in a template part are at part-local coordinates; parser
  errors on the part's tokens are at file coordinates (offset applied to
  every token, column only on the part's first line). Nested templates in
  a part are re-lexed from the part text and their parts found again.
- Dump: `quote` escapes `\` first, then `"`, LF, CR; nothing else. Template
  texts contain real newlines and must come out as `\n` on one line. Both
  renderers must agree on the untagged tuples (name bare, alias quoted or
  `nil`) and on the `Export` wrapper.
- Write checkable Ant (3.4 runs `antc check` on these sources): every
  parameter and return type annotated, empty arrays annotated
  (`var parts: Array[Ast.Expr] = []`), no lambda without an expected type
  (pass named functions to `Array.map`), `is` only on union-typed values
  with the member as target, no top-level name equal to a prelude or
  import name (`Parser` as the private state record is fine inside
  `parser.ant`; `main.ant` imports the module as `Parser`, which is a
  different module's binding).

## Probes (`work/1.2/probes/`, each must agree with the reference)

Write small files for: a lexer error inside a `${}` (part-local position);
a parser error inside a multi-line `${}` (file position on the part's
second line); a part whose expression is followed by junk (`unexpected ...
in template expression`); an empty `${}`; nested templates with their own
parts; `B{}.x` and `A.B{}.x`; `Map{ name, (name): 1, "k": 2, 3: 4 }`,
`X.Map{ a: 1 }`, `Map{ ...m }` (error); `f(a: 1, b)` (error), `f(a.b: 1)`
(error), `f(x: 1,)`; `x ?? return y + 1` and `[return, 1]` and `return)`
inside parens; `a && b || c && d`, `a || b && c`, `a ?? b ?? c`, the two
mix errors; `if a { } else if b { } else { }` chains and `else` on its own
line; headers with a parenthesized literal, a lambda argument, an `is`
chain, an assignment (`if count = 5 {`), and a bare literal (error); a
function type with named and unnamed parameters, `func(T): A | B`, a
parenthesized function type in a union, `func(a.b: int): int` (error);
type arguments across lines; a `type` and an `export` inside a block
(errors); an `import` after a declaration (error); `var` with a type and a
destructure; `for { a: b, c } in m`; `try -x`, `!!a`, `-(-a)`; a `|` chain
whose right side is `{ x }()` (G23) and one whose right side is a bare
identifier (error); `return` at end of file without a newline; deep
nesting (30 levels of parentheses); every statement keyword in `primary`
positions (`var x = if c { 1 } else { 2 }`, `var y = while false {}`,
`var z = for i in xs {}`, `go {}` and `defer {}` as values). Keep each file
under 30 lines; the reference must raise `ParseError` or accept, never
another Python exception (a probe that makes the reference crash is a tool
failure, not a parser failure).

## Risks

- Position fidelity where the reference reports at `peek` after a
  sub-parse, and the `peek` side effect under `nl > 0`: a port that
  computes positions "logically" instead of calling `peek` at the same
  moment will be off by a token on some `tests/err` file. Mirror the call
  order, not the intent.
- The `header` flag and `enterList`/`enterBlock`/`leave` bookkeeping: an
  early `return` on an error path that skips `leave` is harmless (parsing
  stops), but a success path that forgets `leave` turns newlines off for
  the rest of the file and shows up as a baffling dump diff far away.
  Write `leave` right after the matching `expect` of the closer, as the
  reference does.
- `pyRepr` corner cases (a STRING token holding both quote kinds, a
  TEMPLATE with newlines) only matter in `got <kind> <repr>` messages, which
  the suite triggers with identifiers and an int; probe them anyway.
- antpy speed on `--all` in parse mode: `parser.ant` (~900 lines) is parsed
  by itself under the interpreter; expect a few seconds per large file,
  within the 120 s per-file timeout, and `-j` parallelism keeps `--all`
  around a minute or two. If a file approaches the timeout, the culprit is
  almost certainly a quadratic `Array.slice`/`String.concat` in a loop,
  not the grammar.
- Name collisions with the prelude in `ast.ant` (`String`, `File`, `Entry`,
  `Type`, `Index`) are avoided by the names above (`StringLit`, `Module`,
  `LitEntry`, `TypeDecl`, `Index` is fine); an accidental `type Entry` would
  only bite in M3, so keep the list.
- Both renderers drifting apart on an escape or the `Export`/untagged-tuple
  rules; the contract above is the single source, and step 1 of the order
  of work fixes the Python side before the Ant side exists.
- antpy feature gaps on a 900-line module with ~45 record types, 40-way
  `is` chains and `try` everywhere: the planning probes covered the shapes
  used here; an unmet corner shows as an exit-2 trace in the compare
  report. Work around it in the Ant code and note it in the report; do not
  edit `bootstrap/`.
