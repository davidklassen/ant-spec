"""Arithmetic, comparison, unary operators, numeric conversions, and the Bits
namespace (section 12, "Expressions" and "Runtime panics"; section 11)."""
import math

from antpy.values import (
    KIND, INT_KINDS, FLOAT_KINDS, NUMBER_KINDS, SIGNED_KINDS,
    INT_CLASS, INT_RANGE, INT_WIDTH, Float32, round_f32, panic,
)

COMPARE = {
    "<": lambda a, b: a < b,
    "<=": lambda a, b: a <= b,
    ">": lambda a, b: a > b,
    ">=": lambda a, b: a >= b,
}


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
    lo, hi = INT_RANGE[kind]
    if r < lo or r > hi:
        panic("integer overflow")
    return INT_CLASS[kind](r)


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
        r = -v
        lo, hi = INT_RANGE[k]
        if r < lo or r > hi:
            panic("integer overflow")
        return INT_CLASS[k](r)
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
    kv = KIND[type(v)]
    if kv not in NUMBER_KINDS:
        panic("wrong kind")
    if kind in INT_KINDS:
        if kv in FLOAT_KINDS:
            if v != v or v in (math.inf, -math.inf):
                panic("conversion out of range")
            r = math.trunc(v)
        else:
            r = int(v)
        lo, hi = INT_RANGE[kind]
        if r < lo or r > hi:
            panic("conversion out of range")
        return INT_CLASS[kind](r)
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


def bits_and(a, b):
    return wrap(a & b, same_int_kind(a, b))


def bits_or(a, b):
    return wrap(a | b, same_int_kind(a, b))


def bits_xor(a, b):
    return wrap(a ^ b, same_int_kind(a, b))


def bits_not(a):
    return wrap(~a, int_kind(a))


def shift_count(a, n):
    k = int_kind(a)
    if type(n) is not int:
        panic("wrong kind")
    if n < 0 or n >= INT_WIDTH[k]:
        panic("shift count")
    return k


def bits_shift_left(a, n):
    return wrap(a << n, shift_count(a, n))


def bits_shift_right(a, n):
    return wrap(a >> n, shift_count(a, n))   # arithmetic for negatives; unsigned values are never negative


def bits_pop_count(a):
    k = int_kind(a)
    return bin(a & ((1 << INT_WIDTH[k]) - 1)).count("1")


def bits_wrap_add(a, b):
    return wrap(a + b, same_int_kind(a, b))


def bits_wrap_sub(a, b):
    return wrap(a - b, same_int_kind(a, b))


def bits_wrap_mul(a, b):
    return wrap(a * b, same_int_kind(a, b))
