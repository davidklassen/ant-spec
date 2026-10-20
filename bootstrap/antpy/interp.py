"""The evaluator (ant-lang.md section 12): scopes, frames, control flow,
calls, loops, deferred blocks, panics, modules, and goroutines.

Control flow is Python exceptions. `ReturnSignal` carries the frame of the
function it leaves, so a `return` in a `for` body passes through the sequence
function driving the loop, and a `try` always leaves the function it is
written in. A panic unwinds as `AntPanic`; a block marked by the lowering pass
runs its deferred blocks on every exit.

Every node's head is its handler, one of the ev_* methods below, resolved by
the lowering pass through HANDLERS; `node[0](self, node, scope)` evaluates
it, and the hot handlers write that out instead of calling `ev`.
"""
import os
import sys
import threading
import time
import traceback

import antparse
from antpy.values import (
    KIND, VOID, UNINIT, MISSING, MODULE_TAG, OK, ERR, FUNC_TYPES,
    Scope, Frame, Record, Tag, Closure, Native, YieldFn, AntPanic, panic,
    record_field, entry, drive, err, live_elements,
)
from antpy.equal import AntMap, PRIMITIVE_TYPES, eq
from antpy import arith
from antpy.render import render
from antpy.lower import LoadError, parse_module, lower_file
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
EXIT_SIGNALS = (ReturnSignal,) + LOOP_SIGNALS
UNWINDING = (AntPanic,) + EXIT_SIGNALS
YIELD_NAMES = ("x",)


def trace_of(frame):
    lines = []
    while frame is not None:
        lines.append(f"{frame.name} ({frame.path}:{frame.line})")
        frame = frame.parent
    return lines


def need_callable(fn):
    """The callee of a call: a closure, native, or yield function, or
    `not a function`. A call node checks it before its arguments evaluate
    (section 12)."""
    if type(fn) not in FUNC_TYPES:
        panic("not a function")
    return fn


def as_record(v):
    if type(v) is not Record:
        panic("not a record")
    return v


def bind_args(names, pos, named):
    """Fills one slot per parameter name from the positional values and the
    (name, value) pairs (section 12, calls); MISSING marks a slot no argument
    filled."""
    if len(pos) > len(names):
        panic("too many arguments")
    slots = list(pos) + [MISSING] * (len(names) - len(pos))
    for name, v in named:
        if name not in names:
            panic("unknown parameter: " + name)
        i = names.index(name)
        if slots[i] is not MISSING:
            panic("parameter already filled: " + name)
        slots[i] = v
    return slots


def bind_target(vars, target, v):
    """Binds a `var` or `for` target in `vars`: a name, or the named fields
    of a value that must be a record (destructuring)."""
    if target[0] == "Name":
        vars[target[1]] = v
    else:
        as_record(v)
        for name, local in target[1]:
            vars[local] = record_field(v, name)


def target_names(target):
    """The names a target binds."""
    if target[0] == "Name":
        yield target[1]
    else:
        for _, local in target[1]:
            yield local


def read_module(path, importer, line):
    """The lowered (imports, declarations) of the module at `path`, or a
    LoadError: a missing file is reported on the import line of `importer`."""
    try:
        with open(path, encoding="utf-8") as f:
            src = f.read()
    except OSError:
        raise LoadError(importer or path, line, 1, "not found: " + path)
    try:
        ast = parse_module(src)
    except antparse.ParseError as e:
        raise LoadError(path, e.line, e.col, e.msg)
    _, imports, decls = lower_file(ast, path, HANDLERS)
    return imports, decls


def write_report(text):
    """Writes a report to stderr: stdout is flushed first so that the two
    streams keep their order on a shared terminal, and stderr after, since
    the process may end through os._exit."""
    sys.stdout.flush()
    sys.stderr.write(text)
    sys.stderr.flush()


def report_internal():
    """Reports a Python exception that escaped the evaluator: a bug in the interpreter."""
    write_report("panic: internal interpreter error\n" + traceback.format_exc())


