"""The evaluator (ant-lang.md section 12): scopes, frames, control flow,
calls, loops, deferred blocks, panics, modules, and goroutines.

Control flow is Python exceptions. `ReturnSignal` carries the frame of the
function it leaves, so a `return` in a `for` body passes through the sequence
function driving the loop, and a `try` always leaves the function it is
written in. A panic unwinds as `AntPanic`; a block marked by the lowering pass
runs its deferred blocks on every exit.
"""
import os
import sys
import threading
import time
import traceback

import antparse
from antpy.values import (
    VOID, UNINIT, MISSING, MODULE_TAG, OK, ERR, FUNC_TYPES,
    Scope, Frame, Record, Tag, Closure, Native, YieldFn, AntPanic, panic,
)
from antpy.equal import AntMap, eq
from antpy import arith
from antpy.render import render
from antpy.lower import LoadError, parse_module, lower_file
from antpy.library import record_field, entry, drive, err
from antpy import prelude


class ReturnSignal(BaseException):
    def __init__(self, frame, value):
        self.frame = frame
        self.value = value


class BreakSignal(BaseException):
    pass


class ContinueSignal(BaseException):
    pass


class OptNull(BaseException):
    """Raised by `?.` on null; caught by the enclosing Chain node."""


LOOP_SIGNALS = (BreakSignal, ContinueSignal)
UNWINDING = (AntPanic, ReturnSignal, BreakSignal, ContinueSignal)


def trace_of(frame):
    lines = []
    while frame is not None:
        lines.append(f"{frame.name} ({frame.path}:{frame.line})")
        frame = frame.parent
    return lines


def report_internal():
    """Reports a Python exception that escaped the evaluator: a bug in the interpreter."""
    sys.stdout.flush()
    sys.stderr.write("panic: internal interpreter error\n" + traceback.format_exc())
    sys.stderr.flush()


