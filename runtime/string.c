/* Strings: immutable UTF-8 with the code point count computed once, a
 * builder, and the String namespace (section 11). */
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ant.h"

/* ---------------------------------------------------------------- UTF-8 */

int ant_utf8_decode(const char *p, int64_t n, int32_t *cp) {
  unsigned char c = (unsigned char)p[0];
  if (c < 0x80) { *cp = c; return 1; }
  int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
  if (len > n) len = (int)n;
  int32_t v = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
  for (int i = 1; i < len; i++) v = (v << 6) | ((unsigned char)p[i] & 0x3F);
  *cp = v;
  return len;
}

/* Strict validation: no overlong forms, surrogates, or values above 10FFFF. */
bool ant_utf8_valid(const char *p, int64_t n) {
  int64_t i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)p[i];
    if (c < 0x80) { i++; continue; }
    int len; int32_t min;
    if (c >= 0xC2 && c <= 0xDF) { len = 2; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { len = 3; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { len = 4; min = 0x10000; }
    else return false;
    if (i + len > n) return false;
    int32_t v = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
    for (int k = 1; k < len; k++) {
      unsigned char d = (unsigned char)p[i + k];
      if ((d & 0xC0) != 0x80) return false;
      v = (v << 6) | (d & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

/* ---------------------------------------------------------------- creation */

AntValue ant_str_new(const char *bytes, int64_t n) {
  AntString *s = ant_alloc_atomic(sizeof(AntString) + (size_t)n + 1);
  s->nbytes = n;
  s->hash = 0;
  memcpy(s->data, bytes, (size_t)n);
  s->data[n] = 0;
  int64_t cps = 0;
  bool ascii = true;
  for (int64_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)bytes[i];
    if (c >= 0x80) ascii = false;
    if ((c & 0xC0) != 0x80) cps++;
  }
  s->ncps = cps;
  s->ascii = ascii;
  return ant_ptr(K_STRING, s);
}

AntValue ant_str_lit(const char *bytes, int64_t n) {
  return ant_str_new(bytes, n);
}

AntValue ant_str_cstr(const char *s) {
  return ant_str_new(s, (int64_t)strlen(s));
}

AntString *ant_str(AntValue v) {
  if (v.kind != K_STRING) {
    ant_panic("wrong kind: expected a string");
    return NULL;
  }
  return v.as.p;
}

AntValue ant_str_concat(AntValue a, AntValue b) {
  AntString *x = a.as.p, *y = b.as.p;
  if (x->nbytes == 0) return b;
  if (y->nbytes == 0) return a;
  AntString *s = ant_alloc_atomic(sizeof(AntString) + (size_t)(x->nbytes + y->nbytes) + 1);
  s->nbytes = x->nbytes + y->nbytes;
  s->ncps = x->ncps + y->ncps;
  s->hash = 0;
  s->ascii = x->ascii && y->ascii;
  memcpy(s->data, x->data, (size_t)x->nbytes);
  memcpy(s->data + x->nbytes, y->data, (size_t)y->nbytes);
  s->data[s->nbytes] = 0;
  return ant_ptr(K_STRING, s);
}

/* Byte order, which is code point order for valid UTF-8. */
int ant_str_cmp(AntString *a, AntString *b) {
  int64_t n = a->nbytes < b->nbytes ? a->nbytes : b->nbytes;
  int c = memcmp(a->data, b->data, (size_t)n);
  if (c) return c;
  return a->nbytes < b->nbytes ? -1 : a->nbytes > b->nbytes ? 1 : 0;
}

bool ant_str_eq(AntString *a, AntString *b) {
  return a == b || (a->nbytes == b->nbytes && memcmp(a->data, b->data, (size_t)a->nbytes) == 0);
}

uint32_t ant_str_hash(AntString *s) {
  if (s->hash) return s->hash;
  uint32_t h = 2166136261u;
  for (int64_t i = 0; i < s->nbytes; i++) h = (h ^ (unsigned char)s->data[i]) * 16777619u;
  s->hash = h ? h : 1;
  return s->hash;
}

/* The byte offset `count` code points after the code point at byte offset `off`. */
static int64_t advance(AntString *s, int64_t off, int64_t count) {
  if (s->ascii) return off + count;
  while (count-- > 0) {
    off++;
    while (off < s->nbytes && ((unsigned char)s->data[off] & 0xC0) == 0x80) off++;
  }
  return off;
}

AntValue ant_str_slice_cps(AntString *s, int64_t from, int64_t to) {
  int64_t a = advance(s, 0, from), b = advance(s, a, to - from);
  return ant_str_new(s->data + a, b - a);
}

/* ---------------------------------------------------------------- builder */

void ant_sb_init(AntSb *sb) {
  sb->len = 0;
  sb->cap = 64;
  sb->data = ant_alloc_atomic((size_t)sb->cap);
}

void ant_sb_append(AntSb *sb, const char *p, int64_t n) {
  if (sb->len + n > sb->cap) {
    while (sb->len + n > sb->cap) sb->cap *= 2;
    sb->data = ant_realloc(sb->data, (size_t)sb->cap);
  }
  memcpy(sb->data + sb->len, p, (size_t)n);
  sb->len += n;
}

void ant_sb_append_cstr(AntSb *sb, const char *p) {
  ant_sb_append(sb, p, (int64_t)strlen(p));
}

void ant_sb_append_str(AntSb *sb, AntValue s) {
  AntString *x = s.as.p;
  ant_sb_append(sb, x->data, x->nbytes);
}

/* Appends cp (a valid code point) as UTF-8: the lead byte of its length,
 * then its six-bit groups from the top down. */
void ant_sb_append_cp(AntSb *sb, int32_t cp) {
  unsigned char b[4];
  int n;
  if (cp < 0x80) { b[0] = (unsigned char)cp; n = 1; }
  else if (cp < 0x800) { b[0] = (unsigned char)(0xC0 | (cp >> 6)); n = 2; }
  else if (cp < 0x10000) { b[0] = (unsigned char)(0xE0 | (cp >> 12)); n = 3; }
  else { b[0] = (unsigned char)(0xF0 | (cp >> 18)); n = 4; }
  for (int i = 1; i < n; i++) b[i] = (unsigned char)(0x80 | ((cp >> (6 * (n - 1 - i))) & 0x3F));
  ant_sb_append(sb, (const char *)b, n);
}

AntValue ant_sb_finish(AntSb *sb) {
  return ant_str_new(sb->data, sb->len);
}

/* A template part: a string bare, anything else rendered. */
void ant_template_add(AntSb *sb, AntValue v) {
  if (v.kind == K_STRING) ant_sb_append_str(sb, v);
  else ant_render_into(sb, v);
}

/* ---------------------------------------------------------------- natives */

static AntValue s_length(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  return ant_int(s->ncps);
}

static AntValue s_is_empty(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  return ant_bool(s->nbytes == 0);
}

static AntValue s_slice(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  INT(argv[1], from);
  INT(argv[2], to);
  if (from < 0 || from > to || to > s->ncps) { ant_panic("index out of range"); return ANT_VOID; }
  return ant_str_slice_cps(s, from, to);
}

static AntValue join_parts(AntValue parts, AntString *sep) {
  ARR(parts, xs);
  for (int64_t i = 0; i < xs->len; i++) if (!ant_str(xs->data[i])) return ANT_VOID;
  AntSb sb;
  ant_sb_init(&sb);
  for (int64_t i = 0; i < xs->len; i++) {
    if (i > 0 && sep) ant_sb_append(&sb, sep->data, sep->nbytes);
    ant_sb_append_str(&sb, xs->data[i]);
  }
  return ant_sb_finish(&sb);
}

static AntValue s_concat(AntFunc *self, int argc, AntValue *argv) {
  return join_parts(argv[0], NULL);
}

static AntValue s_join(AntFunc *self, int argc, AntValue *argv) {
  if (!ant_need_array(argv[0])) return ANT_VOID;
  STR(argv[1], sep);
  return join_parts(argv[0], sep);
}

static AntValue s_repeat(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  INT(argv[1], n);
  if (n < 0) { ant_panic("negative count"); return ANT_VOID; }
  AntSb sb;
  ant_sb_init(&sb);
  for (int64_t i = 0; i < n; i++) ant_sb_append(&sb, s->data, s->nbytes);
  return ant_sb_finish(&sb);
}

/* The code points with the Unicode White_Space property. */
static bool is_white_space(int32_t cp) {
  switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0x85: case 0xA0:
    case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
      return true;
    default:
      return cp >= 0x2000 && cp <= 0x200A;
  }
}

static AntValue s_trim(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  int64_t start = 0, end = s->nbytes;
  while (start < end) {
    int32_t cp;
    int len = ant_utf8_decode(s->data + start, end - start, &cp);
    if (!is_white_space(cp)) break;
    start += len;
  }
  while (end > start) {
    int64_t k = end - 1;
    while (k > start && ((unsigned char)s->data[k] & 0xC0) == 0x80) k--;
    int32_t cp;
    ant_utf8_decode(s->data + k, end - k, &cp);
    if (!is_white_space(cp)) break;
    end = k;
  }
  return ant_str_new(s->data + start, end - start);
}

/* The byte offset of the first occurrence of t in s at or after `from`, or -1. */
static int64_t find_bytes(AntString *s, AntString *t, int64_t from) {
  if (t->nbytes == 0) return from <= s->nbytes ? from : -1;
  for (int64_t i = from; i + t->nbytes <= s->nbytes; i++) {
    if (s->data[i] == t->data[0] && memcmp(s->data + i, t->data, (size_t)t->nbytes) == 0) return i;
  }
  return -1;
}

static int64_t cps_before(AntString *s, int64_t byteoff) {
  if (s->ascii) return byteoff;
  int64_t n = 0;
  for (int64_t i = 0; i < byteoff; i++) if (((unsigned char)s->data[i] & 0xC0) != 0x80) n++;
  return n;
}

static AntValue s_contains(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  STR(argv[1], t);
  return ant_bool(find_bytes(s, t, 0) >= 0);
}

static AntValue s_starts_with(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  STR(argv[1], t);
  return ant_bool(t->nbytes <= s->nbytes && memcmp(s->data, t->data, (size_t)t->nbytes) == 0);
}

static AntValue s_ends_with(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  STR(argv[1], t);
  return ant_bool(t->nbytes <= s->nbytes &&
                  memcmp(s->data + s->nbytes - t->nbytes, t->data, (size_t)t->nbytes) == 0);
}

static AntValue s_index_of(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  STR(argv[1], t);
  int64_t i = find_bytes(s, t, 0);
  return i < 0 ? ANT_NULL : ant_int(cps_before(s, i));
}

static AntValue s_split(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  STR(argv[1], sep);
  if (sep->nbytes == 0) { ant_panic("empty separator"); return ANT_VOID; }
  AntValue out = ant_array_new(4);
  int64_t start = 0;
  for (;;) {
    int64_t i = find_bytes(s, sep, start);
    if (i < 0) break;
    ant_array_push(out, ant_str_new(s->data + start, i - start));
    start = i + sep->nbytes;
  }
  ant_array_push(out, ant_str_new(s->data + start, s->nbytes - start));
  return out;
}

static AntValue s_replace(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  STR(argv[1], old);
  STR(argv[2], new);
  if (old->nbytes == 0) { ant_panic("empty pattern"); return ANT_VOID; }
  AntSb sb;
  ant_sb_init(&sb);
  int64_t start = 0;
  for (;;) {
    int64_t i = find_bytes(s, old, start);
    if (i < 0) break;
    ant_sb_append(&sb, s->data + start, i - start);
    ant_sb_append(&sb, new->data, new->nbytes);
    start = i + old->nbytes;
  }
  ant_sb_append(&sb, s->data + start, s->nbytes - start);
  return ant_sb_finish(&sb);
}

/* Simple one-to-one case mapping per code point (G36) through the generated
 * tables of unicase.c; a code point without such a mapping stays. */
static AntValue case_map(AntValue v, bool upper) {
  STR(v, s);
  AntSb sb;
  ant_sb_init(&sb);
  int64_t i = 0;
  while (i < s->nbytes) {
    int32_t cp;
    i += ant_utf8_decode(s->data + i, s->nbytes - i, &cp);
    ant_sb_append_cp(&sb, upper ? ant_cp_upper(cp) : ant_cp_lower(cp));
  }
  return ant_sb_finish(&sb);
}

static AntValue s_to_upper(AntFunc *self, int argc, AntValue *argv) { return case_map(argv[0], true); }
static AntValue s_to_lower(AntFunc *self, int argc, AntValue *argv) { return case_map(argv[0], false); }

static AntValue s_chars(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  AntValue out = ant_array_new(s->ncps);
  int64_t i = 0;
  while (i < s->nbytes) {
    int32_t cp;
    int len = ant_utf8_decode(s->data + i, s->nbytes - i, &cp);
    ant_array_push(out, ant_str_new(s->data + i, len));
    i += len;
  }
  return out;
}

static AntValue s_to_bytes(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  AntValue out = ant_array_new(s->nbytes);
  for (int64_t i = 0; i < s->nbytes; i++) {
    AntValue b = ant_mk(K_UINT8);
    b.as.u = (unsigned char)s->data[i];
    ant_array_push(out, b);
  }
  return out;
}

static AntValue s_from_bytes(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  int64_t n = xs->len;
  char *buf = ant_alloc_atomic((size_t)n + 1);
  for (int64_t i = 0; i < n; i++) {
    if (xs->data[i].kind != K_UINT8) { ant_panic("wrong kind: expected a byte"); return ANT_VOID; }
    buf[i] = (char)xs->data[i].as.u;
  }
  if (!ant_utf8_valid(buf, n)) return ANT_NULL;
  return ant_str_new(buf, n);
}

static AntValue s_to_int(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  const char *p = s->data;
  int64_t i = 0, n = s->nbytes;
  bool neg = false;
  if (i < n && (p[i] == '+' || p[i] == '-')) { neg = p[i] == '-'; i++; }
  if (i == n) return ANT_NULL;
  uint64_t acc = 0;
  for (; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') return ANT_NULL;
    uint64_t d = (uint64_t)(p[i] - '0');
    if (acc > (UINT64_MAX - d) / 10) return ANT_NULL;
    acc = acc * 10 + d;
  }
  if (neg) {
    if (acc > (uint64_t)INT64_MAX + 1) return ANT_NULL;
    return ant_int((int64_t)(0 - acc));
  }
  if (acc > (uint64_t)INT64_MAX) return ANT_NULL;
  return ant_int((int64_t)acc);
}

/* An optional sign, digits, an optional fraction, an optional exponent; null otherwise. */
static AntValue s_to_float(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], s);
  const char *p = s->data;
  int64_t i = 0, n = s->nbytes;
  if (i < n && (p[i] == '+' || p[i] == '-')) i++;
  int64_t digits = 0;
  while (i < n && p[i] >= '0' && p[i] <= '9') { i++; digits++; }
  if (digits == 0) return ANT_NULL;
  if (i < n && p[i] == '.') {
    i++;
    int64_t frac = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') { i++; frac++; }
    if (frac == 0) return ANT_NULL;
  }
  if (i < n && (p[i] == 'e' || p[i] == 'E')) {
    i++;
    if (i < n && (p[i] == '+' || p[i] == '-')) i++;
    int64_t ed = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') { i++; ed++; }
    if (ed == 0) return ANT_NULL;
  }
  if (i != n) return ANT_NULL;
  double v = strtod(p, NULL);
  if (!isfinite(v)) return ANT_NULL;
  return ant_float(v);
}

static AntValue s_from(AntFunc *self, int argc, AntValue *argv) {
  return ant_render(argv[0]);
}

AntValue ant_ns_string(void) {
  static const AntMember members[] = {
    { "length", "s", s_length },
    { "isEmpty", "s", s_is_empty },
    { "slice", "s,from,to", s_slice },
    { "concat", "parts", s_concat },
    { "split", "s,sep", s_split },
    { "join", "parts,sep", s_join },
    { "repeat", "s,n", s_repeat },
    { "trim", "s", s_trim },
    { "contains", "s,t", s_contains },
    { "startsWith", "s,t", s_starts_with },
    { "endsWith", "s,t", s_ends_with },
    { "indexOf", "s,t", s_index_of },
    { "replace", "s,old,new", s_replace },
    { "toUpper", "s", s_to_upper },
    { "toLower", "s", s_to_lower },
    { "chars", "s", s_chars },
    { "toBytes", "s", s_to_bytes },
    { "fromBytes", "bytes", s_from_bytes },
    { "toInt", "s", s_to_int },
    { "toFloat", "s", s_to_float },
    { "from", "value", s_from },
  };
  return ANT_NAMESPACE("String", members);
}