class Interp:
    def __init__(self, args):
        self.args = args
        self.modules = {}            # absolute path -> module record
        self.loading = []            # (importer path, import line) of each import being loaded, outermost first
        self.unwinding = set()       # idents of threads between a panic and its report
        self.prelude = prelude.make_prelude()

    def ev(self, node, scope):
        return node[0](self, node, scope)

    # ------------------------------------------------------------------ names

    def lookup(self, scope, name):
        """Resolves a name outward through the scopes: a scope's own bindings,
        then the live imports of a module scope."""
        v = MISSING
        s = scope
        while s is not None:
            v = s.vars.get(name, MISSING)
            if v is not MISSING:
                break
            if s.imports is not None:
                imp = s.imports.get(name)
                if imp is not None:
                    v = imp[0].get(imp[1], MISSING)
                    break
            s = s.parent
        if v is MISSING:
            panic("unbound name: " + name)
        if v is UNINIT:
            panic("not initialized: " + name)
        return v

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

    def tag_at(self, path, scope):
        """The head of a record literal or the right side of `is`: a path that must evaluate to a tag."""
        tag = self.ev_path(path, scope)
        if type(tag) is not Tag:
            panic("not a tag")
        return tag

    def field(self, obj, name):
        """A field read `a.b`: `a` must be a record with the field, and the
        field initialized. ev_Field writes the same three checks out."""
        v = record_field(as_record(obj), name)
        if v is UNINIT:
            panic("not initialized: " + name)
        return v

    def index(self, obj, i):
        """An index read `a[i]`, and the one definition of a valid array index."""
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
        value = VOID
        try:
            for line, node in stmts:
                frame.line = line
                value = node[0](self, node, scope)
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
        """Evaluates a Block node: in a fresh child scope when it binds
        anything (a `var` or a `defer` of its own), and otherwise in `scope`
        itself, which such a block cannot change."""
        if block[3]:
            return self.run_body(block, Scope({}, scope, scope.frame))
        return self.run_stmts(block[1], scope)

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
        need_callable(fn)                # a library callback that is not a function

    def call_closure(self, fn, pos, named, caller):
        """A lambda is a function of the one parameter `x` with the default
        void; only its refusal of named arguments is special."""
        frame = Frame(fn.name, fn.module, 0, caller)
        names = fn.names
        if not named and len(pos) == len(names):        # every parameter filled by position
            scope = Scope(dict(zip(names, pos)), fn.scope, frame)
        else:
            if named and fn.is_lambda:
                panic("named argument to lambda")
            slots = bind_args(names, pos, named)
            vars = {name: v for name, v in zip(names, slots) if v is not MISSING}
            scope = Scope(vars, fn.scope, frame)
            for (name, default), v in zip(fn.params, slots):     # defaults in parameter order, in the callee's scope
                if v is MISSING:
                    if default is None:
                        panic("missing argument: " + name)
                    vars[name] = default[0](self, default, scope)
        body = fn.body
        try:
            if body[2]:
                return self.run_stmts_with_defers(body[1], scope)
            return self.run_stmts(body[1], scope)
        except ReturnSignal as r:
            if r.frame is frame:
                return r.value
            raise
        except LOOP_SIGNALS:
            panic("break or continue outside a loop")

    def call_native(self, fn, pos, named, frame):
        params = fn.params
        if named or len(pos) != len(params):
            pos = bind_args(params, pos, named)
            for name, v in zip(params, pos):
                if v is MISSING:
                    panic("missing argument: " + name)
        if fn.ctx:
            return fn.fn(self, frame, *pos)
        return fn.fn(*pos)

    def call_yield(self, y, pos, named):
        if y.done:
            panic("yield after done")
        if named:
            panic("unknown parameter: " + named[0][0])
        x = bind_args(YIELD_NAMES, pos, ())[0]
        if x is MISSING:
            panic("missing argument")
        result = y.body(x)
        if not result:
            y.done = True
        return result

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
        o = node[1]
        obj = o[0](self, o, scope)
        if type(obj) is not Record:                 # self.field, written out: the hottest read
            panic("not a record")
        v = obj.fields.get(node[2], MISSING)
        if v is MISSING:
            panic("no field: " + node[2])
        if v is UNINIT:
            panic("not initialized: " + node[2])
        return v

    def ev_Index(self, node, scope):
        o, i = node[1], node[2]
        obj = o[0](self, o, scope)
        return self.index(obj, i[0](self, i, scope))

    def ev_NullGuard(self, node, scope):
        """The operand of a `?.`: null leaves the enclosing chain with null."""
        v = self.ev(node[1], scope)
        if v is None:
            raise OptNull
        return v

    # --- calls: the callee evaluates first and must be a function, then the arguments (section 12)

    def ev_Call(self, node, scope):
        _, f, pos_nodes, named_nodes = node
        fn = f[0](self, f, scope)
        t = type(fn)
        if t not in FUNC_TYPES:
            panic("not a function")
        pos = [a[0](self, a, scope) for a in pos_nodes]
        named = [(name, a[0](self, a, scope)) for name, a in named_nodes]
        if t is Closure:
            return self.call_closure(fn, pos, named, scope.frame)
        if t is Native:
            return self.call_native(fn, pos, named, scope.frame)
        return self.call_yield(fn, pos, named)

    def ev_Chain(self, node, scope):
        try:
            return self.ev(node[1], scope)
        except OptNull:
            return None

    def ev_Pipe(self, node, scope):
        _, l, f, pos_nodes, named_nodes = node
        left = l[0](self, l, scope)
        fn = need_callable(f[0](self, f, scope))
        pos = [left] + [a[0](self, a, scope) for a in pos_nodes]
        named = [(name, a[0](self, a, scope)) for name, a in named_nodes]
        return self.call(fn, pos, named, scope.frame)

    def ev_Arith(self, node, scope):
        _, op, l, r = node
        a = l[0](self, l, scope)
        return op(a, r[0](self, r, scope))

    def ev_Eq(self, node, scope):
        l, r = node[1], node[2]
        a = l[0](self, l, scope)
        b = r[0](self, r, scope)
        ta = type(a)
        if ta is type(b) and ta in PRIMITIVE_TYPES:   # eq's own first step, written out
            return a == b
        return eq(a, b)

    def ev_Ne(self, node, scope):
        l, r = node[1], node[2]
        a = l[0](self, l, scope)
        b = r[0](self, r, scope)
        ta = type(a)
        if ta is type(b) and ta in PRIMITIVE_TYPES:
            return a != b
        return not eq(a, b)

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

    # --- `&& || if while`: the bool check (arith.need_bool) written out

    def ev_And(self, node, scope):
        l, r = node[1], node[2]
        a = l[0](self, l, scope)
        if type(a) is not bool:
            panic("not a bool")
        if not a:
            return False
        b = r[0](self, r, scope)
        if type(b) is not bool:
            panic("not a bool")
        return b

    def ev_Or(self, node, scope):
        l, r = node[1], node[2]
        a = l[0](self, l, scope)
        if type(a) is not bool:
            panic("not a bool")
        if a:
            return True
        b = r[0](self, r, scope)
        if type(b) is not bool:
            panic("not a bool")
        return b

    def ev_Nullish(self, node, scope):
        a = self.ev(node[1], scope)
        if a is None:
            return self.ev(node[2], scope)
        return a

    def ev_IsKind(self, node, scope):
        return KIND[type(self.ev(node[1], scope))] == node[2]

    def ev_IsTag(self, node, scope):
        v = self.ev(node[1], scope)
        tag = self.tag_at(node[2], scope)
        return type(v) is Record and v.tag is tag

    # --- assignment: the target's sub-expressions evaluate and are checked, then the right side (section 12)

    def ev_AssignVar(self, node, scope):
        _, op, name, right = node
        if op is None:
            v = right[0](self, right, scope)
        else:
            v = op(self.lookup(scope, name), right[0](self, right, scope))
        self.assign_var(scope, name, v)
        return VOID

    def ev_AssignField(self, node, scope):
        _, op, o, name, right = node
        obj = o[0](self, o, scope)
        if type(obj) is not Record:
            panic("not a record")
        fields = obj.fields
        current = fields.get(name, MISSING)
        if current is MISSING:
            panic("no field: " + name)
        if op is None:
            v = right[0](self, right, scope)
        else:
            if current is UNINIT:
                panic("not initialized: " + name)
            v = op(current, right[0](self, right, scope))
        fields[name] = v
        return VOID

    def ev_AssignIndex(self, node, scope):
        _, op, o, i_node, right = node
        obj = o[0](self, o, scope)
        i = i_node[0](self, i_node, scope)
        if type(obj) is AntMap:
            if op is None:
                v = right[0](self, right, scope)
            else:
                v = op(obj.get(i), right[0](self, right, scope))
            obj.set(i, v)
            return VOID
        current = self.index(obj, i)             # an array with a valid index, or the read's panic
        if op is None:
            v = right[0](self, right, scope)
        else:
            v = op(current, right[0](self, right, scope))
        try:
            obj[i] = v
        except IndexError:          # the right side, or another goroutine, shrank the array
            panic("index out of range")
        return VOID

    def ev_AssignBad(self, node, scope):
        panic("cannot assign")

    # --- control flow

    def ev_If(self, node, scope):
        _, cond, then, otherwise = node
        c = cond[0](self, cond, scope)
        if type(c) is not bool:
            panic("not a bool")
        if c:
            return self.run_block(then, scope)
        if otherwise is None:
            return VOID
        return otherwise[0](self, otherwise, scope)

    def ev_While(self, node, scope):
        cond, body = node[1], node[2]
        while True:
            c = cond[0](self, cond, scope)
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

        def iteration(v):
            """One iteration in a fresh scope; True to continue, False after a break."""
            s = Scope({}, scope, frame)
            bind_target(s.vars, target, v)
            try:
                self.run_body(body, s)
            except ContinueSignal:
                return True
            except BreakSignal:
                return False
            return True

        t = type(it)
        if t is list:
            for v in live_elements(it):
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

    def ev_Return(self, node, scope):
        e = node[1]
        raise ReturnSignal(scope.frame, VOID if e is None else e[0](self, e, scope))

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
        _, name, params, names, body, is_lambda = node
        return Closure(name, params, names, body, scope, is_lambda, scope.frame.path)

    # --- literals

    def ev_RecordLit(self, node, scope):
        tag = self.tag_at(node[1], scope)
        fields = {}
        for item in node[2]:
            if item[0] == "F":
                fields[item[1]] = self.ev(item[2], scope)
            else:
                fields.update(as_record(self.ev(item[1], scope)).fields)
        return Record(tag, fields)

    def ev_MapLit(self, node, scope):
        m = AntMap()
        m.update((self.ev(key, scope), self.ev(value, scope)) for key, value in node[1])
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
        e = node[2]
        bind_target(scope.vars, node[1], e[0](self, e, scope))
        return VOID

    # ------------------------------------------------------------------ modules

    def load_module(self, path, importer, line=0):
        """Loads, lowers, and initializes the module at `path` once (section
        10): the cycle check, the cache, the read, the imports, the hoisting
        of its declarations, and its top-level initializers in order."""
        self.check_cycle(path)
        module = self.modules.get(path)
        if module is not None:
            return module
        imports, decls = read_module(path, importer, line)
        if importer is None:
            self.check_main(path, decls)

        vars = {}
        frame = Frame("module", path, 0, None)
        scope = Scope(vars, self.prelude, frame)
        scope.imports = {}
        module = Record(MODULE_TAG, vars)
        self.modules[path] = module
        self.bind_imports(path, imports, scope)
        self.hoist(decls, scope)
        self.init_vars(decls, scope)
        return module

    def check_cycle(self, path):
        """A module still on the loading stack is an import cycle, a compile
        error; the bootstrap reports it as a load error on the import line of
        the first module of the cycle (where the check tier marks it). Checked
        before the cache lookup: a loading module is already registered."""
        for i, (start, start_line) in enumerate(self.loading):
            if start == path:
                chain = [p for p, _ in self.loading[i:]] + [path]
                raise LoadError(start, start_line, 1,
                                "cycle: " + " -> ".join(os.path.basename(p) for p in chain))

    def bind_imports(self, path, imports, scope):
        """Loads each imported module and binds it: a namespace import as the
        module record, a name import as a live link to the exporter (G31)."""
        base = os.path.dirname(path)
        for _, line, target, pattern in imports:
            self.loading.append((path, line))
            try:
                dep = self.load_module(os.path.normpath(os.path.join(base, target)), path, line)
            finally:
                self.loading.pop()
            if pattern[0] == "Namespace":
                scope.vars[pattern[1]] = dep
            else:
                for name, alias in pattern[1]:
                    scope.imports[alias or name] = (dep.fields, name)

    def hoist(self, decls, scope):
        """Binds every declaration before any initializer runs: types to
        tags, functions to closures, and the names of `var`s to UNINIT."""
        vars = scope.vars
        for kind, _, node in decls:
            if kind == "Type":
                vars[node] = Tag(node)
            elif kind == "Func":
                vars[node[1]] = self.ev_Func(node, scope)
            else:
                for name in target_names(node[1]):
                    vars[name] = UNINIT

    def init_vars(self, decls, scope):
        """Runs the top-level `var` initializers in order, as the module's
        statements. A `return`, `break`, or `continue` there is a compile
        error (G24), reachable unchecked."""
        frame = scope.frame
        try:
            self.run_stmts([(line, node) for kind, line, node in decls if kind == "Var"], scope)
        except ReturnSignal:
            raise AntPanic("return outside a function", trace_of(frame))
        except LOOP_SIGNALS:
            raise AntPanic("break or continue outside a loop", trace_of(frame))

    def check_main(self, path, decls):
        """Section 10: the entry module declares `func main()` with no
        parameters, diagnosed on its lowered declarations before anything
        runs (G32). A missing one is reported at the entry module's last
        declaration (G53); the return type is erased, so `main(): int` is left
        to the checker."""
        decl = next((d for d in decls if d[0] == "Func" and d[2][1] == "main"), None)
        if decl is None:
            raise LoadError(path, decls[-1][1] if decls else 1, 1, "main: missing or not a function")
        if decl[2][2]:                           # the params
            raise LoadError(path, decl[1], 1, "main: takes no parameters")

    # ------------------------------------------------------------------ roots

    def report_panic(self, p):
        """One write for the message and its trace, so the reports of two
        goroutines panicking at once never interleave."""
        report = "panic: " + p.message + "\n"
        for line in p.trace or ():
            report += "  at " + line + "\n"
        write_report(report)

    def run_root(self, run, frame):
        """The root of a goroutine: runs `run` and reports what escapes it,
        a panic with its trace (section 12), a RecursionError as the panic
        `stack overflow` at `frame`, any other Python exception as a bug.
        Returns main's exit status for the outcome: 0, or 2 after a report.
        The thread counts as unwinding until the report is written."""
        try:
            run()
            return 0
        except LoadError:
            raise                        # the entry's load error, reported by run_main
        except AntPanic as p:
            self.report_panic(p)
        except RecursionError:
            self.report_panic(AntPanic("stack overflow", trace_of(frame)))
        except Exception:
            report_internal()
        finally:
            self.unwinding.discard(threading.get_ident())
        return 2

    def run_goroutine(self, block, scope):
        """A goroutine: a panic ends it and is reported; so does a `return`,
        `break`, or `continue` at its root, silently (section 9)."""
        def run():
            try:
                self.run_body(block, scope)
            except EXIT_SIGNALS:
                pass

        self.run_root(run, scope.frame)

    def run_main(self, path):
        """Runs the entry module; the exit status: 0 when main returns, 2 on a
        panic, 1 on a load error."""
        def run():
            module = self.load_module(os.path.abspath(path), None)
            self.call(module.fields["main"], [], (), None)

        try:
            status = self.run_root(run, None)
        except LoadError as e:
            write_report(str(e) + "\n")
            return 1
        sys.stdout.flush()
        return status


