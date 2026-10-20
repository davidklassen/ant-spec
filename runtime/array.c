/* Arrays, indexing, and the Array namespace (section 11). */
#include <string.h>
#include "ant.h"

AntValue ant_array_new(int64_t cap) {
  AntArray *a = ant_alloc(sizeof(AntArray));
  a->len = 0;
  a->cap = cap > 0 ? cap : 4;
  a->data = ant_alloc(sizeof(AntValue) * (size_t)a->cap);
  return ant_ptr(K_ARRAY, a);
}

static void grow(AntArray *a, int64_t need) {
  if (need <= a->cap) return;
  int64_t cap = a->cap * 2;
  while (cap < need) cap *= 2;
  AntValue *data = ant_alloc(sizeof(AntValue) * (size_t)cap);
  memcpy(data, a->data, sizeof(AntValue) * (size_t)a->len);
  a->data = data;
  a->cap = cap;
}

void ant_array_push(AntValue av, AntValue v) {
  AntArray *a = av.as.p;
  grow(a, a->len + 1);
  a->data[a->len++] = v;
}

/* An array-literal spread: every element of a value that must be an array. */
AntValue ant_array_extend(AntValue av, AntValue v) {
  if (v.kind != K_ARRAY) {
    ant_panic("not an array");
    return ANT_VOID;
  }
  AntArray *a = av.as.p, *b = v.as.p;
  grow(a, a->len + b->len);
  memcpy(a->data + a->len, b->data, sizeof(AntValue) * (size_t)b->len);
  a->len += b->len;
  return ANT_VOID;
}

AntArray *ant_need_array(AntValue v) {
  if (v.kind != K_ARRAY) {
    ant_panic("wrong kind: expected an array");
    return NULL;
  }
  return v.as.p;
}

int64_t ant_need_int(AntValue v) {
  if (v.kind != K_INT64) {
    ant_panic("wrong kind: expected an int");
    return 0;
  }
  return v.as.i;
}

/* The one definition of a valid array index for `a[i]`; -1 after a panic. */
static int64_t index_of(AntArray *a, AntValue i) {
  if (i.kind != K_INT64) {
    ant_panic("index not an int");
    return -1;
  }
  if (i.as.i < 0 || i.as.i >= a->len) {
    ant_panic("index out of range");
    return -1;
  }
  return i.as.i;
}

AntValue ant_index_get(AntValue av, AntValue i) {
  if (av.kind == K_ARRAY) {
    int64_t k = index_of(av.as.p, i);
    if (k < 0) return ANT_VOID;
    return ((AntArray *)av.as.p)->data[k];
  }
  if (av.kind == K_MAP) return ant_map_get(av, i);
  ant_panic("not indexable");
  return ANT_VOID;
}

AntValue ant_index_set(AntValue av, AntValue i, AntValue v) {
  if (av.kind == K_ARRAY) {
    int64_t k = index_of(av.as.p, i);
    if (k < 0) return ANT_VOID;
    ((AntArray *)av.as.p)->data[k] = v;
    return ANT_VOID;
  }
  if (av.kind == K_MAP) { ant_map_set(av, i, v); return ANT_VOID; }
  ant_panic("not indexable");
  return ANT_VOID;
}

/* The target check of `a[i] = e`, made before the right side is evaluated. */
AntValue ant_index_check(AntValue av, AntValue i) {
  if (av.kind == K_ARRAY) { index_of(av.as.p, i); return ANT_VOID; }
  if (av.kind == K_MAP) return ANT_VOID;
  ant_panic("not indexable");
  return ANT_VOID;
}

/* ---------------------------------------------------------------- natives */

static AntValue a_length(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  return ant_int(xs->len);
}

static AntValue a_is_empty(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  return ant_bool(xs->len == 0);
}

static AntValue a_get(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  INT(argv[1], i);
  if (i < 0 || i >= xs->len) return ANT_NULL;
  return xs->data[i];
}

static AntValue a_push(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  ant_array_push(argv[0], argv[1]);
  return ANT_VOID;
}

static AntValue a_pop(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  if (xs->len == 0) return ANT_NULL;
  return xs->data[--xs->len];
}

static AntValue a_slice(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  INT(argv[1], from);
  INT(argv[2], to);
  if (from < 0 || from > to || to > xs->len) { ant_panic("index out of range"); return ANT_VOID; }
  AntValue out = ant_array_new(to - from);
  AntArray *o = out.as.p;
  memcpy(o->data, xs->data + from, sizeof(AntValue) * (size_t)(to - from));
  o->len = to - from;
  return out;
}

static AntValue a_concat(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  ARR(argv[1], ys);
  AntValue out = ant_array_new(xs->len + ys->len);
  AntArray *o = out.as.p;
  memcpy(o->data, xs->data, sizeof(AntValue) * (size_t)xs->len);
  memcpy(o->data + xs->len, ys->data, sizeof(AntValue) * (size_t)ys->len);
  o->len = xs->len + ys->len;
  return out;
}

