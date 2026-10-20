"""Lowering: one pass over the tuple AST of tools/antparse.py that produces the
evaluator's nodes. The head of every node is its handler, looked up once
here in the table interp.py passes (`node[0](interp, node, scope)` evaluates
it); the kinds below name the handlers:

  Const value                      decoded literal
  Template parts                   str chunks and expression nodes, in order
  Ident name
  Field obj name
  Index obj i
  Call f pos named                 pos: [node]; named: [(name, node)]; the grammar
                                   puts every positional argument first, so the split keeps the order
  NullGuard e                      the operand of a `?.`: leaves the enclosing Chain when it is null
  Chain node                       a postfix chain with a `?.` somewhere on its spine
  Pipe left f pos named            left | f(args)
  Arith op l r                     op: the function of arith.OPS for + - * / % < <= > >=
  Eq l r | Ne l r
  Neg e | Not e | Try e
  And l r | Or l r | Nullish l r
  IsKind e kind | IsTag e path
  AssignVar op name right          op None for `=`, else the function of arith.OPS
  AssignField op obj name right | AssignIndex op obj i right | AssignBad
  If cond then else                else: None, a Block, or an If
  While cond body | For target it body
  Return e_or_None | Break | Continue
  Defer body | Go body
  Func name params names body is_lambda
                                   params: [(name, default_node_or_None)]; names: the tuple of their names
  RecordLit path entries           entries: ("F", name, node) | ("S", node)
  MapLit entries                   entries: (key_node, value_node)
  ArrayLit elems                   elems: ("E", node) | ("S", node)
  Block stmts has_defer binds      stmts: [(line, node)]; binds: has_defer, or a `var` among stmts
  Var target e                     target: ("Name", n) | ("Destructure", [(field, local)])

A file lowers to ("File", imports, decls): each import is ("Import", line,
path, pattern), each declaration ("Type", line, name), ("Func", line, Func
node), or ("Var", line, Var node).

Literals are decoded here, once. Blocks that contain a `defer` directly are
marked so that only they allocate a defer list at run time, and blocks that
bind nothing so that they run in the enclosing scope.
"""
import math

import antparse
from antpy.values import VOID, CONVERSION_KINDS, INT_RANGE
from antpy.arith import OPS


class LoadError(Exception):
    """An error reported before the program runs: path:line:col: message."""

    def __init__(self, path, line, col, msg):
        super().__init__(f"{path}:{line}:{col}: {msg}")
        self.path, self.line, self.col, self.msg = path, line, col, msg


class LineParser(antparse.Parser):
    """The reference parser, with every statement, declaration, and import
    wrapped as ("At", line, node) so that traces and load errors have lines."""

    def statement(self):
        line = self.peek().line
        return ("At", line, super().statement())

    def declaration(self):
        line = self.peek().line
        return ("At", line, super().declaration())

    def import_decl(self):
        line = self.peek().line
        return ("At", line, super().import_decl())


def parse_module(src):
    return LineParser(src).parse_file()


# The names `a is T` tests by kind (section 12; G48 for int and byte): the
# conversion names and these.
KIND_TESTS = dict(CONVERSION_KINDS, null="null", void="void", bool="bool", string="string",
                  func="func", Array="array", Map="map")

SIMPLE_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "`": "`", "$": "$"}
LITERALS = {"true": True, "false": False, "null": None, "void": VOID}
# The postfix operators and the plain operator each `?.` form guards.
POSTFIX = {"Field": "Field", "Index": "Index", "Call": "Call",
           "OptField": "Field", "OptIndex": "Index", "OptCall": "Call"}
INT_MAX = INT_RANGE["int64"][1]


def lower_file(ast, path, handlers):
    return Lowerer(path, handlers).file(ast)