# The handler of each node kind, for the lowering pass: the head of every node.
HANDLERS = {
    "Const": Interp.ev_Const, "Template": Interp.ev_Template, "Ident": Interp.ev_Ident,
    "Field": Interp.ev_Field, "Index": Interp.ev_Index, "Call": Interp.ev_Call,
    "NullGuard": Interp.ev_NullGuard, "Chain": Interp.ev_Chain, "Pipe": Interp.ev_Pipe,
    "Arith": Interp.ev_Arith, "Eq": Interp.ev_Eq, "Ne": Interp.ev_Ne,
    "Neg": Interp.ev_Neg, "Not": Interp.ev_Not, "Try": Interp.ev_Try,
    "And": Interp.ev_And, "Or": Interp.ev_Or, "Nullish": Interp.ev_Nullish,
    "IsKind": Interp.ev_IsKind, "IsTag": Interp.ev_IsTag,
    "AssignVar": Interp.ev_AssignVar, "AssignField": Interp.ev_AssignField,
    "AssignIndex": Interp.ev_AssignIndex, "AssignBad": Interp.ev_AssignBad,
    "If": Interp.ev_If, "While": Interp.ev_While, "For": Interp.ev_For,
    "Return": Interp.ev_Return, "Break": Interp.ev_Break, "Continue": Interp.ev_Continue,
    "Defer": Interp.ev_Defer, "Go": Interp.ev_Go, "Func": Interp.ev_Func,
    "RecordLit": Interp.ev_RecordLit, "MapLit": Interp.ev_MapLit, "ArrayLit": Interp.ev_ArrayLit,
    "Block": Interp.run_block, "Var": Interp.ev_Var,
}

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
