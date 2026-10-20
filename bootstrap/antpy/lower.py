"""Lowering: one pass over the tuple AST of tools/antparse.py that produces the
evaluator's nodes (dispatched on node[0] in interp.py):

  Const value                      decoded literal
  Template parts                   str chunks and expression nodes, in order
  Ident name
  Field obj name | OptField obj name
  Index obj i | OptIndex obj i
  Call f args | OptCall f args     args: [(name_or_None, node)] in written order
  Chain node                       a postfix chain with a `?.` somewhere on its spine
  Pipe left f args                 left | f(args)
  Arith op l r                     + - * / % < <= > >=
  Eq l r | Ne l r
  Neg e | Not e | Try e
  And l r | Or l r | Nullish l r
  IsKind e types | IsTag e path
  AssignVar op name right          op None for `=`, else the arithmetic operator
  AssignField op obj name right | AssignIndex op obj i right | AssignBad
  If cond then else                else: None, a Block, or an If
  While cond body | For target it body
  Return e_or_None | Break | Continue
  Defer body | Go body
  Func name params body is_lambda  params: [(name, default_node_or_None)]
  RecordLit path entries           entries: ("F", name, node) | ("S", node)
  MapLit entries                   entries: (key_node, value_node)
  ArrayLit elems                   elems: ("E", node) | ("S", node)
  Block stmts has_defer            stmts: [(line, node)]
  Var target e                     target: ("Name", n) | ("Destructure", [(field, local)])

Literals are decoded here, once. Blocks that contain a `defer` directly are
marked so that only they allocate a defer list at run time.
"""
import math

import antparse
from antpy.values import VOID, Int8, Int16, Int32, UInt8, UInt16, UInt32, UInt64, Float32
from antpy.values import Closure, Native, YieldFn, Void
from antpy.equal import AntMap


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


# Kind tests decided by name (section 12, "a is T"; G48 for int and byte).
KIND_TESTS = {
    "null": (type(None),), "void": (Void,), "bool": (bool,), "string": (str,),
    "func": (Closure, Native, YieldFn), "Array": (list,), "Map": (AntMap,),
    "int": (int,), "int64": (int,), "int8": (Int8,), "int16": (Int16,), "int32": (Int32,),
    "byte": (UInt8,), "uint8": (UInt8,), "uint16": (UInt16,), "uint32": (UInt32,), "uint64": (UInt64,),
    "float32": (Float32,), "float64": (float,),
}

SIMPLE_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "`": "`", "$": "$"}
LITERALS = {"true": True, "false": False, "null": None, "void": VOID}
POSTFIX = ("Field", "OptField", "Index", "OptIndex", "Call", "OptCall")
INT_MAX = 2 ** 63 - 1


def lower_file(ast, path):
    return Lowerer(path).file(ast)


