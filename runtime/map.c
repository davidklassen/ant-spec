/* Maps keyed by `==`: a compact dict (insertion-ordered entries plus an
 * open-addressed index), and the Map namespace (section 11). */
#include <string.h>
#include "ant.h"

typedef struct { uint32_t hash; bool alive; AntValue key, value; } Entry;

struct AntMap {
  int64_t n, cap, live;    /* entries used (dead ones included), their capacity, live count */
  Entry *entries;
  int64_t icap;            /* index slots, a power of two */
  int32_t *index;          /* entry index, -1 empty, -2 deleted */
};

#define EMPTY (-1)
#define DELETED (-2)

static void build_index(AntMap *m, int64_t icap) {
  m->icap = icap;
  m->index = ant_alloc_atomic_big(sizeof(int32_t) * (size_t)icap);
  memset(m->index, -1, sizeof(int32_t) * (size_t)icap);
  for (int64_t i = 0; i < m->n; i++) {
    if (!m->entries[i].alive) continue;
    int64_t slot = m->entries[i].hash & (icap - 1);
    while (m->index[slot] != EMPTY) slot = (slot + 1) & (icap - 1);
    m->index[slot] = (int32_t)i;
  }
}

/* The tables are allocated at the first insertion: `Map{}` costs one
 * block, and most empty maps (scratch sets, scopes) stay small or empty. */
AntValue ant_map_new(void) {
  AntMap *m = ant_alloc(sizeof(AntMap));
  m->n = m->live = m->cap = m->icap = 0;
  m->entries = NULL;
  m->index = NULL;
  return ant_ptr(K_MAP, m);
}

AntMap *ant_need_map(AntValue v) {
  return ant_need_kind(v, K_MAP, "a map") ? v.as.p : NULL;
}

/* The index slot holding the entry for key, or -1. */
static int64_t find(AntMap *m, AntValue key, uint32_t h) {
  if (m->icap == 0) return -1;
  int64_t slot = h & (m->icap - 1);
  for (;;) {
    int32_t idx = m->index[slot];
    if (idx == EMPTY) return -1;
    if (idx >= 0) {
      Entry *e = &m->entries[idx];
      if (e->hash == h && e->alive && ant_equal(e->key, key)) return slot;
    }
    slot = (slot + 1) & (m->icap - 1);
  }
}

/* Drops dead entries and rebuilds the index so that it is under a third full. */
static void compact(AntMap *m) {
  int64_t k = 0;
  for (int64_t i = 0; i < m->n; i++) {
    if (m->entries[i].alive) m->entries[k++] = m->entries[i];
  }
  m->n = k;
  int64_t icap = 16;
  while (icap < 3 * (m->n + 1)) icap *= 2;
  build_index(m, icap);
}

void ant_map_set(AntValue mv, AntValue key, AntValue value) {
  AntMap *m = mv.as.p;
  uint32_t h = ant_hash(key);
  int64_t slot = find(m, key, h);
  if (slot >= 0) {
    m->entries[m->index[slot]].value = value;
    return;
  }
  if ((m->n + 1) * 3 >= m->icap * 2) compact(m);
  if (m->n == m->cap) {
    m->cap = m->cap ? m->cap * 2 : 8;
    Entry *entries = ant_alloc_big(sizeof(Entry) * (size_t)m->cap);
    if (m->n) memcpy(entries, m->entries, sizeof(Entry) * (size_t)m->n);
    m->entries = entries;
  }
  Entry *e = &m->entries[m->n];
  e->hash = h;
  e->alive = true;
  e->key = key;
  e->value = value;
  slot = h & (m->icap - 1);
  while (m->index[slot] >= 0) slot = (slot + 1) & (m->icap - 1);
  m->index[slot] = (int32_t)m->n;
  m->n++;
  m->live++;
}

bool ant_map_lookup(AntValue mv, AntValue key, AntValue *out) {
  AntMap *m = mv.as.p;
  int64_t slot = find(m, key, ant_hash(key));
  if (slot < 0) return false;
  *out = m->entries[m->index[slot]].value;
  return true;
}

AntValue ant_map_get(AntValue mv, AntValue key) {
  AntValue v;
  return ant_map_lookup(mv, key, &v) ? v : ANT_NULL;
}

static bool map_remove(AntValue mv, AntValue key, AntValue *out) {
  AntMap *m = mv.as.p;
  int64_t slot = find(m, key, ant_hash(key));
  if (slot < 0) return false;
  Entry *e = &m->entries[m->index[slot]];
  *out = e->value;
  e->alive = false;
  e->key = ANT_NULL;
  e->value = ANT_NULL;
  m->index[slot] = DELETED;
  m->live--;
  return true;
}

