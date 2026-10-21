/* Arithmetic, comparison, unary operators, conversions, Bits and Math
 * (section 12 "Expressions" and "Runtime panics"; section 11). */
#include <float.h>
#include <math.h>
#include "ant.h"

/* ---------------------------------------------------------------- integer kinds */

/* One case per integer kind: T is the C type, F the union field. */
#define INT_KINDS(X) \
  X(K_INT8, int8_t, i) X(K_INT16, int16_t, i) X(K_INT32, int32_t, i) X(K_INT64, int64_t, i) \
  X(K_UINT8, uint8_t, u) X(K_UINT16, uint16_t, u) X(K_UINT32, uint32_t, u) X(K_UINT64, uint64_t, u)

static AntValue overflow(void) {
  ant_panic("integer overflow");
  return ANT_VOID;
}

static AntValue mk(AntKind k, int64_t i, uint64_t u) {
  AntValue v;
  v.kind = k;
  if (k >= K_UINT8) v.as.u = u; else v.as.i = i;
  return v;
}

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_LT, OP_LE, OP_GT, OP_GE };

static AntValue int_op(int op, AntValue a, AntValue b) {
  switch (a.kind) {
#define CASE(K, T, F) \
    case K: { \
      T x = (T)a.as.F, y = (T)b.as.F, r; \
      switch (op) { \
        case OP_ADD: if (__builtin_add_overflow(x, y, &r)) return overflow(); break; \
        case OP_SUB: if (__builtin_sub_overflow(x, y, &r)) return overflow(); break; \
        case OP_MUL: if (__builtin_mul_overflow(x, y, &r)) return overflow(); break; \
        case OP_DIV: \
          if (y == 0) { ant_panic("division by zero"); return ANT_VOID; } \
          if ((T)-1 < (T)0 && y == (T)-1) { if (__builtin_sub_overflow((T)0, x, &r)) return overflow(); } \
          else r = (T)(x / y); \
          break; \
        case OP_MOD: \
          if (y == 0) { ant_panic("division by zero"); return ANT_VOID; } \
          if ((T)-1 < (T)0 && y == (T)-1) r = 0; else r = (T)(x % y); \
          break; \
        case OP_LT: return ant_bool(x < y); \
        case OP_LE: return ant_bool(x <= y); \
        case OP_GT: return ant_bool(x > y); \
        default: return ant_bool(x >= y); \
      } \
      return mk(K, (int64_t)r, (uint64_t)r); \
    }
    INT_KINDS(CASE)
#undef CASE
    default: return ANT_VOID;
  }
}

static AntValue float_op(int op, AntValue a, AntValue b) {
  if (a.kind == K_FLOAT32) {
    float x = a.as.f32, y = b.as.f32, r;
    AntValue v = ant_mk(K_FLOAT32);
    switch (op) {
      case OP_ADD: r = x + y; break;
      case OP_SUB: r = x - y; break;
      case OP_MUL: r = x * y; break;
      case OP_DIV: r = x / y; break;
      case OP_MOD: r = fmodf(x, y); break;
      case OP_LT: return ant_bool(x < y);
      case OP_LE: return ant_bool(x <= y);
      case OP_GT: return ant_bool(x > y);
      default: return ant_bool(x >= y);
    }
    v.as.f32 = r;
    return v;
  }
  double x = a.as.f, y = b.as.f;
  switch (op) {
    case OP_ADD: return ant_float(x + y);
    case OP_SUB: return ant_float(x - y);
    case OP_MUL: return ant_float(x * y);
    case OP_DIV: return ant_float(x / y);
    case OP_MOD: return ant_float(fmod(x, y));
    case OP_LT: return ant_bool(x < y);
    case OP_LE: return ant_bool(x <= y);
    case OP_GT: return ant_bool(x > y);
    default: return ant_bool(x >= y);
  }
}

/* `+ - * / %` and `< <= > >=`: the kinds must agree (`kind mismatch`), then
 * strings take `+` and the comparisons, otherwise the kind must be numeric. */
