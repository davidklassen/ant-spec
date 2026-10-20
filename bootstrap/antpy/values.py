"""Run-time values (ant-lang.md section 12, "Values").

Every Ant value is a Python object whose exact type names its kind, so that
`KIND[type(v)]` is one dictionary lookup. Python's `bool` is a subclass of
`int` and the sized numeric kinds below subclass `int` and `float`, so kind
tests always use `type(v) is ...`, never `isinstance`.
"""
import math
import struct


class AntPanic(BaseException):
    """A panic unwinding the current goroutine. `trace` is filled at the
    innermost statement runner and is a list of "name (path:line)" strings."""

    def __init__(self, message, trace=None):
        super().__init__(message)
        self.message = message
        self.trace = trace


def panic(message):
    raise AntPanic(message)


# ----------------------------------------------------------------------------- scalars

class Void:
    __slots__ = ()

    def __repr__(self):
        return "void"


VOID = Void()


class Sentinel:
    __slots__ = ("name",)

    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return self.name


UNINIT = Sentinel("UNINIT")     # a top-level var whose initializer has not run
MISSING = Sentinel("MISSING")   # internal "no such key"; never stored in a scope


class Int8(int):
    __slots__ = ()


class Int16(int):
    __slots__ = ()


class Int32(int):
    __slots__ = ()


class UInt8(int):
    __slots__ = ()


class UInt16(int):
    __slots__ = ()


class UInt32(int):
    __slots__ = ()


class UInt64(int):
    __slots__ = ()


def round_f32(x):
    """Round a float to float32 precision. Raises OverflowError when a finite
    value rounds to infinity (the conversion panics; arithmetic gives inf)."""
    return struct.unpack("f", struct.pack("f", x))[0]


class Float32(float):
    __slots__ = ()

    def __new__(cls, x):
        try:
            r = round_f32(x)
        except OverflowError:
            r = math.copysign(math.inf, x)
        return float.__new__(cls, r)


# ----------------------------------------------------------------------------- references

class Record:
    __slots__ = ("tag", "fields")

    def __init__(self, tag, fields):
        self.tag = tag
        self.fields = fields      # dict in creation order


class Tag:
    __slots__ = ("name",)

    def __init__(self, name):
        self.name = name


MODULE_TAG = Tag("Module")

# The prelude's record tags (section 11), shared by the library and the evaluator.
OK = Tag("Ok")
ERR = Tag("Err")
ERROR = Tag("Error")
ENTRY = Tag("Entry")
INDEXED = Tag("Indexed")


class Closure:
    """A declared function, an anonymous function, or a lambda."""
    __slots__ = ("name", "params", "param_names", "body", "scope", "is_lambda", "module")

    def __init__(self, name, params, body, scope, is_lambda, module):
        self.name = name              # for traces: the declared name, "func", or "lambda"
        self.params = params          # [(name, default_node_or_None)]
        self.param_names = frozenset(p[0] for p in params)
        self.body = body              # a lowered Block node
        self.scope = scope            # the captured scope
        self.is_lambda = is_lambda
        self.module = module          # path, for traces


class Native:
    """A library function. `params` names the parameters so that named
    arguments resolve; with `ctx` the function also receives the interpreter
    and the caller's frame (needed to call back into Ant)."""
    __slots__ = ("name", "params", "fn", "ctx")

    def __init__(self, name, params, fn, ctx=False):
        self.name = name
        self.params = params
        self.fn = fn
        self.ctx = ctx


class YieldFn:
    """The `yield` function a `for` loop hands to a sequence. `body(v)` runs
    one iteration and returns True to continue or False after a break."""
    __slots__ = ("body", "done")

    def __init__(self, body):
        self.body = body
        self.done = False


# ----------------------------------------------------------------------------- scopes and frames

class Scope:
    """A lexical scope. `vars` is the binding dict (shared with the module
    record for module scopes); `frame` the activation it belongs to; `defers`
    the block's defer list or None; `imports` the live name imports of a
    module scope (local name -> (exporter vars, exported name)) or None."""
    __slots__ = ("vars", "parent", "frame", "defers", "imports")

    def __init__(self, vars, parent, frame):
        self.vars = vars
        self.parent = parent
        self.frame = frame
        self.defers = None
        self.imports = None


class Frame:
    """A function activation, for stack traces: the chain through `parent`
    is the trace. `line` is the statement being evaluated."""
    __slots__ = ("name", "path", "line", "parent")

    def __init__(self, name, path, line, parent):
        self.name = name
        self.path = path
        self.line = line
        self.parent = parent


# ----------------------------------------------------------------------------- kinds

KIND = {
    int: "int64", Int8: "int8", Int16: "int16", Int32: "int32",
    UInt8: "uint8", UInt16: "uint16", UInt32: "uint32", UInt64: "uint64",
    float: "float64", Float32: "float32",
    bool: "bool", str: "string", type(None): "null", Void: "void",
    list: "array", Record: "record", Tag: "tag",
    Closure: "func", Native: "func", YieldFn: "func",
}
# equal.py adds its AntMap class under "map" (the map's lookup needs
# structural equality and hashing), sync.py its six classes under "sync", and
# fileio.py its File under "file".

INT_CLASS = {
    "int8": Int8, "int16": Int16, "int32": Int32, "int64": int,
    "uint8": UInt8, "uint16": UInt16, "uint32": UInt32, "uint64": UInt64,
}
INT_WIDTH = {
    "int8": 8, "int16": 16, "int32": 32, "int64": 64,
    "uint8": 8, "uint16": 16, "uint32": 32, "uint64": 64,
}
INT_RANGE = {
    "int8": (-2 ** 7, 2 ** 7 - 1), "int16": (-2 ** 15, 2 ** 15 - 1),
    "int32": (-2 ** 31, 2 ** 31 - 1), "int64": (-2 ** 63, 2 ** 63 - 1),
    "uint8": (0, 2 ** 8 - 1), "uint16": (0, 2 ** 16 - 1),
    "uint32": (0, 2 ** 32 - 1), "uint64": (0, 2 ** 64 - 1),
}
INT_KINDS = frozenset(INT_CLASS)
FLOAT_KINDS = frozenset(("float32", "float64"))
NUMBER_KINDS = INT_KINDS | FLOAT_KINDS
SIGNED_KINDS = frozenset(("int8", "int16", "int32", "int64"))

# The conversion functions, by name. `int` and `byte` are aliases (section 4).
CONVERSION_KINDS = {
    "int8": "int8", "int16": "int16", "int32": "int32", "int64": "int64",
    "uint8": "uint8", "uint16": "uint16", "uint32": "uint32", "uint64": "uint64",
    "float32": "float32", "float64": "float64", "int": "int64", "byte": "uint8",
}

FUNC_TYPES = (Closure, Native, YieldFn)
