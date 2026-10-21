/* String.from (section 11; GAPS G33, G55): strings bare at the top level
 * and quoted inside containers, shortest round-trip floats laid out with
 * JavaScript's thresholds, `...` for a container being rendered. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ant.h"

bool ant_active_has(AntActive *a, void *p) {
  for (int i = 0; i < a->n; i++) if (a->items[i] == p) return true;
  return false;
}

void ant_active_push(AntActive *a, void *p) {
  ANT_PUSH(a->items, a->n, a->cap, 16, p);
}

static bool reads_back(const char *buf, double x, bool f32) {
  return f32 ? strtof(buf, NULL) == (float)x : strtod(buf, NULL) == x;
}

/* Replaces the mantissa of buf (d.ddde[+-]xx or de[+-]xx) by its successor at
 * the same precision: the last digit plus one, carrying; when the carry runs
 * off the front the mantissa becomes 1.00... and the exponent grows by one. */
static void next_decimal(char *buf, size_t size) {
  char *e = strchr(buf, 'e');
  for (char *p = e - 1; ; p--) {
    if (*p == '.') continue;
    if (*p < '9') { (*p)++; return; }
    *p = '0';
    if (p == buf) {   /* every digit was 9: 9.99e+05 becomes 1.00e+06 */
      *p = '1';
      snprintf(e, size - (size_t)(e - buf), "e%+03d", atoi(e + 1) + 1);
      return;
    }
  }
}

/* The shortest decimal digit string that reads back as x (x > 0, finite)
 * and the position n of the decimal point: x = 0.<digits> * 10^n.
 * At each precision the correctly rounded decimal is tried first; it is
 * the nearest, but at a power of two the round-trip interval is asymmetric
 * (narrower below), so the next decimal above it may read back when it
 * does not. */
static void shortest_digits(double x, bool f32, char *digits, int *n) {
  char buf[40];
  int maxp = f32 ? 9 : 17;
  for (int p = 1; p <= maxp; p++) {
    snprintf(buf, sizeof buf, "%.*e", p - 1, x);
    if (reads_back(buf, x, f32) || p == maxp) break;
    next_decimal(buf, sizeof buf);
    if (reads_back(buf, x, f32)) break;
  }
  /* buf is d.ddde[+-]xx or de[+-]xx */
  char *e = strchr(buf, 'e');
  int exponent = atoi(e + 1);
  int k = 0;
  for (char *p = buf; p < e; p++) if (*p != '.') digits[k++] = *p;
  while (k > 1 && digits[k - 1] == '0') k--;
  digits[k] = 0;
  *n = exponent + 1;
}

static void layout(AntSb *sb, const char *digits, int n) {
  int k = (int)strlen(digits);
  char buf[64];
  if (k <= n && n <= 21) {
    ant_sb_append_cstr(sb, digits);
    for (int i = k; i < n; i++) ant_sb_append(sb, "0", 1);
    ant_sb_append(sb, ".0", 2);
  } else if (0 < n && n <= 21) {
    ant_sb_append(sb, digits, n);
    ant_sb_append(sb, ".", 1);
    ant_sb_append_cstr(sb, digits + n);
  } else if (-6 < n && n <= 0) {
    ant_sb_append(sb, "0.", 2);
    for (int i = 0; i < -n; i++) ant_sb_append(sb, "0", 1);
    ant_sb_append_cstr(sb, digits);
  } else {
    ant_sb_append(sb, digits, 1);
    if (k > 1) {
      ant_sb_append(sb, ".", 1);
      ant_sb_append_cstr(sb, digits + 1);
    }
    snprintf(buf, sizeof buf, "e%d", n - 1);
    ant_sb_append_cstr(sb, buf);
  }
}

void ant_render_float(AntSb *sb, double x, bool f32) {
  if (x != x) { ant_sb_append_cstr(sb, "NaN"); return; }
  if (x == INFINITY) { ant_sb_append_cstr(sb, "Infinity"); return; }
  if (x == -INFINITY) { ant_sb_append_cstr(sb, "-Infinity"); return; }
  if (signbit(x)) ant_sb_append(sb, "-", 1);
  if (x == 0.0) { ant_sb_append_cstr(sb, "0.0"); return; }
  char digits[40];
  int n;
  shortest_digits(fabs(x), f32, digits, &n);
  layout(sb, digits, n);
}