static AntValue binary(int op, AntValue a, AntValue b) {
  if (a.kind != b.kind) { ant_panic("kind mismatch"); return ANT_VOID; }
  if (a.kind == K_STRING) {
    if (op == OP_ADD) return ant_str_concat(a, b);
    if (op >= OP_LT) {
      int c = ant_str_cmp(a.as.p, b.as.p);
      switch (op) {
        case OP_LT: return ant_bool(c < 0);
        case OP_LE: return ant_bool(c <= 0);
        case OP_GT: return ant_bool(c > 0);
        default: return ant_bool(c >= 0);
      }
    }
    ant_panic("not a number");
    return ANT_VOID;
  }
  if (a.kind <= K_UINT64) return int_op(op, a, b);
  if (a.kind <= K_FLOAT64) return float_op(op, a, b);
  ant_panic("not a number");
  return ANT_VOID;
}

/* Two ints, the common case, skip the dispatch. */
#define FAST(a, b) ((a).kind == K_INT64 && (b).kind == K_INT64)

AntValue ant_add(AntValue a, AntValue b) {
  int64_t r;
  if (FAST(a, b)) return __builtin_add_overflow(a.as.i, b.as.i, &r) ? overflow() : ant_int(r);
  return binary(OP_ADD, a, b);
}

AntValue ant_sub(AntValue a, AntValue b) {
  int64_t r;
  if (FAST(a, b)) return __builtin_sub_overflow(a.as.i, b.as.i, &r) ? overflow() : ant_int(r);
  return binary(OP_SUB, a, b);
}

AntValue ant_mul(AntValue a, AntValue b) {
  int64_t r;
  if (FAST(a, b)) return __builtin_mul_overflow(a.as.i, b.as.i, &r) ? overflow() : ant_int(r);
  return binary(OP_MUL, a, b);
}

AntValue ant_div(AntValue a, AntValue b) { return binary(OP_DIV, a, b); }
AntValue ant_mod(AntValue a, AntValue b) { return binary(OP_MOD, a, b); }
AntValue ant_lt(AntValue a, AntValue b) { return FAST(a, b) ? ant_bool(a.as.i < b.as.i) : binary(OP_LT, a, b); }
AntValue ant_le(AntValue a, AntValue b) { return FAST(a, b) ? ant_bool(a.as.i <= b.as.i) : binary(OP_LE, a, b); }
AntValue ant_gt(AntValue a, AntValue b) { return FAST(a, b) ? ant_bool(a.as.i > b.as.i) : binary(OP_GT, a, b); }
AntValue ant_ge(AntValue a, AntValue b) { return FAST(a, b) ? ant_bool(a.as.i >= b.as.i) : binary(OP_GE, a, b); }

bool ant_less(AntValue a, AntValue b) {
  AntValue r = binary(OP_LT, a, b);
  return r.kind == K_BOOL && r.as.b;
}

AntValue ant_neg(AntValue a) {
  switch (a.kind) {
#define CASE(K, T, F) case K: { T x = (T)a.as.F, r; if (__builtin_sub_overflow((T)0, x, &r)) return overflow(); return mk(K, (int64_t)r, (uint64_t)r); }
    INT_KINDS(CASE)
#undef CASE
    case K_FLOAT32: { AntValue v = ant_mk(K_FLOAT32); v.as.f32 = -a.as.f32; return v; }
    case K_FLOAT64: return ant_float(-a.as.f);
    default: ant_panic("not a number"); return ANT_VOID;
  }
}

AntValue ant_not(AntValue a) {
  bool b = ant_truth(a);
  return ant_g->unwind.mode ? ANT_VOID : ant_bool(!b);
}

/* ---------------------------------------------------------------- conversions */

static AntValue out_of_range(void) {
  ant_panic("conversion out of range");
  return ANT_VOID;
}

