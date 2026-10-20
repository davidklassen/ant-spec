/* Tags, records, field access with inline caches, `is`, and module records
 * (sections 4, 10, 12). */
#include <string.h>
#include "ant.h"

AntValue ant_tag_new(const char *name) {
  AntTag *t = ant_alloc(sizeof(AntTag));
  t->name = name;
  return ant_ptr(K_TAG, t);
}

AntTag *ant_need_tag(AntValue v) {
  if (v.kind != K_TAG) {
    ant_panic("not a tag");
    return NULL;
  }
  return v.as.p;
}

AntValue ant_check_tag(AntValue v) {
  if (v.kind != K_TAG) ant_panic("not a tag");
  return v;
}

/* The record behind v, or NULL after a `not a record` panic. */
static AntRecord *need_record(AntValue v) {
  if (v.kind != K_RECORD) {
    ant_panic("not a record");
    return NULL;
  }
  return v.as.p;
}

AntValue ant_record_make(AntTag *tag, const AntShape *shape, AntValue *values) {
  AntRecord *r = ant_alloc(sizeof(AntRecord) + sizeof(AntValue) * (size_t)shape->n);
  r->tag = tag;
  r->shape = shape;
  if (shape->n) memcpy(r->fields, values, sizeof(AntValue) * (size_t)shape->n);
  return ant_ptr(K_RECORD, r);
}

/* A literal without spread: the head must be a tag, the shape is static. */
AntValue ant_record_new(AntValue tag, const AntShape *shape, AntValue *values) {
  AntTag *t = ant_need_tag(tag);
  if (!t) return ANT_VOID;
  return ant_record_make(t, shape, values);
}

/* The field s of a record under construction: appended, or replaced in place. */
static void put(Sym *syms, AntValue *vals, int *count, Sym s, AntValue v) {
  int j = 0;
  while (j < *count && syms[j] != s) j++;
  if (j == *count) syms[(*count)++] = s;
  vals[j] = v;
}

/* A spread operand, copied in its place: a shallow copy of the record, so a
 * later entry whose evaluation mutates the source cannot leak into the
 * literal; anything but a record panics `not a record` (section 12). */
AntValue ant_record_snapshot(AntValue v) {
  AntRecord *r = need_record(v);
  if (!r) return ANT_VOID;
  return ant_record_make(r->tag, r->shape, r->fields);
}

/* A literal with a spread: fields are gathered in order, a later entry
 * replacing an earlier one in place, and the shape is interned. The emitter
 * lands each spread operand through ant_record_snapshot, which is where a
 * non-record panics in evaluation order; the `not a record` check in the
 * pre-pass below is a second line of defence, not the primary one. */
AntValue ant_record_build(AntValue tag, int n, const Sym *names, AntValue *values, const bool *isSpread) {
  AntTag *t = ant_need_tag(tag);
  if (!t) return ANT_VOID;
  int cap = n;
  for (int i = 0; i < n; i++) {
    if (isSpread[i]) {
      AntRecord *src = need_record(values[i]);
      if (!src) return ANT_VOID;
      cap += src->shape->n;
    }
  }
  Sym *syms = ant_alloc_atomic(sizeof(Sym) * (size_t)(cap ? cap : 1));
  AntValue *vals = ant_alloc(sizeof(AntValue) * (size_t)(cap ? cap : 1));
  int count = 0;
  for (int i = 0; i < n; i++) {
    if (isSpread[i]) {
      AntRecord *src = values[i].as.p;
      for (int k = 0; k < src->shape->n; k++) put(syms, vals, &count, src->shape->names[k], src->fields[k]);
    } else {
      put(syms, vals, &count, names[i], values[i]);
    }
  }
  return ant_record_make(t, ant_shape_intern(count, syms), vals);
}

int ant_shape_find(const AntShape *shape, Sym s) {
  for (int i = 0; i < shape->n; i++) if (shape->names[i] == s) return i;
  return -1;
}

/* The field index of s in r, through the site's cache; -1 after a panic. */
static int field_index(AntValue v, Sym s, AntFieldCache *c) {
  AntRecord *r = need_record(v);
  if (!r) return -1;
  if (c && c->shape == r->shape) return c->idx;
  int i = ant_shape_find(r->shape, s);
  if (i < 0) {
    ant_panicf("no field: %s", ant_sym_name(s));
    return -1;
  }
  if (c) { c->shape = r->shape; c->idx = i; }
  return i;
}

AntValue ant_field_get(AntValue v, Sym s, AntFieldCache *c) {
  int i = field_index(v, s, c);
  if (i < 0) return ANT_VOID;
  AntValue f = ((AntRecord *)v.as.p)->fields[i];
  if (f.kind == K_UNINIT) {
    ant_panicf("not initialized: %s", ant_sym_name(s));
    return ANT_VOID;
  }
  return f;
}

AntValue ant_field_set(AntValue v, Sym s, AntValue val, AntFieldCache *c) {
  int i = field_index(v, s, c);
  if (i < 0) return ANT_VOID;
  ((AntRecord *)v.as.p)->fields[i] = val;
  return ANT_VOID;
}

/* The target check of `a.b = e`, made before the right side is evaluated. */
AntValue ant_field_check(AntValue v, Sym s, AntFieldCache *c) {
  field_index(v, s, c);
  return ANT_VOID;
}

AntValue ant_is_tag(AntValue v, AntValue tag) {
  AntTag *t = ant_need_tag(tag);
  if (!t) return ANT_VOID;
  return ant_bool(v.kind == K_RECORD && ((AntRecord *)v.as.p)->tag == t);
}

/* ---------------------------------------------------------------- modules */

static AntTag module_tag = { "Module" };

AntTag *ant_module_tag(void) {
  return &module_tag;
}

AntValue ant_module_new(const AntShape *shape) {
  AntRecord *r = ant_alloc(sizeof(AntRecord) + sizeof(AntValue) * (size_t)shape->n);
  r->tag = &module_tag;
  r->shape = shape;
  for (int i = 0; i < shape->n; i++) r->fields[i] = ANT_UNINIT;
  return ant_ptr(K_RECORD, r);
}

AntValue ant_module_get(AntValue m, int idx) {
  AntRecord *r = m.as.p;
  AntValue v = r->fields[idx];
  if (v.kind == K_UNINIT) {
    ant_panicf("not initialized: %s", ant_sym_name(r->shape->names[idx]));
    return ANT_VOID;
  }
  return v;
}

void ant_module_set(AntValue m, int idx, AntValue v) {
  ((AntRecord *)m.as.p)->fields[idx] = v;
}
