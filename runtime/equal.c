/* Structural equality (section 12) and the hash that agrees with it. */
#include <string.h>
#include "ant.h"

/* The pair stack: a pair of containers already under comparison counts as
 * equal, so cyclic values compare in finite time. */
static bool pair_active(void *a, void *b) {
  for (int i = 0; i < ant_g->npairs; i++) {
    if (ant_g->pairs[i].a == a && ant_g->pairs[i].b == b) return true;
  }
  return false;
}

static void pair_push(void *a, void *b) {
  if (ant_g->npairs == ant_g->cappairs) {
    ant_g->cappairs = ant_g->cappairs ? ant_g->cappairs * 2 : 16;
    ant_g->pairs = ant_realloc(ant_g->pairs, sizeof(AntPair) * (size_t)ant_g->cappairs);
  }
  ant_g->pairs[ant_g->npairs].a = a;
  ant_g->pairs[ant_g->npairs].b = b;
  ant_g->npairs++;
}

static bool equal_in(AntValue a, AntValue b);

static bool record_equal(AntRecord *x, AntRecord *y) {
  if (x->tag != y->tag || x->shape->n != y->shape->n) return false;
  if (x->shape == y->shape) {
    for (int i = 0; i < x->shape->n; i++) if (!equal_in(x->fields[i], y->fields[i])) return false;
    return true;
  }
  for (int i = 0; i < x->shape->n; i++) {
    int j = ant_shape_find(y->shape, x->shape->names[i]);
    if (j < 0 || !equal_in(x->fields[i], y->fields[j])) return false;
  }
  return true;
}

static bool map_equal(AntValue a, AntValue b) {
  if (ant_map_size(a) != ant_map_size(b)) return false;
  int64_t n = ant_map_slots(a);
  for (int64_t i = 0; i < n; i++) {
    AntValue k, v;
    if (!ant_map_entry(a, i, &k, &v)) continue;
    AntValue w;
    if (!ant_map_lookup(b, k, &w) || !equal_in(v, w)) return false;
  }
  return true;
}

static bool equal_in(AntValue a, AntValue b) {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case K_INT8: case K_INT16: case K_INT32: case K_INT64: return a.as.i == b.as.i;
    case K_UINT8: case K_UINT16: case K_UINT32: case K_UINT64: return a.as.u == b.as.u;
    case K_FLOAT32: return a.as.f32 == b.as.f32;
    case K_FLOAT64: return a.as.f == b.as.f;
    case K_BOOL: return a.as.b == b.as.b;
    case K_STRING: return ant_str_eq(a.as.p, b.as.p);
    case K_NULL: case K_VOID: return true;
    case K_FUNC: case K_TAG: case K_SYNC: return a.as.p == b.as.p;
    default: break;
  }
  if (pair_active(a.as.p, b.as.p)) return true;
  pair_push(a.as.p, b.as.p);
  bool r;
  if (a.kind == K_ARRAY) {
    AntArray *x = a.as.p, *y = b.as.p;
    r = x->len == y->len;
    for (int64_t i = 0; r && i < x->len && i < y->len; i++) r = equal_in(x->data[i], y->data[i]);
  } else if (a.kind == K_RECORD) {
    r = record_equal(a.as.p, b.as.p);
  } else {
    r = map_equal(a, b);
  }
  ant_g->npairs--;
  return r;
}

bool ant_equal(AntValue a, AntValue b) {
  return equal_in(a, b);
}

/* ---------------------------------------------------------------- hashing */

static uint32_t mix(uint32_t h, uint32_t x) {
  h ^= x + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h;
}

static uint32_t hash_bits(uint64_t bits) {
  bits ^= bits >> 33;
  bits *= 0xff51afd7ed558ccdULL;
  bits ^= bits >> 33;
  return (uint32_t)bits;
}

static uint32_t hash_in(AntValue v, int depth) {
  switch (v.kind) {
    case K_INT8: case K_INT16: case K_INT32: case K_INT64:
    case K_UINT8: case K_UINT16: case K_UINT32: case K_UINT64:
      return mix(v.kind, hash_bits(v.as.u));
    case K_FLOAT32: case K_FLOAT64: {
      double d = v.kind == K_FLOAT32 ? v.as.f32 : v.as.f;
      if (d != d) return mix(v.kind, 0x7fc00000u);
      if (d == 0.0) return mix(v.kind, 0);
      uint64_t bits; memcpy(&bits, &d, 8);
      return mix(v.kind, hash_bits(bits));
    }
    case K_BOOL: return mix(v.kind, v.as.b ? 1 : 2);
    case K_STRING: return ant_str_hash(v.as.p);
    case K_NULL: return 0x6e756c6cu;
    case K_VOID: return 0x766f6964u;
    case K_FUNC: case K_TAG: case K_SYNC: return mix(v.kind, hash_bits((uint64_t)(uintptr_t)v.as.p));
    default: break;
  }
  if (depth > 8) return 0x64656570u;
  if (v.kind == K_ARRAY) {
    AntArray *a = v.as.p;
    uint32_t h = 0x61727261u;
    for (int64_t i = 0; i < a->len; i++) h = mix(h, hash_in(a->data[i], depth + 1));
    return h;
  }
  if (v.kind == K_RECORD) {
    AntRecord *r = v.as.p;
    uint32_t h = hash_bits((uint64_t)(uintptr_t)r->tag);
    for (int i = 0; i < r->shape->n; i++) {
      h ^= (r->shape->names[i] + 1) * 0x9e3779b9u * (hash_in(r->fields[i], depth + 1) | 1);   /* order-insensitive */
    }
    return h;
  }
  uint32_t h = 0x6d6170u;
  int64_t n = ant_map_slots(v);
  for (int64_t i = 0; i < n; i++) {
    AntValue k, val;
    if (!ant_map_entry(v, i, &k, &val)) continue;
    h ^= mix(hash_in(k, depth + 1), hash_in(val, depth + 1));
  }
  return h;
}

uint32_t ant_hash(AntValue v) {
  return hash_in(v, 0);
}