static AntValue convert(AntKind k, AntValue v) {
  NUM(v);
  if (k == K_FLOAT64 || k == K_FLOAT32) {
    double d;
    if (v.kind == K_FLOAT64) d = v.as.f;
    else if (v.kind == K_FLOAT32) d = v.as.f32;
    else if (v.kind >= K_UINT8) d = (double)v.as.u;
    else d = (double)v.as.i;
    if (k == K_FLOAT64) return ant_float(d);
    if (isfinite(d) && fabs(d) > FLT_MAX) return out_of_range();
    AntValue r = ant_mk(K_FLOAT32);
    r.as.f32 = (float)d;
    return r;
  }
  /* an integer target: the value as a signed or unsigned 64-bit integer, then the range check */
  bool neg = false;
  uint64_t mag;
  if (v.kind == K_FLOAT64 || v.kind == K_FLOAT32) {
    double d = v.kind == K_FLOAT64 ? v.as.f : (double)v.as.f32;
    if (!isfinite(d)) return out_of_range();
    d = trunc(d);
    if (d >= 18446744073709551616.0 || d < -9223372036854775808.0) return out_of_range();
    neg = d < 0;
    mag = neg ? (uint64_t)(-d) : (uint64_t)d;
  } else if (v.kind >= K_UINT8) {
    mag = v.as.u;
  } else {
    neg = v.as.i < 0;
    mag = neg ? (uint64_t)0 - (uint64_t)v.as.i : (uint64_t)v.as.i;
  }
  switch (k) {
#define CASE(K, T, F) \
    case K: { \
      if ((T)-1 > (T)0) { if (neg && mag) return out_of_range(); if (mag > (uint64_t)(T)-1) return out_of_range(); return mk(K, (int64_t)mag, mag); } \
      uint64_t lim = ((uint64_t)1 << (sizeof(T) * 8 - 1)); \
      if (neg ? mag > lim : mag >= lim) return out_of_range(); \
      int64_t r = neg ? (int64_t)(0 - mag) : (int64_t)mag; \
      return mk(K, r, (uint64_t)r); \
    }
    INT_KINDS(CASE)
#undef CASE
    default: return ANT_VOID;
  }
}

static AntValue convert_native(AntFunc *self, int argc, AntValue *argv) {
  return convert((AntKind)(intptr_t)self->data, argv[0]);
}

AntValue ant_conversion(const char *name, AntKind k) {
  return ant_native(name, "x", convert_native, (void *)(intptr_t)k);
}

/* ---------------------------------------------------------------- Bits */

static int int_width(AntKind k) {
  switch (k) {
    case K_INT8: case K_UINT8: return 8;
    case K_INT16: case K_UINT16: return 16;
    case K_INT32: case K_UINT32: return 32;
    default: return 64;
  }
}

/* Two's-complement wrap of 64 bits into the kind's width. */
static AntValue wrap(AntKind k, uint64_t bits) {
  int w = int_width(k);
  if (w < 64) bits &= ((uint64_t)1 << w) - 1;
  if (k < K_UINT8 && w < 64 && (bits >> (w - 1)) & 1) bits |= ~(((uint64_t)1 << w) - 1);
  return mk(k, (int64_t)bits, bits);
}

static bool need_int_kind(AntValue a) {
  return ant_is_int(a.kind) || ant_wrong_kind("an integer");
}

static bool need_same_int(AntValue a, AntValue b) {
  if (!need_int_kind(a) || !need_int_kind(b)) return false;
  return a.kind == b.kind || ant_wrong_kind("integers of one kind");
}

#define BITS2(NAME, EXPR) \
  static AntValue NAME(AntFunc *self, int argc, AntValue *argv) { \
    AntValue a = argv[0], b = argv[1]; \
    if (!need_same_int(a, b)) return ANT_VOID; \
    uint64_t x = a.as.u, y = b.as.u; \
    return wrap(a.kind, (EXPR)); \
  }
BITS2(b_and, x & y)
BITS2(b_or, x | y)
BITS2(b_xor, x ^ y)
BITS2(b_wrap_add, x + y)
BITS2(b_wrap_sub, x - y)
BITS2(b_wrap_mul, x * y)

static AntValue b_not(AntFunc *self, int argc, AntValue *argv) {
  if (!need_int_kind(argv[0])) return ANT_VOID;
  return wrap(argv[0].kind, ~argv[0].as.u);
}

static bool shift_count(AntValue a, AntValue n) {
  if (!need_int_kind(a)) return false;
  ant_need_int(n);
  if (ant_g->unwind.mode) return false;
  if (n.as.i < 0 || n.as.i >= int_width(a.kind)) { ant_panic("shift count"); return false; }
  return true;
}

static AntValue b_shift_left(AntFunc *self, int argc, AntValue *argv) {
  if (!shift_count(argv[0], argv[1])) return ANT_VOID;
  return wrap(argv[0].kind, argv[0].as.u << argv[1].as.i);
}

