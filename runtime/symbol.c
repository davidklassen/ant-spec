/* Interned names (field and parameter symbols) and interned shapes.
 *
 * The generated program numbers its symbols at compile time and seeds the
 * table with ant_symbols_init before anything else runs; ant_sym then
 * returns the seeded id for a known name and appends an unknown one (the
 * runtime's own names, interned at prelude init). */
#include <string.h>
#include "ant.h"

typedef struct { const char *name; uint32_t hash; } SymEntry;

static SymEntry *syms;        /* by id */
static int nsyms, capsyms;
static int32_t *symindex;        /* open addressing over ids */
static int icap;

static uint32_t hash_cstr(const char *s) {
  return ant_fnv1a(s, (int64_t)strlen(s));
}

static void rebuild_index(int cap) {
  icap = cap;
  symindex = ant_alloc_atomic(sizeof(int32_t) * (size_t)cap);
  memset(symindex, -1, sizeof(int32_t) * (size_t)cap);
  for (int id = 0; id < nsyms; id++) {
    uint32_t i = syms[id].hash & (uint32_t)(cap - 1);
    while (symindex[i] != -1) i = (i + 1) & (uint32_t)(cap - 1);
    symindex[i] = id;
  }
}

static Sym add(const char *name, uint32_t h) {
  size_t n = strlen(name);
  char *copy = ant_alloc_atomic(n + 1);
  memcpy(copy, name, n + 1);
  ANT_PUSH(syms, nsyms, capsyms, 256, ((SymEntry){ copy, h }));
  Sym id = (Sym)(nsyms - 1);
  if (nsyms * 2 >= icap) {
    rebuild_index(icap ? icap * 2 : 512);
  } else {
    uint32_t i = h & (uint32_t)(icap - 1);
    while (symindex[i] != -1) i = (i + 1) & (uint32_t)(icap - 1);
    symindex[i] = (int32_t)id;
  }
  return id;
}

void ant_symbols_init(int n, const char *const *names) {
  for (int i = 0; i < n; i++) add(names[i], hash_cstr(names[i]));
}

Sym ant_sym(const char *name) {
  uint32_t h = hash_cstr(name);
  if (icap) {
    uint32_t i = h & (uint32_t)(icap - 1);
    while (symindex[i] != -1) {
      SymEntry *e = &syms[symindex[i]];
      if (e->hash == h && strcmp(e->name, name) == 0) return (Sym)symindex[i];
      i = (i + 1) & (uint32_t)(icap - 1);
    }
  }
  return add(name, h);
}

const char *ant_sym_name(Sym s) {
  return syms[s].name;
}

/* ---------------------------------------------------------------- shapes */

typedef struct ShapeNode { const AntShape *shape; uint32_t hash; struct ShapeNode *next; } ShapeNode;
static ShapeNode *shapes[1024];

static uint32_t hash_syms(int n, const Sym *names) {
  uint32_t h = 2166136261u ^ (uint32_t)n;
  for (int i = 0; i < n; i++) h = (h ^ names[i]) * 16777619u;
  return h;
}

/* The one shape for a sequence of names: the inline caches and the
 * equality fast path compare shapes by address. */
const AntShape *ant_shape_intern(int n, const Sym *names) {
  uint32_t h = hash_syms(n, names);
  ShapeNode *node;
  for (node = shapes[h & 1023]; node; node = node->next) {
    if (node->hash == h && node->shape->n == n &&
        memcmp(node->shape->names, names, sizeof(Sym) * (size_t)n) == 0)
      return node->shape;
  }
  Sym *copy = ant_alloc_atomic(sizeof(Sym) * (size_t)(n ? n : 1));
  memcpy(copy, names, sizeof(Sym) * (size_t)n);
  AntShape *shape = ant_alloc(sizeof(AntShape));
  shape->n = n;
  shape->names = copy;
  node = ant_alloc(sizeof(ShapeNode));
  node->shape = shape;
  node->hash = h;
  node->next = shapes[h & 1023];
  shapes[h & 1023] = node;
  return shape;
}
