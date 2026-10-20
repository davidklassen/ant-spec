"""Arithmetic, comparison, unary operators, numeric conversions, and the Bits
namespace (section 12, "Expressions" and "Runtime panics"; section 11)."""
import functools
import math
import operator

from antpy.values import (
    KIND, INT_KINDS, FLOAT_KINDS, NUMBER_KINDS, SIGNED_KINDS,
    INT_CLASS, INT_RANGE, INT_WIDTH, Float32, round_f32, panic, need_number, need_int,
)

COMPARE = {"<": operator.lt, "<=": operator.le, ">": operator.gt, ">=": operator.ge}


def binary(op, a, b):
    """`+ - * / %` and `< <= > >=` on two values."""
    ka, kb = KIND[type(a)], KIND[type(b)]
    if ka != kb:
        panic("kind mismatch")
    if ka == "string":
        if op == "+":
            return a + b
        cmp = COMPARE.get(op)
        if cmp is not None:
            return cmp(a, b)          # code point order
        panic("not a number")
    if ka not in NUMBER_KINDS:
        panic("not a number")
    cmp = COMPARE.get(op)
    if cmp is not None:
        return cmp(a, b)
    if ka in INT_KINDS:
        return int_arith(op, a, b, ka)
    return float_arith(op, a, b, ka)


def fit(kind, r, message="integer overflow"):
    """The Python integer `r` as a value of the integer `kind`, or the panic
    `message` when it lies outside the kind's range."""
    lo, hi = INT_RANGE[kind]
    if r < lo or r > hi:
        panic(message)
    return INT_CLASS[kind](r)


def int_arith(op, a, b, kind):
    if op == "+":
        r = a + b
    elif op == "-":
        r = a - b
    elif op == "*":
        r = a * b
    elif op == "/":
        if b == 0:
            panic("division by zero")
        r = abs(a) // abs(b)              # truncate toward zero
        if (a < 0) != (b < 0):
            r = -r
    else:
        if b == 0:
            panic("division by zero")
        r = abs(a) % abs(b)               # the sign of the dividend
        if a < 0:
            r = -r
    return fit(kind, r)


# The operator functions the lowering pass puts in Arith and compound
# assignment nodes: the (int, int) and (string, string) cases inline, since
# an `int` is exactly a Python int and a string a Python str, and everything
# else through `binary`, with its checks and panics.

INT64_LO, INT64_HI = INT_RANGE["int64"]


def op_add(a, b):
    ta = type(a)
    if ta is type(b):
        if ta is int:
            r = a + b
            if INT64_LO <= r <= INT64_HI:
                return r
            panic("integer overflow")
        if ta is str:
            return a + b
    return binary("+", a, b)


def int_op(op, fn):
    """`-` and `*`: the int case inline."""
    def f(a, b):
        if type(a) is int and type(b) is int:
            r = fn(a, b)
            if INT64_LO <= r <= INT64_HI:
                return r
            panic("integer overflow")
        return binary(op, a, b)
    return f


def ordered_op(op, fn):
    """`< <= > >=`: the int and string cases inline."""
    def f(a, b):
        ta = type(a)
        if ta is type(b) and (ta is int or ta is str):
            return fn(a, b)
        return binary(op, a, b)
    return f


OPS = {
    "+": op_add, "-": int_op("-", operator.sub), "*": int_op("*", operator.mul),
    "/": functools.partial(binary, "/"), "%": functools.partial(binary, "%"),
    "<": ordered_op("<", operator.lt), "<=": ordered_op("<=", operator.le),
    ">": ordered_op(">", operator.gt), ">=": ordered_op(">=", operator.ge),
}


def float_arith(op, a, b, kind):
    if op == "+":
        r = a + b
    elif op == "-":
        r = a - b
    elif op == "*":
        r = a * b
    elif op == "/":
        if b == 0.0:
            if a == 0.0 or a != a:
                r = math.nan
            else:
                r = math.copysign(math.inf, a) * math.copysign(1.0, b)
        else:
            r = a / b
    else:
        try:
            r = math.fmod(a, b)           # C fmod: the sign of the dividend (G6)
        except ValueError:                # zero divisor or infinite dividend
            r = math.nan
    if kind == "float32":
        return Float32(r)
    return r


def neg(v):
    k = KIND[type(v)]
    if k in INT_KINDS:
        return fit(k, -v)
    if k in FLOAT_KINDS:
        return Float32(-v) if k == "float32" else -v
    panic("not a number")


def need_bool(v):
    """The operand of `!`, `&&`, `||`, `if`, `while`, and the value a
    predicate callback returns: a bool, or `not a bool`."""
    if type(v) is not bool:
        panic("not a bool")
    return v


def not_(v):
    return not need_bool(v)


def convert(kind, v):
    """The ten conversion functions."""
    kv = KIND[type(need_number(v))]
    if kind in INT_KINDS:
        if kv in FLOAT_KINDS:
            if v != v or v in (math.inf, -math.inf):
                panic("conversion out of range")
            r = math.trunc(v)
        else:
            r = int(v)
        return fit(kind, r, "conversion out of range")
    if kind == "float64":
        return float(v)
    try:
        return Float32(round_f32(float(v)))
    except OverflowError:
        panic("conversion out of range")


# ----------------------------------------------------------------------------- Bits

def int_kind(v):
    k = KIND[type(v)]
    if k not in INT_KINDS:
        panic("wrong kind")
    return k


def same_int_kind(a, b):
    k = int_kind(a)
    if int_kind(b) != k:
        panic("wrong kind")
    return k


def wrap(x, kind):
    """Two's-complement wrap of a Python integer into the kind's width."""
    width = INT_WIDTH[kind]
    x &= (1 << width) - 1
    if kind in SIGNED_KINDS and x >= 1 << (width - 1):
        x -= 1 << width
    return INT_CLASS[kind](x)


def wrapping(op):
    """A Bits function of two integers of one kind: the kinds are checked
    before the operator runs (a string operand is `wrong kind`, never a
    Python error), and the result wraps into the kind."""
    def f(a, b):
        k = same_int_kind(a, b)
        return wrap(op(a, b), k)
    return f


bits_and = wrapping(operator.and_)
bits_or = wrapping(operator.or_)
bits_xor = wrapping(operator.xor)
bits_wrap_add = wrapping(operator.add)
bits_wrap_sub = wrapping(operator.sub)
bits_wrap_mul = wrapping(operator.mul)


def bits_not(a):
    k = int_kind(a)
    return wrap(~a, k)


def shift_count(a, n):
    k = int_kind(a)
    need_int(n)
    if n < 0 or n >= INT_WIDTH[k]:
        panic("shift count")
    return k


def bits_shift_left(a, n):
    k = shift_count(a, n)
    return wrap(a << n, k)


def bits_shift_right(a, n):
    k = shift_count(a, n)
    return wrap(a >> n, k)      # arithmetic for negatives; unsigned values are never negative


def bits_pop_count(a):
    k = int_kind(a)
    return bin(a & ((1 << INT_WIDTH[k]) - 1)).count("1")