class Interp:
    def __init__(self, args):
        self.args = args
        self.modules = {}            # absolute path -> module record
        self.loading = []            # (importer path, import line) of each import being loaded, outermost first
        self.unwinding = set()       # idents of threads between a panic and its report
        self.prelude = prelude.make_prelude()
        self.table = {
            "Const": self.ev_Const, "Template": self.ev_Template, "Ident": self.ev_Ident,
            "Field": self.ev_Field, "OptField": self.ev_OptField,
            "Index": self.ev_Index, "OptIndex": self.ev_OptIndex,
            "Call": self.ev_Call, "OptCall": self.ev_OptCall, "Chain": self.ev_Chain, "Pipe": self.ev_Pipe,
            "Arith": self.ev_Arith, "Eq": self.ev_Eq, "Ne": self.ev_Ne,
            "Neg": self.ev_Neg, "Not": self.ev_Not, "Try": self.ev_Try,
            "And": self.ev_And, "Or": self.ev_Or, "Nullish": self.ev_Nullish,
            "IsKind": self.ev_IsKind, "IsTag": self.ev_IsTag,
            "AssignVar": self.ev_AssignVar, "AssignField": self.ev_AssignField,
            "AssignIndex": self.ev_AssignIndex, "AssignBad": self.ev_AssignBad,
            "If": self.ev_If, "While": self.ev_While, "For": self.ev_For,
            "Return": self.ev_Return, "Break": self.ev_Break, "Continue": self.ev_Continue,
            "Defer": self.ev_Defer, "Go": self.ev_Go, "Func": self.ev_Func,
            "RecordLit": self.ev_RecordLit, "MapLit": self.ev_MapLit, "ArrayLit": self.ev_ArrayLit,
            "Block": self.run_block, "Var": self.ev_Var,
        }

    def ev(self, node, scope):
        return self.table[node[0]](node, scope)

    # ------------------------------------------------------------------ names

    def lookup(self, scope, name):
        s = scope
        while s is not None:
            v = s.vars.get(name, MISSING)
            if v is not MISSING:
                if v is UNINIT:
                    panic("not initialized: " + name)
                return v
            if s.imports is not None:
                imp = s.imports.get(name)
                if imp is not None:
                    v = imp[0].get(imp[1], MISSING)
                    if v is MISSING:
                        panic("unbound name: " + name)
                    if v is UNINIT:
                        panic("not initialized: " + name)
                    return v
            s = s.parent
        panic("unbound name: " + name)

    def assign_var(self, scope, name, value):
        s = scope
        while s is not None:
            if name in s.vars:
                s.vars[name] = value
                return
            if s.imports is not None and name in s.imports:
                imp = s.imports[name]
                imp[0][imp[1]] = value       # writes through to the exporter (G31)
                return
            s = s.parent
        panic("unbound name: " + name)

    def ev_path(self, path, scope):
        """An identifier path: a lookup followed by field reads."""
        v = self.lookup(scope, path[0])
        for name in path[1:]:
            v = self.field(v, name)
        return v

    def field(self, obj, name):
        if type(obj) is not Record:
            panic("not a record")
        v = record_field(obj, name)
        if v is UNINIT:
            panic("not initialized: " + name)
        return v

    def index(self, obj, i):
        t = type(obj)
        if t is list:
            if type(i) is not int:
                panic("index not an int")
            if i >= 0:
                try:
                    return obj[i]
                except IndexError:    # out of range, or shrunk by another goroutine
                    pass
            panic("index out of range")
        if t is AntMap:
            return obj.get(i)
        panic("not indexable")

    # ------------------------------------------------------------------ statements and blocks

    def run_stmts(self, stmts, scope):
        """Evaluates statements in order in `scope`; the value of the last one,
        or void. Fills the trace of a panic raised here, at the innermost frame."""
        frame = scope.frame
        table = self.table
        value = VOID
        try:
            for line, node in stmts:
                frame.line = line
                value = table[node[0]](node, scope)
        except AntPanic as p:
            if p.trace is None:       # the innermost frame: no deferred block has run yet
                p.trace = trace_of(frame)
                self.unwinding.add(threading.get_ident())
            raise
        return value

    def run_stmts_with_defers(self, stmts, scope):
        """As run_stmts, for a block that registers deferred blocks: they run
        in reverse order on every exit, and a panic in one replaces whatever
        was pending (G59) while the rest still run."""
        defers = scope.defers = []
        pending = None
        value = VOID
        try:
            value = self.run_stmts(stmts, scope)
        except UNWINDING as e:
            pending = e
        for block, reg_scope in reversed(defers):
            try:
                self.run_deferred(block, reg_scope)
            except AntPanic as p:
                pending = p
        if pending is not None:
            raise pending
        return value

    def run_body(self, block, scope):
        """Evaluates a Block node directly in `scope` (a call, iteration, or goroutine scope)."""
        if block[2]:
            return self.run_stmts_with_defers(block[1], scope)
        return self.run_stmts(block[1], scope)

    def run_block(self, block, scope):
        """Evaluates a Block node in a fresh child scope."""
        return self.run_body(block, Scope({}, scope, scope.frame))

    def run_deferred(self, block, reg_scope):
        frame = Frame("defer", reg_scope.frame.path, 0, reg_scope.frame)
        try:
            self.run_body(block, Scope({}, reg_scope, frame))
        except ReturnSignal as r:
            if r.frame is not frame:
                raise
        except LOOP_SIGNALS:
            pass

    # ------------------------------------------------------------------ calls

    def call(self, fn, pos, named, frame):
        """Calls `fn` with positional values and (name, value) pairs; `frame`
        is the caller's frame."""
        t = type(fn)
        if t is Closure:
            return self.call_closure(fn, pos, named, frame)
        if t is Native:
            return self.call_native(fn, pos, named, frame)
        if t is YieldFn:
            return self.call_yield(fn, pos, named)
        panic("not a function")

    def call_closure(self, fn, pos, named, caller):
        frame = Frame(fn.name, fn.module, 0, caller)
        vars = {}
        if fn.is_lambda:
            if named:
                panic("named argument to lambda")
            if len(pos) > 1:
                panic("too many arguments")
            vars["x"] = pos[0] if pos else VOID
            scope = Scope(vars, fn.scope, frame)
        else:
            params = fn.params
            if len(pos) > len(params):
                panic("too many arguments")
            for (name, _), v in zip(params, pos):
                vars[name] = v
            for name, v in named:
                if name not in fn.param_names:
                    panic("unknown parameter: " + name)
                if name in vars:
                    panic("parameter already filled: " + name)
                vars[name] = v
            scope = Scope(vars, fn.scope, frame)
            if len(vars) < len(params):
                for name, default in params:       # defaults in parameter order, in the callee's scope
                    if name not in vars:
                        if default is None:
                            panic("missing argument: " + name)
                        vars[name] = self.ev(default, scope)
        try:
            return self.run_body(fn.body, scope)
        except ReturnSignal as r:
            if r.frame is frame:
                return r.value
            raise
        except LOOP_SIGNALS:
            panic("break or continue outside a loop")

    def call_native(self, fn, pos, named, frame):
        params = fn.params
        if len(pos) > len(params):
            panic("too many arguments")
        if named:
            args = list(pos) + [MISSING] * (len(params) - len(pos))
            for name, v in named:
                if name not in params:
                    panic("unknown parameter: " + name)
                i = params.index(name)
                if args[i] is not MISSING:
                    panic("parameter already filled: " + name)
                args[i] = v
            for i, a in enumerate(args):
                if a is MISSING:
                    panic("missing argument: " + params[i])
        else:
            if len(pos) < len(params):
                panic("missing argument: " + params[len(pos)])
            args = pos
        if fn.ctx:
            return fn.fn(self, frame, *args)
        return fn.fn(*args)

    def call_yield(self, y, pos, named):
        if y.done:
            panic("yield after done")
        if named:
            panic("unknown parameter: " + named[0][0])
        if not pos:
            panic("missing argument")
        if len(pos) > 1:
            panic("too many arguments")
        result = y.body(pos[0])
        if not result:
            y.done = True
        return result

    def eval_args(self, args, scope):
        pos, named = [], []
        for name, node in args:
            v = self.ev(node, scope)
            if name is None:
                pos.append(v)
            else:
                named.append((name, v))
        return pos, named

    # ------------------------------------------------------------------ expressions

    def ev_Const(self, node, scope):
        return node[1]

    def ev_Template(self, node, scope):
        out = []
        for part in node[1]:
            out.append(part if type(part) is str else render(self.ev(part, scope)))
        return "".join(out)

    def ev_Ident(self, node, scope):
        return self.lookup(scope, node[1])

    def ev_Field(self, node, scope):
        return self.field(self.ev(node[1], scope), node[2])

    def ev_OptField(self, node, scope):
        obj = self.ev(node[1], scope)
        if obj is None:
            raise OptNull
        return self.field(obj, node[2])

    def ev_Index(self, node, scope):
        obj = self.ev(node[1], scope)
        return self.index(obj, self.ev(node[2], scope))

    def ev_OptIndex(self, node, scope):
        obj = self.ev(node[1], scope)
        if obj is None:
            raise OptNull
        return self.index(obj, self.ev(node[2], scope))

    # --- calls: the callee evaluates first and must be a function, then the arguments (section 12)

    def ev_Call(self, node, scope):
        fn = self.ev(node[1], scope)
        if type(fn) not in FUNC_TYPES:
            panic("not a function")
        pos, named = self.eval_args(node[2], scope)
        return self.call(fn, pos, named, scope.frame)

    def ev_OptCall(self, node, scope):
        fn = self.ev(node[1], scope)
        if fn is None:
            raise OptNull
        if type(fn) not in FUNC_TYPES:
            panic("not a function")
        pos, named = self.eval_args(node[2], scope)
        return self.call(fn, pos, named, scope.frame)

    def ev_Chain(self, node, scope):
        try:
            return self.ev(node[1], scope)
        except OptNull:
            return None

    def ev_Pipe(self, node, scope):
        left = self.ev(node[1], scope)
        fn = self.ev(node[2], scope)
        if type(fn) not in FUNC_TYPES:
            panic("not a function")
        pos, named = self.eval_args(node[3], scope)
        pos.insert(0, left)
        return self.call(fn, pos, named, scope.frame)

    def ev_Arith(self, node, scope):
        a = self.ev(node[2], scope)
        b = self.ev(node[3], scope)
        return arith.binary(node[1], a, b)

    def ev_Eq(self, node, scope):
        a = self.ev(node[1], scope)
        return eq(a, self.ev(node[2], scope))

    def ev_Ne(self, node, scope):
        a = self.ev(node[1], scope)
        return not eq(a, self.ev(node[2], scope))

    def ev_Neg(self, node, scope):
        return arith.neg(self.ev(node[1], scope))

    def ev_Not(self, node, scope):
        return arith.not_(self.ev(node[1], scope))

    def ev_Try(self, node, scope):
        v = self.ev(node[1], scope)
        if v is None:
            raise ReturnSignal(scope.frame, None)
        if type(v) is Record:
            if v.tag is ERR:
                raise ReturnSignal(scope.frame, err(record_field(v, "error")))
            if v.tag is OK:
                return record_field(v, "value")
        return v

    def ev_And(self, node, scope):
        a = self.ev(node[1], scope)
        if type(a) is not bool:
            panic("not a bool")
        if not a:
            return False
        b = self.ev(node[2], scope)
        if type(b) is not bool:
            panic("not a bool")
        return b

    def ev_Or(self, node, scope):
        a = self.ev(node[1], scope)
        if type(a) is not bool:
            panic("not a bool")
        if a:
            return True
        b = self.ev(node[2], scope)
        if type(b) is not bool:
            panic("not a bool")
        return b

    def ev_Nullish(self, node, scope):
        a = self.ev(node[1], scope)
        if a is None:
            return self.ev(node[2], scope)
        return a

    def ev_IsKind(self, node, scope):
        return type(self.ev(node[1], scope)) in node[2]

    def ev_IsTag(self, node, scope):
        v = self.ev(node[1], scope)
        tag = self.ev_path(node[2], scope)
        if type(tag) is not Tag:
            panic("not a tag")
        return type(v) is Record and v.tag is tag

    # --- assignment: the target's sub-expressions evaluate and are checked, then the right side (section 12)

    def ev_AssignVar(self, node, scope):
        _, op, name, right = node
        if op is None:
            v = self.ev(right, scope)
        else:
            v = arith.binary(op, self.lookup(scope, name), self.ev(right, scope))
        self.assign_var(scope, name, v)
        return VOID

    def check_field_target(self, obj, name):
        """A field target must be a record that already has that field."""
        if type(obj) is not Record:
            panic("not a record")
        if name not in obj.fields:
            panic("no field: " + name)

    def check_index_target(self, obj, i):
        """An index target must be an array with an `int` index in range, or a map."""
        t = type(obj)
        if t is list:
            if type(i) is not int:
                panic("index not an int")
            if not 0 <= i < len(obj):
                panic("index out of range")
        elif t is not AntMap:
            panic("not indexable")

    def ev_AssignField(self, node, scope):
        _, op, obj_node, name, right = node
        obj = self.ev(obj_node, scope)
        self.check_field_target(obj, name)
        if op is None:
            v = self.ev(right, scope)
        else:
            v = arith.binary(op, self.field(obj, name), self.ev(right, scope))
        obj.fields[name] = v
        return VOID

    def ev_AssignIndex(self, node, scope):
        _, op, obj_node, idx_node, right = node
        obj = self.ev(obj_node, scope)
        i = self.ev(idx_node, scope)
        self.check_index_target(obj, i)
        if op is None:
            v = self.ev(right, scope)
        else:
            v = arith.binary(op, self.index(obj, i), self.ev(right, scope))
        if type(obj) is list:
            try:
                obj[i] = v
            except IndexError:          # the right side, or another goroutine, shrank the array
                panic("index out of range")
        else:
            obj.set(i, v)
        return VOID

    def ev_AssignBad(self, node, scope):
        panic("cannot assign")

    # --- control flow

    def ev_If(self, node, scope):
        c = self.ev(node[1], scope)
        if type(c) is not bool:
            panic("not a bool")
        if c:
            return self.run_block(node[2], scope)
        if node[3] is None:
            return VOID
        return self.ev(node[3], scope)

    def ev_While(self, node, scope):
        cond, body = node[1], node[2]
        while True:
            c = self.ev(cond, scope)
            if type(c) is not bool:
                panic("not a bool")
            if not c:
                return VOID
            try:
                self.run_block(body, scope)
            except ContinueSignal:
                continue
            except BreakSignal:
                return VOID

    def ev_For(self, node, scope):
        _, target, iterable, body = node
        it = self.ev(iterable, scope)
        frame = scope.frame
        is_name = target[0] == "Name"

        def iteration(v):
            """One iteration in a fresh scope; True to continue, False after a break."""
            s = Scope({}, scope, frame)
            if is_name:
                s.vars[target[1]] = v
            else:
                self.destructure(s.vars, target[1], v)
            try:
                self.run_body(body, s)
            except ContinueSignal:
                return True
            except BreakSignal:
                return False
            return True

        t = type(it)
        if t is list:
            i = 0
            while i < len(it):          # the length is re-read: appends are visited
                try:
                    v = it[i]
                except IndexError:      # popped by another goroutine: not visited
                    break
                i += 1
                if not iteration(v):
                    break
        elif t is AntMap:
            for k, v in it.items():
                if not iteration(entry(k, v)):
                    break
        elif t is str:
            for c in it:
                if not iteration(c):
                    break
        elif t in FUNC_TYPES:
            drive(self, frame, it, iteration)
        else:
            panic("not iterable")
        return VOID

    def destructure(self, vars, fields, v):
        if type(v) is not Record:
            panic("not a record")
        for name, local in fields:
            vars[local] = record_field(v, name)

    def ev_Return(self, node, scope):
        value = VOID if node[1] is None else self.ev(node[1], scope)
        raise ReturnSignal(scope.frame, value)

    def ev_Break(self, node, scope):
        raise BreakSignal

    def ev_Continue(self, node, scope):
        raise ContinueSignal

    def ev_Defer(self, node, scope):
        defers = scope.defers
        if defers is None:          # only outside any block (a top-level initializer): never runs
            defers = scope.defers = []
        defers.append((node[1], scope))
        return VOID

    def ev_Go(self, node, scope):
        frame = Frame("goroutine", scope.frame.path, scope.frame.line, None)
        child = Scope({}, scope, frame)
        thread = threading.Thread(target=self.run_goroutine, args=(node[1], child), daemon=True)
        thread.start()
        return VOID

    def ev_Func(self, node, scope):
        _, name, params, body, is_lambda = node
        return Closure(name, params, body, scope, is_lambda, scope.frame.path)

    # --- literals

    def ev_RecordLit(self, node, scope):
        tag = self.ev_path(node[1], scope)
        if type(tag) is not Tag:
            panic("not a tag")
        fields = {}
        for entry in node[2]:
            if entry[0] == "F":
                fields[entry[1]] = self.ev(entry[2], scope)
            else:
                src = self.ev(entry[1], scope)
                if type(src) is not Record:
                    panic("not a record")
                fields.update(src.fields)
        return Record(tag, fields)

    def ev_MapLit(self, node, scope):
        m = AntMap()
        for key, value in node[1]:
            k = self.ev(key, scope)
            m.set(k, self.ev(value, scope))
        return m

    def ev_ArrayLit(self, node, scope):
        out = []
        for kind, e in node[1]:
            v = self.ev(e, scope)
            if kind == "E":
                out.append(v)
            else:
                if type(v) is not list:
                    panic("not an array")
                out.extend(v)
        return out

    def ev_Var(self, node, scope):
        v = self.ev(node[2], scope)
        target = node[1]
        if target[0] == "Name":
            scope.vars[target[1]] = v
        else:
            self.destructure(scope.vars, target[1], v)
        return VOID

    # ------------------------------------------------------------------ modules

    def load_module(self, path, importer, line=0):
        """Loads, lowers, and initializes the module at `path` once (section 10).
        A module still on the loading stack is an import cycle, a compile
        error; the bootstrap reports it as a load error on the import line of
        the first module of the cycle (where the check tier marks it)."""
        for i, (start, start_line) in enumerate(self.loading):   # before the lookup: a loading module is registered
            if start == path:
                chain = [p for p, _ in self.loading[i:]] + [path]
                raise LoadError(start, start_line, 1,
                                "cycle: " + " -> ".join(os.path.basename(p) for p in chain))
        module = self.modules.get(path)
        if module is not None:
            return module
        try:
            with open(path, encoding="utf-8") as f:
                src = f.read()
        except OSError:
            raise LoadError(importer or path, line, 1, "not found: " + path)
        try:
            ast = parse_module(src)
        except antparse.ParseError as e:
            raise LoadError(path, e.line, e.col, e.msg)
        _, imports, decls = lower_file(ast, path)
        if importer is None:
            self.check_main(path, decls)

        vars = {}
        frame = Frame("module", path, 0, None)
        scope = Scope(vars, self.prelude, frame)
        scope.imports = {}
        module = Record(MODULE_TAG, vars)
        self.modules[path] = module

        base = os.path.dirname(path)
        for _, line, target, pattern in imports:
            self.loading.append((path, line))
            try:
                dep = self.load_module(os.path.normpath(os.path.join(base, target)), path, line)
            finally:
                self.loading.pop()
            if pattern[0] == "Namespace":
                vars[pattern[1]] = dep
            else:
                for name, alias in pattern[1]:
                    scope.imports[alias or name] = (dep.fields, name)

        for decl in decls:                       # hoisting: types, funcs, then UNINIT vars
            kind = decl[0]
            if kind == "Type":
                vars[decl[2]] = Tag(decl[2])
            elif kind == "Func":
                _, _, name, params, body = decl
                vars[name] = Closure(name, params, body, scope, False, path)
            elif decl[2][0] == "Name":
                vars[decl[2][1]] = UNINIT
            else:
                for _, local in decl[2][1]:
                    vars[local] = UNINIT
        for decl in decls:
            if decl[0] == "Var":
                _, line, target, expr = decl
                frame.line = line
                try:
                    self.ev_Var(("Var", target, expr), scope)
                except AntPanic as p:
                    if p.trace is None:
                        p.trace = trace_of(frame)
                    raise
                except ReturnSignal:          # a compile error (G24), reachable unchecked
                    raise AntPanic("return outside a function", trace_of(frame))
                except LOOP_SIGNALS:
                    raise AntPanic("break or continue outside a loop", trace_of(frame))
        return module

    # ------------------------------------------------------------------ roots

    def report_panic(self, p):
        """One write for the message and its trace, so the reports of two
        goroutines panicking at once never interleave."""
        report = "panic: " + p.message + "\n"
        for entry in p.trace or ():
            report += "  at " + entry + "\n"
        sys.stdout.flush()
        sys.stderr.write(report)
        sys.stderr.flush()

    def run_goroutine(self, block, scope):
        """The root of a goroutine: a panic ends it and is reported (section 9).
        The thread counts as unwinding until the report is written."""
        try:
            self.run_body(block, scope)
        except ReturnSignal:
            pass
        except LOOP_SIGNALS:
            pass
        except AntPanic as p:
            self.report_panic(p)
        except RecursionError:
            self.report_panic(AntPanic("stack overflow", trace_of(scope.frame)))
        except Exception:
            report_internal()
        finally:
            self.unwinding.discard(threading.get_ident())

    def check_main(self, path, decls):
        """Section 10: the entry module declares `func main()` with no
        parameters, diagnosed on its lowered declarations before anything
        runs (G32). A missing one is reported at the entry module's last
        declaration (G53); the return type is erased, so `main(): int` is left
        to the checker."""
        decl = next((d for d in decls if d[0] == "Func" and d[2] == "main"), None)
        if decl is None:
            raise LoadError(path, decls[-1][1] if decls else 1, 1, "main: missing or not a function")
        if decl[3]:                              # the params
            raise LoadError(path, decl[1], 1, "main: takes no parameters")

    def run_main(self, path):
        """Runs the entry module; the exit status: 0 when main returns, 2 on a
        panic, 1 on a load error."""
        try:
            module = self.load_module(os.path.abspath(path), None)
            self.call(module.fields["main"], [], (), None)
        except LoadError as e:
            sys.stdout.flush()
            sys.stderr.write(str(e) + "\n")
            sys.stderr.flush()
            return 1
        except AntPanic as p:
            self.report_panic(p)
            return 2
        except RecursionError:
            self.report_panic(AntPanic("stack overflow"))
            return 2
        except Exception:
            report_internal()
            return 2
        finally:
            self.unwinding.discard(threading.get_ident())
        sys.stdout.flush()
        return 0


# How long the exit waits for a goroutine that has panicked but not yet
# written its report (its deferred blocks may have released main already).
UNWIND_GRACE = 1.0


def run_program(path, args):
    """Runs the program on a thread with a large stack and returns the exit
    status. The program ends when main returns, whatever other goroutines
    are doing (section 12), except that one already past its `panic(...)` is
    given a bounded moment to write its report."""
    sys.setrecursionlimit(5_000_000)        # about ten Python frames per Ant call; frames live on the heap
    sys.setswitchinterval(0.005)
    threading.stack_size(512 * 1024 * 1024)
    interp = Interp(args)
    status = [1]

    def worker():
        try:
            status[0] = interp.run_main(path)
        except BaseException:               # no exit path may end in a bare traceback
            report_internal()
            status[0] = 2

    thread = threading.Thread(target=worker)
    thread.start()
    thread.join()
    deadline = time.monotonic() + UNWIND_GRACE
    while interp.unwinding and time.monotonic() < deadline:
        time.sleep(0.001)
    sys.stdout.flush()
    sys.stderr.flush()
    return status[0]