static AntValue a_map(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  FUNC(argv[1]);
  AntValue out = ant_array_new(xs->len);
  for (int64_t i = 0; i < xs->len; i++) {
    AntValue x = xs->data[i];
    AntValue r = ant_call(argv[1], 1, &x);
    UNWOUND();
    ant_array_push(out, r);
  }
  return out;
}

static AntValue a_filter(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  FUNC(argv[1]);
  AntValue out = ant_array_new(4);
  for (int64_t i = 0; i < xs->len; i++) {
    AntValue x = xs->data[i];
    bool keep = ant_call_truth(argv[1], x);
    UNWOUND();
    if (keep) ant_array_push(out, x);
  }
  return out;
}

static AntValue a_reduce(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  FUNC(argv[2]);
  AntValue acc = argv[1];
  for (int64_t i = 0; i < xs->len; i++) {
    AntValue args[2] = { acc, xs->data[i] };
    acc = ant_call(argv[2], 2, args);
    UNWOUND();
  }
  return acc;
}

static AntValue a_find(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  FUNC(argv[1]);
  for (int64_t i = 0; i < xs->len; i++) {
    AntValue x = xs->data[i];
    bool hit = ant_call_truth(argv[1], x);
    UNWOUND();
    if (hit) return x;
  }
  return ANT_NULL;
}

static AntValue a_some(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  FUNC(argv[1]);
  for (int64_t i = 0; i < xs->len; i++) {
    bool hit = ant_call_truth(argv[1], xs->data[i]);
    UNWOUND();
    if (hit) return ANT_TRUE;
  }
  return ANT_FALSE;
}

static AntValue a_every(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  FUNC(argv[1]);
  for (int64_t i = 0; i < xs->len; i++) {
    bool hit = ant_call_truth(argv[1], xs->data[i]);
    UNWOUND();
    if (!hit) return ANT_FALSE;
  }
  return ANT_TRUE;
}

static AntValue a_index_of(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  for (int64_t i = 0; i < xs->len; i++) if (ant_equal(xs->data[i], argv[1])) return ant_int(i);
  return ANT_NULL;
}

static AntValue a_contains(AntFunc *self, int argc, AntValue *argv) {
  AntValue i = a_index_of(self, argc, argv);
  if (ant_g->unwind.mode) return ANT_VOID;
  return ant_bool(i.kind != K_NULL);
}

/* ---------------------------------------------------------------- sorting */

/* A stable merge sort of n values with a less-than that may panic: `less`
 * answers through *out and returns false once the unwind flag is set. */
typedef bool (*Less)(void *ctx, AntValue a, AntValue b, bool *out);

static bool merge_sort(AntValue *v, AntValue *tmp, int64_t n, Less less, void *ctx) {
  if (n < 2) return true;
  int64_t mid = n / 2;
  if (!merge_sort(v, tmp, mid, less, ctx)) return false;
  if (!merge_sort(v + mid, tmp, n - mid, less, ctx)) return false;
  int64_t i = 0, j = mid, k = 0;
  while (i < mid && j < n) {
    bool right_first;
    if (!less(ctx, v[j], v[i], &right_first)) return false;
    tmp[k++] = right_first ? v[j++] : v[i++];
  }
  while (i < mid) tmp[k++] = v[i++];
  while (j < n) tmp[k++] = v[j++];
  memcpy(v, tmp, sizeof(AntValue) * (size_t)n);
  return true;
}

static AntValue sorted_copy(AntArray *xs, Less less, void *ctx) {
  AntValue out = ant_array_new(xs->len);
  AntArray *o = out.as.p;
  memcpy(o->data, xs->data, sizeof(AntValue) * (size_t)xs->len);
  o->len = xs->len;
  AntValue *tmp = ant_alloc(sizeof(AntValue) * (size_t)(xs->len ? xs->len : 1));
  if (!merge_sort(o->data, tmp, o->len, less, ctx)) return ANT_VOID;
  return out;
}

static bool less_values(void *ctx, AntValue a, AntValue b, bool *out) {
  *out = ant_less(a, b);
  return !ant_g->unwind.mode;
}

static AntValue a_sort(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  for (int64_t i = 0; i < xs->len; i++) if (!ant_need_ordered(xs->data[i])) return ANT_VOID;
  return sorted_copy(xs, less_values, NULL);
}

/* sortBy: the keys are computed once, in order; indices are sorted by key. */
typedef struct { AntValue *keys; } ByKeys;