int64_t ant_map_size(AntValue mv) {
  return ((AntMap *)mv.as.p)->live;
}

int64_t ant_map_slots(AntValue mv) {
  return ((AntMap *)mv.as.p)->n;
}

bool ant_map_entry(AntValue mv, int64_t i, AntValue *k, AntValue *v) {
  AntMap *m = mv.as.p;
  if (i >= m->n || !m->entries[i].alive) return false;
  *k = m->entries[i].key;
  *v = m->entries[i].value;
  return true;
}

/* ---------------------------------------------------------------- natives */

static AntValue m_size(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  return ant_int(m->live);
}

static AntValue m_is_empty(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  return ant_bool(m->live == 0);
}

static AntValue m_has(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  AntValue v;
  return ant_bool(ant_map_lookup(argv[0], argv[1], &v));
}

static AntValue m_get(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  return ant_map_get(argv[0], argv[1]);
}

static AntValue m_set(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  ant_map_set(argv[0], argv[1], argv[2]);
  return ANT_VOID;
}

static AntValue m_remove(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  AntValue out;
  if (!map_remove(argv[0], argv[1], &out)) return ANT_NULL;
  return out;
}

/* Keys, values, or entries as a fresh array: a snapshot in entry order. */
static AntValue collect(AntValue mv, int what) {
  AntMap *m = mv.as.p;
  AntValue out = ant_array_new(m->live);
  for (int64_t i = 0; i < m->n; i++) {
    Entry *e = &m->entries[i];
    if (!e->alive) continue;
    ant_array_push(out, what == 0 ? e->key : what == 1 ? e->value : ant_entry(e->key, e->value));
  }
  return out;
}

static AntValue m_keys(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  return collect(argv[0], 0);
}

static AntValue m_values(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  return collect(argv[0], 1);
}

static AntValue m_entries(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  return collect(argv[0], 2);
}

static AntValue m_from_entries(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  AntValue out = ant_map_new();
  for (int64_t i = 0; i < xs->len; i++) {
    AntValue e = xs->data[i];
    if (e.kind != K_RECORD) { ant_wrong_kind("a record"); return ANT_VOID; }
    AntValue k = ant_field_get(e, S_key, NULL);
    UNWOUND();
    AntValue v = ant_field_get(e, S_value, NULL);
    UNWOUND();
    ant_map_set(out, k, v);
  }
  return out;
}

static AntValue m_map(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  FUNC(argv[1]);
  AntValue snapshot = collect(argv[0], 2);
  AntArray *es = snapshot.as.p;
  AntValue out = ant_map_new();
  for (int64_t i = 0; i < es->len; i++) {
    AntValue r = ant_call(argv[1], 1, &es->data[i]);
    UNWOUND();
    ant_map_set(out, ((AntRecord *)es->data[i].as.p)->fields[0], r);
  }
  return out;
}

static AntValue m_filter(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  FUNC(argv[1]);
  AntValue snapshot = collect(argv[0], 2);
  AntArray *es = snapshot.as.p;
  AntValue out = ant_map_new();
  for (int64_t i = 0; i < es->len; i++) {
    bool keep = ant_call_truth(argv[1], es->data[i]);
    UNWOUND();
    AntRecord *e = es->data[i].as.p;
    if (keep) ant_map_set(out, e->fields[0], e->fields[1]);
  }
  return out;
}

static void copy_into(AntValue out, AntMap *src) {
  for (int64_t i = 0; i < src->n; i++) {
    if (src->entries[i].alive) ant_map_set(out, src->entries[i].key, src->entries[i].value);
  }
}

static AntValue m_merge(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  MAP(argv[1], n);
  AntValue out = ant_map_new();
  copy_into(out, m);
  copy_into(out, n);
  return out;
}

static AntValue m_same(AntFunc *self, int argc, AntValue *argv) {
  MAP(argv[0], m);
  MAP(argv[1], n);
  return ant_bool(m == n);
}

void ant_ns_map(void) {
  static const AntMember members[] = {
    { "size", "m", m_size },
    { "isEmpty", "m", m_is_empty },
    { "has", "m,k", m_has },
    { "get", "m,k", m_get },
    { "set", "m,k,v", m_set },
    { "remove", "m,k", m_remove },
    { "keys", "m", m_keys },
    { "values", "m", m_values },
    { "entries", "m", m_entries },
    { "fromEntries", "entries", m_from_entries },
    { "map", "m,f", m_map },
    { "filter", "m,f", m_filter },
    { "merge", "m,n", m_merge },
    { "same", "m,n", m_same },
  };
  ANT_NAMESPACE("Map", members);
}