class Lowerer:
    def __init__(self, path):
        self.path = path
        self.line = 1
        self.blocks = [[False]]     # has_defer cells of the blocks being lowered

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
                decls.append(("Var", line, self.target(node[1]), self.expr(node[3])))
            elif node[0] == "Func":
                _, name, _tparams, sig, body = node
                decls.append(("Func", line, name, self.params(sig), self.block(body)))
            else:
                decls.append(("Type", line, node[1]))
        return ("File", imports, decls)

    def params(self, sig):
        return [(name, self.expr(default) if default is not None else None) for name, _typ, default in sig[0]]

    def target(self, t):
        if t[0] == "Name":
            return t
        return ("Destructure", [(name, alias or name) for name, alias in t[1]])

    # --- statements and blocks
    def block(self, node):
        self.blocks.append([False])
        stmts = self.stmts(node[1])
        has_defer = self.blocks.pop()[0]
        return ("Block", stmts, has_defer)

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
            return ("Const", self.int_lit(node[1]))
        if k == "Float":
            v = float(node[1].replace("_", ""))
            if math.isinf(v):
                self.error("literal out of range")
            return ("Const", v)
        if k == "String":
            return ("Const", self.string(node[1]))
        if k == "Lit":
            return ("Const", LITERALS[node[1]])
        if k == "Template":
            return self.template(node[1], node[2])
        if k == "Ident":
            return node
        if k == "Paren":
            return self.expr(node[1])
        if k in POSTFIX:
            lowered, has_opt = self.spine(node)
            return ("Chain", lowered) if has_opt else lowered
        if k == "Pipe":
            left = self.expr(node[1])
            call = node[2]                      # the parser guarantees a Call
            callee, has_opt = self.spine(call[1])
            pipe = ("Pipe", left, callee, self.args(call[2]))
            return ("Chain", pipe) if has_opt else pipe
        if k == "Bin":
            op, l, r = node[1], self.expr(node[2]), self.expr(node[3])
            if op == "==":
                return ("Eq", l, r)
            if op == "!=":
                return ("Ne", l, r)
            return ("Arith", op, l, r)
        if k == "Unary":
            e = self.expr(node[2])
            return ({"-": "Neg", "!": "Not", "try": "Try"}[node[1]], e)
        if k in ("And", "Or", "Nullish"):
            return (k, self.expr(node[1]), self.expr(node[2]))
        if k == "Is":
            e, path = self.expr(node[1]), node[2]
            if len(path) == 1 and path[0] in KIND_TESTS:
                return ("IsKind", e, KIND_TESTS[path[0]])
            return ("IsTag", e, path)
        if k == "Assign":
            return self.assign(node)
        if k == "If":
            _, cond, then, otherwise = node
            if otherwise is not None:
                otherwise = self.block(otherwise) if otherwise[0] == "Block" else self.expr(otherwise)
            return ("If", self.expr(cond), self.block(then), otherwise)
        if k == "While":
            return ("While", self.expr(node[1]), self.block(node[2]))
        if k == "For":
            return ("For", self.target(node[1]), self.expr(node[2]), self.block(node[3]))
        if k == "Return":
            return ("Return", self.expr(node[1]) if node[1] is not None else None)
        if k in ("Break", "Continue"):
            return node
        if k == "Defer":
            self.blocks[-1][0] = True
            return ("Defer", self.block(node[1]))
        if k == "Go":
            return ("Go", self.block(node[1]))
        if k == "AnonFunc":
            return ("Func", "func", self.params(node[1]), self.block(node[2]), False)
        if k == "Lambda":
            return ("Func", "lambda", [("x", None)], self.block(("Block", node[1])), True)
        if k == "RecordLit":
            entries = []
            for e in node[2]:
                if e[0] == "Spread":
                    entries.append(("S", self.expr(e[1])))
                else:
                    entries.append(("F", e[1][1], self.expr(e[2])))
            return ("RecordLit", node[1], entries)
        if k == "MapLit":
            return ("MapLit", [(self.expr(e[1]), self.expr(e[2])) for e in node[1]])
        if k == "ArrayLit":
            return ("ArrayLit", [("S" if e[0] == "Spread" else "E", self.expr(e[1])) for e in node[1]])
        if k == "Var":
            return ("Var", self.target(node[1]), self.expr(node[3]))
        if k == "Block":
            return self.block(node)
        self.error(f"unsupported syntax {k}")

    def spine(self, node):
        """Lowers a postfix chain; returns (node, has_opt). The object side
        stays on the spine; arguments and indices start chains of their own."""
        k = node[0]
        if k == "Field":
            obj, has_opt = self.spine(node[1])
            return ("Field", obj, node[2]), has_opt
        if k == "OptField":
            obj, _ = self.spine(node[1])
            return ("OptField", obj, node[2]), True
        if k == "Index":
            obj, has_opt = self.spine(node[1])
            return ("Index", obj, self.expr(node[2])), has_opt
        if k == "OptIndex":
            obj, _ = self.spine(node[1])
            return ("OptIndex", obj, self.expr(node[2])), True
        if k == "Call":
            f, has_opt = self.spine(node[1])
            return ("Call", f, self.args(node[2])), has_opt
        if k == "OptCall":
            f, _ = self.spine(node[1])
            return ("OptCall", f, self.args(node[2])), True
        return self.expr(node), False

    def args(self, args):
        out = []
        for a in args:
            if a[0] == "Pos":
                out.append((None, self.expr(a[1])))
            else:
                out.append((a[1], self.expr(a[2])))
        return out

    def assign(self, node):
        _, op, target, right = node
        aop = None if op == "=" else op[0]
        right = self.expr(right)
        if target[0] == "Ident":
            return ("AssignVar", aop, target[1], right)
        if target[0] == "Field":
            return ("AssignField", aop, self.expr(target[1]), target[2], right)
        if target[0] == "Index":
            return ("AssignIndex", aop, self.expr(target[1]), self.expr(target[2]), right)
        return ("AssignBad",)

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
        return ("Template", parts)