class Lowerer:
    def __init__(self, path, handlers):
        self.path = path
        self.handlers = handlers    # node kind -> the evaluator's handler
        self.line = 1
        self.blocks = [False]       # has_defer of each block being lowered, innermost last

    def node(self, kind, *rest):
        return (self.handlers[kind],) + rest

    def error(self, msg):
        raise LoadError(self.path, self.line, 1, msg)

    def unwrap(self, node):
        """Splits an ("At", line, node) wrapper; a bare node keeps the current line."""
        if node[0] == "At":
            self.line = node[1]
            return node[2]
        return node

    # --- file
    def file(self, ast):
        imports = []
        for imp in ast[1]:
            node = self.unwrap(imp)
            imports.append(("Import", self.line, self.string(node[1]), node[2]))
        decls = []
        for d in ast[2]:
            node = self.unwrap(d)
            line = self.line
            if node[0] == "Export":
                node = node[1]
            if node[0] == "Var":
                decls.append(("Var", line, self.expr(node)))
            elif node[0] == "Func":
                _, name, _tparams, sig, body = node
                decls.append(("Func", line, self.func(name, self.params(sig), self.block(body), False)))
            else:
                decls.append(("Type", line, node[1]))
        return ("File", imports, decls)

    def params(self, sig):
        return [(name, self.expr(default) if default is not None else None) for name, _typ, default in sig[0]]

    def func(self, name, params, body, is_lambda):
        return self.node("Func", name, params, tuple(n for n, _ in params), body, is_lambda)

    def target(self, t):
        if t[0] == "Name":
            return t
        return ("Destructure", [(name, alias or name) for name, alias in t[1]])

    # --- statements and blocks
    def block(self, node):
        self.blocks.append(False)
        stmts = self.stmts(node[1])
        has_defer = self.blocks.pop()
        var = self.handlers["Var"]
        return self.node("Block", stmts, has_defer, has_defer or any(n[0] is var for _, n in stmts))

    def stmts(self, nodes):
        out = []
        for s in nodes:
            node = self.unwrap(s)
            out.append((self.line, self.expr(node)))
        return out

    # --- expressions
    def expr(self, node):
        k = node[0]
        if k == "Int":
            return self.node("Const", self.int_lit(node[1]))
        if k == "Float":
            v = float(node[1].replace("_", ""))
            if math.isinf(v):
                self.error("literal out of range")
            return self.node("Const", v)
        if k == "String":
            return self.node("Const", self.string(node[1]))
        if k == "Lit":
            return self.node("Const", LITERALS[node[1]])
        if k == "Template":
            return self.template(node[1], node[2])
        if k == "Ident":
            return self.node("Ident", node[1])
        if k == "Paren":
            return self.expr(node[1])
        if k in POSTFIX:
            lowered, has_opt = self.spine(node)
            return self.node("Chain", lowered) if has_opt else lowered
        if k == "Pipe":
            left = self.expr(node[1])
            call = node[2]                      # the parser guarantees a Call
            callee, has_opt = self.spine(call[1])
            pipe = self.node("Pipe", left, callee, *self.args(call[2]))
            return self.node("Chain", pipe) if has_opt else pipe
        if k == "Bin":
            op, l, r = node[1], self.expr(node[2]), self.expr(node[3])
            if op == "==":
                return self.node("Eq", l, r)
            if op == "!=":
                return self.node("Ne", l, r)
            return self.node("Arith", OPS[op], l, r)
        if k == "Unary":
            e = self.expr(node[2])
            return self.node({"-": "Neg", "!": "Not", "try": "Try"}[node[1]], e)
        if k in ("And", "Or", "Nullish"):
            return self.node(k, self.expr(node[1]), self.expr(node[2]))
        if k == "Is":
            e, path = self.expr(node[1]), node[2]
            if len(path) == 1 and path[0] in KIND_TESTS:
                return self.node("IsKind", e, KIND_TESTS[path[0]])
            return self.node("IsTag", e, path)
        if k == "Assign":
            return self.assign(node)
        if k == "If":
            _, cond, then, otherwise = node
            otherwise = None if otherwise is None else self.expr(otherwise)
            return self.node("If", self.expr(cond), self.block(then), otherwise)
        if k == "While":
            return self.node("While", self.expr(node[1]), self.block(node[2]))
        if k == "For":
            return self.node("For", self.target(node[1]), self.expr(node[2]), self.block(node[3]))
        if k == "Return":
            return self.node("Return", self.expr(node[1]) if node[1] is not None else None)
        if k in ("Break", "Continue"):
            return self.node(k)
        if k == "Defer":
            self.blocks[-1] = True
            return self.node("Defer", self.block(node[1]))
        if k == "Go":
            return self.node("Go", self.block(node[1]))
        if k == "AnonFunc":
            return self.func("func", self.params(node[1]), self.block(node[2]), False)
        if k == "Lambda":                       # the one parameter `x`, void when no argument fills it
            return self.func("lambda", [("x", self.node("Const", VOID))], self.block(("Block", node[1])), True)
        if k == "RecordLit":
            entries = []
            for e in node[2]:
                if e[0] == "Spread":
                    entries.append(("S", self.expr(e[1])))
                else:
                    entries.append(("F", e[1][1], self.expr(e[2])))
            return self.node("RecordLit", node[1], entries)
        if k == "MapLit":
            return self.node("MapLit", [(self.expr(e[1]), self.expr(e[2])) for e in node[1]])
        if k == "ArrayLit":
            return self.node("ArrayLit", [("S" if e[0] == "Spread" else "E", self.expr(e[1])) for e in node[1]])
        if k == "Var":
            return self.node("Var", self.target(node[1]), self.expr(node[3]))
        if k == "Block":
            return self.block(node)
        self.error(f"unsupported syntax {k}")

    def spine(self, node):
        """Lowers a postfix chain; returns (node, has_opt). The object side
        stays on the spine; arguments and indices start chains of their own.
        A `?.` form is the plain form over a NullGuard of its object."""
        k = node[0]
        base = POSTFIX.get(k)
        if base is None:
            return self.expr(node), False
        obj, has_opt = self.spine(node[1])
        if base != k:
            obj, has_opt = self.node("NullGuard", obj), True
        if base == "Field":
            return self.node("Field", obj, node[2]), has_opt
        if base == "Index":
            return self.node("Index", obj, self.expr(node[2])), has_opt
        return self.node("Call", obj, *self.args(node[2])), has_opt

    def args(self, args):
        """The arguments as (positional nodes, [(name, node)]); the parser
        refuses a positional argument after a named one, so the two lists
        keep the written order."""
        pos = [self.expr(a[1]) for a in args if a[0] == "Pos"]
        named = [(a[1], self.expr(a[2])) for a in args if a[0] == "Named"]
        return pos, named

    def assign(self, node):
        _, op, target, right = node
        aop = None if op == "=" else OPS[op[0]]
        right = self.expr(right)
        if target[0] == "Ident":
            return self.node("AssignVar", aop, target[1], right)
        if target[0] == "Field":
            return self.node("AssignField", aop, self.expr(target[1]), target[2], right)
        if target[0] == "Index":
            return self.node("AssignIndex", aop, self.expr(target[1]), self.expr(target[2]), right)
        return self.node("AssignBad")

    # --- literals
    def int_lit(self, text):
        text = text.replace("_", "")
        if text.startswith("0x"):
            v = int(text[2:], 16)
        elif text.startswith("0b"):
            v = int(text[2:], 2)
        else:
            v = int(text, 10)
        if v > INT_MAX:
            self.error("literal out of range")
        return v

    def string(self, raw):
        """Decodes a string literal token, quotes included."""
        body = raw[1:-1]
        out = []
        i = 0
        while i < len(body):
            c = body[i]
            if c == "\\":
                ch, i = self.escape(body, i)
                out.append(ch)
            else:
                out.append(c)
                i += 1
        return "".join(out)

    def escape(self, s, i):
        """At a backslash (already validated by the lexer): the character and the next index."""
        c = s[i + 1]
        if c == "u":
            end = s.index("}", i + 3)
            cp = int(s[i + 3:end], 16)
            if cp > 0x10FFFF or 0xD800 <= cp <= 0xDFFF:
                self.error("invalid escape")
            return chr(cp), end + 1
        return SIMPLE_ESCAPES[c], i + 2

    def template(self, raw, exprs):
        """Splits a template token into chunks and the parser's expressions.
        The `${...}` bodies are skipped with the reference lexer's own scanner."""
        body = raw[1:-1]
        lexer = antparse.Lexer(body)
        parts, buf = [], []
        i, next_expr = 0, 0
        while i < len(body):
            c = body[i]
            if c == "\\":
                ch, i = self.escape(body, i)
                buf.append(ch)
            elif c == "$" and body.startswith("{", i + 1):
                lexer.pos = i + 2
                lexer.embedded()
                i = lexer.pos
                if buf:
                    parts.append("".join(buf))
                    buf = []
                parts.append(self.expr(exprs[next_expr]))
                next_expr += 1
            else:
                buf.append(c)
                i += 1
        if buf:
            parts.append("".join(buf))
        return self.node("Template", parts)