static void render_quoted(AntSb *sb, AntString *s) {
  ant_sb_append(sb, "\"", 1);
  for (int64_t i = 0; i < s->nbytes; i++) {
    char c = s->data[i];
    const char *esc = c == '\\' ? "\\\\" : c == '"' ? "\\\"" : c == '\n' ? "\\n" : c == '\t' ? "\\t" : c == '\r' ? "\\r" : NULL;
    if (esc) ant_sb_append_cstr(sb, esc); else ant_sb_append(sb, &c, 1);
  }
  ant_sb_append(sb, "\"", 1);
}

/* The decimal digits of mag, written backwards into a local and appended once. */
static void render_uint(AntSb *sb, uint64_t mag) {
  char buf[24];
  int i = sizeof buf;
  do { buf[--i] = (char)('0' + mag % 10); mag /= 10; } while (mag);
  ant_sb_append(sb, buf + i, (int64_t)(sizeof buf - (size_t)i));
}

static void render_in(AntSb *sb, AntValue v, AntActive *active) {
  switch (v.kind) {
    case K_INT8: case K_INT16: case K_INT32: case K_INT64:
      if (v.as.i < 0) ant_sb_append(sb, "-", 1);
      render_uint(sb, v.as.i < 0 ? (uint64_t)0 - (uint64_t)v.as.i : (uint64_t)v.as.i);
      return;
    case K_UINT8: case K_UINT16: case K_UINT32: case K_UINT64:
      render_uint(sb, v.as.u);
      return;
    case K_FLOAT32: ant_render_float(sb, v.as.f32, true); return;
    case K_FLOAT64: ant_render_float(sb, v.as.f, false); return;
    case K_BOOL: ant_sb_append_cstr(sb, v.as.b ? "true" : "false"); return;
    case K_STRING: render_quoted(sb, v.as.p); return;
    case K_NULL: ant_sb_append_cstr(sb, "null"); return;
    case K_VOID: ant_sb_append_cstr(sb, "void"); return;
    case K_FUNC: ant_sb_append_cstr(sb, "func"); return;
    case K_TAG: ant_sb_append_cstr(sb, ((AntTag *)v.as.p)->name); return;
    case K_SYNC: ant_sb_append_cstr(sb, ((AntSync *)v.as.p)->name); return;
    case K_RECORD:
      if (((AntRecord *)v.as.p)->tag == ant_module_tag()) { ant_sb_append_cstr(sb, "Module"); return; }
      break;
    case K_ARRAY: case K_MAP: break;
    default: ant_sb_append_cstr(sb, ant_kind_name(v.kind)); return;
  }
  if (ant_active_has(active, v.as.p)) { ant_sb_append_cstr(sb, "..."); return; }
  ant_active_push(active, v.as.p);
  if (v.kind == K_ARRAY) {
    AntArray *a = v.as.p;
    ant_sb_append(sb, "[", 1);
    for (int64_t i = 0; i < a->len; i++) {
      if (i) ant_sb_append(sb, ", ", 2);
      render_in(sb, a->data[i], active);
    }
    ant_sb_append(sb, "]", 1);
  } else if (v.kind == K_MAP) {
    int64_t n = ant_map_slots(v);
    bool first = true;
    ant_sb_append_cstr(sb, "Map{");
    for (int64_t i = 0; i < n; i++) {
      AntValue k, val;
      if (!ant_map_entry(v, i, &k, &val)) continue;
      ant_sb_append_cstr(sb, first ? " " : ", ");
      first = false;
      render_in(sb, k, active);
      ant_sb_append(sb, ": ", 2);
      render_in(sb, val, active);
    }
    ant_sb_append_cstr(sb, first ? "}" : " }");
  } else {
    AntRecord *r = v.as.p;
    ant_sb_append_cstr(sb, r->tag->name);
    ant_sb_append(sb, "{", 1);
    for (int i = 0; i < r->shape->n; i++) {
      ant_sb_append_cstr(sb, i ? ", " : " ");
      ant_sb_append_cstr(sb, ant_sym_name(r->shape->names[i]));
      ant_sb_append(sb, ": ", 2);
      render_in(sb, r->fields[i], active);
    }
    ant_sb_append_cstr(sb, r->shape->n ? " }" : "}");
  }
  active->n--;
}

/* Renders v as it appears inside a container (a string quoted). */
void ant_render_into(AntSb *sb, AntValue v) {
  AntActive active = { NULL, 0, 0 };
  render_in(sb, v, &active);
}

AntValue ant_render(AntValue v) {
  if (v.kind == K_STRING) return v;
  AntSb sb;
  ant_sb_init(&sb);
  ant_render_into(&sb, v);
  return ant_sb_finish(&sb);
}