static bool less_by_key(void *ctx, AntValue a, AntValue b, bool *out) {
  AntValue *keys = ((ByKeys *)ctx)->keys;
  *out = ant_less(keys[a.as.i], keys[b.as.i]);
  return !ant_g->unwind.mode;
}

static AntValue a_sort_by(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  FUNC(argv[1]);
  int64_t n = xs->len;
  AntValue *keys = ant_alloc(sizeof(AntValue) * (size_t)(n ? n : 1));
  for (int64_t i = 0; i < n; i++) {
    AntValue x = xs->data[i];
    keys[i] = ant_call(argv[1], 1, &x);
    UNWOUND();
    if (!ant_need_ordered(keys[i])) return ANT_VOID;
  }
  AntValue idx = ant_array_new(n);
  for (int64_t i = 0; i < n; i++) ant_array_push(idx, ant_int(i));
  ByKeys ctx = { keys };
  AntValue order = sorted_copy(idx.as.p, less_by_key, &ctx);
  UNWOUND();
  AntArray *o = order.as.p;
  for (int64_t i = 0; i < n; i++) o->data[i] = xs->data[o->data[i].as.i];
  return order;
}

static bool less_with(void *ctx, AntValue a, AntValue b, bool *out) {
  AntValue args[2] = { a, b };
  AntValue r = ant_call(*(AntValue *)ctx, 2, args);
  if (ant_g->unwind.mode) return false;
  int64_t c = ant_need_int(r);
  if (ant_g->unwind.mode) return false;
  *out = c < 0;
  return true;
}

static AntValue a_sort_with(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  FUNC(argv[1]);
  return sorted_copy(xs, less_with, &argv[1]);
}

static AntValue a_reverse(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  AntValue out = ant_array_new(xs->len);
  for (int64_t i = xs->len - 1; i >= 0; i--) ant_array_push(out, xs->data[i]);
  return out;
}

static AntValue a_entries(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  AntValue out = ant_array_new(xs->len);
  for (int64_t i = 0; i < xs->len; i++) ant_array_push(out, ant_indexed(i, xs->data[i]));
  return out;
}

static AntValue a_range(AntFunc *self, int argc, AntValue *argv) {
  INT(argv[0], from);
  INT(argv[1], to);
  AntValue out = ant_array_new(to > from ? to - from : 0);
  for (int64_t i = from; i < to; i++) ant_array_push(out, ant_int(i));
  return out;
}

static AntValue a_filled(AntFunc *self, int argc, AntValue *argv) {
  INT(argv[0], n);
  if (n < 0) { ant_panic("negative count"); return ANT_VOID; }
  AntValue out = ant_array_new(n);
  for (int64_t i = 0; i < n; i++) ant_array_push(out, argv[1]);
  return out;
}

static AntValue a_flatten(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  for (int64_t i = 0; i < xs->len; i++) { ARR(xs->data[i], inner); (void)inner; }
  AntValue out = ant_array_new(4);
  for (int64_t i = 0; i < xs->len; i++) ant_array_extend(out, xs->data[i]);
  return out;
}

static AntValue a_sum(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  if (xs->len == 0) return ANT_NULL;
  for (int64_t i = 0; i < xs->len; i++) {
    if (!ant_is_number(xs->data[i].kind)) { ant_panic("wrong kind: expected a number"); return ANT_VOID; }
  }
  AntValue acc = xs->data[0];
  for (int64_t i = 1; i < xs->len; i++) {
    acc = ant_add(acc, xs->data[i]);
    UNWOUND();
  }
  return acc;
}

static AntValue a_same(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  ARR(argv[1], ys);
  return ant_bool(xs == ys);
}

AntValue ant_ns_array(void) {
  static const AntMember members[] = {
    { "length", "xs", a_length },
    { "isEmpty", "xs", a_is_empty },
    { "get", "xs,i", a_get },
    { "push", "xs,v", a_push },
    { "pop", "xs", a_pop },
    { "slice", "xs,from,to", a_slice },
    { "concat", "xs,ys", a_concat },
    { "map", "xs,f", a_map },
    { "filter", "xs,f", a_filter },
    { "reduce", "xs,init,f", a_reduce },
    { "find", "xs,f", a_find },
    { "some", "xs,f", a_some },
    { "every", "xs,f", a_every },
    { "contains", "xs,v", a_contains },
    { "indexOf", "xs,v", a_index_of },
    { "sort", "xs", a_sort },
    { "sortBy", "xs,key", a_sort_by },
    { "sortWith", "xs,cmp", a_sort_with },
    { "reverse", "xs", a_reverse },
    { "entries", "xs", a_entries },
    { "range", "from,to", a_range },
    { "filled", "n,v", a_filled },
    { "flatten", "xs", a_flatten },
    { "sum", "xs", a_sum },
    { "same", "xs,ys", a_same },
  };
  return ANT_NAMESPACE("Array", members);
}