static AntValue b_shift_right(AntFunc *self, int argc, AntValue *argv) {
  AntValue a = argv[0];
  if (!shift_count(a, argv[1])) return ANT_VOID;
  int n = (int)argv[1].as.i;
  if (a.kind < K_UINT8) return wrap(a.kind, (uint64_t)(a.as.i >> n));   /* arithmetic */
  return wrap(a.kind, a.as.u >> n);
}

static AntValue b_pop_count(AntFunc *self, int argc, AntValue *argv) {
  if (!need_int_kind(argv[0])) return ANT_VOID;
  uint64_t bits = argv[0].as.u;
  int w = int_width(argv[0].kind);
  if (w < 64) bits &= ((uint64_t)1 << w) - 1;
  return ant_int(__builtin_popcountll(bits));
}

void ant_ns_bits(void) {
  static const AntMember members[] = {
    { "and", "a,b", b_and },
    { "or", "a,b", b_or },
    { "xor", "a,b", b_xor },
    { "not", "a", b_not },
    { "shiftLeft", "a,n", b_shift_left },
    { "shiftRight", "a,n", b_shift_right },
    { "popCount", "a", b_pop_count },
    { "wrapAdd", "a,b", b_wrap_add },
    { "wrapSub", "a,b", b_wrap_sub },
    { "wrapMul", "a,b", b_wrap_mul },
  };
  ANT_NAMESPACE("Bits", members);
}

/* ---------------------------------------------------------------- Math */

static bool need_f64(AntValue v) {
  return ant_need_kind(v, K_FLOAT64, "a float64");
}

static AntValue m_abs(AntFunc *self, int argc, AntValue *argv) {
  AntValue x = argv[0];
  NUM(x);
  if (x.kind == K_FLOAT64) return ant_float(fabs(x.as.f));
  if (x.kind == K_FLOAT32) { x.as.f32 = fabsf(x.as.f32); return x; }
  if (x.kind >= K_UINT8 || x.as.i >= 0) return x;
  return ant_neg(x);
}

bool ant_need_number(AntValue v) {
  return ant_is_number(v.kind) || ant_wrong_kind("a number");
}

bool ant_need_ordered(AntValue v) {
  return ant_is_number(v.kind) || v.kind == K_STRING || ant_wrong_kind("a number or a string");
}

static AntValue m_min(AntFunc *self, int argc, AntValue *argv) {
  ORD(argv[0]);
  ORD(argv[1]);
  bool less = ant_less(argv[1], argv[0]);
  UNWOUND();
  return less ? argv[1] : argv[0];
}

static AntValue m_max(AntFunc *self, int argc, AntValue *argv) {
  ORD(argv[0]);
  ORD(argv[1]);
  bool less = ant_less(argv[0], argv[1]);
  UNWOUND();
  return less ? argv[1] : argv[0];
}

static AntValue m_pow(AntFunc *self, int argc, AntValue *argv) {
  if (!need_f64(argv[0]) || !need_f64(argv[1])) return ANT_VOID;
  return ant_float(pow(argv[0].as.f, argv[1].as.f));
}

#define MATH1(NAME, FN) \
  static AntValue NAME(AntFunc *self, int argc, AntValue *argv) { \
    if (!need_f64(argv[0])) return ANT_VOID; \
    return ant_float(FN(argv[0].as.f)); \
  }
MATH1(m_sqrt, sqrt)
MATH1(m_floor, floor)
MATH1(m_ceil, ceil)
MATH1(m_round, round)     /* half away from zero */

void ant_ns_math(void) {
  static const AntMember members[] = {
    { "abs", "x", m_abs },
    { "min", "a,b", m_min },
    { "max", "a,b", m_max },
    { "pow", "x,y", m_pow },
    { "sqrt", "x", m_sqrt },
    { "floor", "x", m_floor },
    { "ceil", "x", m_ceil },
    { "round", "x", m_round },
    { "PI", NULL, NULL, { K_FLOAT64, { .f = M_PI } } },
    { "E", NULL, NULL, { K_FLOAT64, { .f = M_E } } },
  };
  ANT_NAMESPACE("Math", members);
}
