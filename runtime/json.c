/* The Json namespace (stdlib.md; GAPS G38, G39, G45). Json.stringify is
 * compact: no whitespace, numbers as String.from renders them, NaN and
 * infinities as null, record fields in order without the tag, map keys
 * through String.from, and null for whatever JSON has no form for
 * (functions, tags, sync objects, void, a module, a container that
 * contains itself). Json.parse is a strict RFC 8259 recursive descent
 * parser whose numbers are all float64; it never panics on text, every
 * malformed input is an Err naming the first problem and its byte offset. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ant.h"

/* ---------------------------------------------------------------- stringify */

/* A JSON string literal: `"`, `\`, and control characters escaped,
 * everything else (including non-ASCII) literal. */
static void quote(AntSb *sb, const char *p, int64_t n) {
  ant_sb_append(sb, "\"", 1);
  for (int64_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    const char *esc = c == '"' ? "\\\"" : c == '\\' ? "\\\\" : c == '\n' ? "\\n" : c == '\r' ? "\\r"
                    : c == '\t' ? "\\t" : c == '\b' ? "\\b" : c == '\f' ? "\\f" : NULL;
    if (esc) {
      ant_sb_append_cstr(sb, esc);
    } else if (c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof buf, "\\u%04x", c);
      ant_sb_append_cstr(sb, buf);
    } else {
      ant_sb_append(sb, p + i, 1);
    }
  }
  ant_sb_append(sb, "\"", 1);
}

static void quote_str(AntSb *sb, AntValue s) {
  AntString *x = s.as.p;
  quote(sb, x->data, x->nbytes);
}

static void emit(AntSb *sb, AntValue v, AntActive *active);

static void emit_container(AntSb *sb, AntValue v, AntActive *active) {
  if (ant_active_has(active, v.as.p)) { ant_sb_append_cstr(sb, "null"); return; }
  ant_active_push(active, v.as.p);
  if (v.kind == K_ARRAY) {
    AntArray *a = v.as.p;
    ant_sb_append(sb, "[", 1);
    for (int64_t i = 0; i < a->len; i++) {
      if (i) ant_sb_append(sb, ",", 1);
      emit(sb, a->data[i], active);
    }
    ant_sb_append(sb, "]", 1);
  } else if (v.kind == K_MAP) {
    int64_t n = ant_map_slots(v);
    bool first = true;
    ant_sb_append(sb, "{", 1);
    for (int64_t i = 0; i < n; i++) {
      AntValue k, val;
      if (!ant_map_entry(v, i, &k, &val)) continue;
      if (!first) ant_sb_append(sb, ",", 1);
      first = false;
      quote_str(sb, ant_render(k));   /* the key's top-level String.from text */
      ant_sb_append(sb, ":", 1);
      emit(sb, val, active);
    }
    ant_sb_append(sb, "}", 1);
  } else {
    AntRecord *r = v.as.p;
    ant_sb_append(sb, "{", 1);
    for (int i = 0; i < r->shape->n; i++) {
      if (i) ant_sb_append(sb, ",", 1);
      const char *name = ant_sym_name(r->shape->names[i]);
      quote(sb, name, (int64_t)strlen(name));
      ant_sb_append(sb, ":", 1);
      emit(sb, r->fields[i], active);
    }
    ant_sb_append(sb, "}", 1);
  }
  active->n--;
}

static void emit_float(AntSb *sb, double x, bool f32) {
  if (isfinite(x)) ant_render_float(sb, x, f32);
  else ant_sb_append_cstr(sb, "null");
}

static void emit(AntSb *sb, AntValue v, AntActive *active) {
  switch (v.kind) {
    case K_INT8: case K_INT16: case K_INT32: case K_INT64:
    case K_UINT8: case K_UINT16: case K_UINT32: case K_UINT64:
    case K_BOOL: case K_NULL:
      ant_render_into(sb, v);   /* their String.from text is their JSON text */
      return;
    case K_FLOAT32: emit_float(sb, v.as.f32, true); return;
    case K_FLOAT64: emit_float(sb, v.as.f, false); return;
    case K_STRING: quote_str(sb, v); return;
    case K_ARRAY: case K_MAP: emit_container(sb, v, active); return;
    case K_RECORD:
      if (((AntRecord *)v.as.p)->tag != ant_module_tag()) { emit_container(sb, v, active); return; }
      break;
    default: break;
  }
  ant_sb_append_cstr(sb, "null");
}

static AntValue j_stringify(AntFunc *self, int argc, AntValue *argv) {
  AntSb sb;
  ant_sb_init(&sb);
  AntActive active = { NULL, 0, 0 };
  emit(&sb, argv[0], &active);
  return ant_sb_finish(&sb);
}

/* ---------------------------------------------------------------- parse */

enum { MAX_DEPTH = 10000 };   /* nested arrays and objects: an Err, not a stack overflow */

typedef struct {
  const char *p, *end;    /* the next byte and the end of the text */
  const char *start;      /* the text, for offsets */
  const char *msg;        /* the first error, or NULL */
  int64_t at;             /* its byte offset */
  int depth;
} Parser;

/* Records the first error; the value returned stands for nothing. */
static AntValue fail(Parser *ps, const char *msg) {
  if (!ps->msg) {
    ps->msg = msg;
    ps->at = ps->p - ps->start;
  }
  return ANT_VOID;
}

static void skip_ws(Parser *ps) {
  while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

/* Consumes word when it is next. */
static bool literal(Parser *ps, const char *word) {
  size_t n = strlen(word);
  if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) return false;
  ps->p += n;
  return true;
}

static bool digits(Parser *ps) {
  const char *s = ps->p;
  while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
  return ps->p > s;
}

/* `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`, then strtod on that
 * span: leading zeros, a bare `.5` or `1.`, hex, and `+` are errors. */
static AntValue parse_number(Parser *ps) {
  const char *s = ps->p;
  if (*ps->p == '-') ps->p++;
  if (ps->p < ps->end && *ps->p == '0') ps->p++;
  else if (!digits(ps)) return fail(ps, "invalid number");
  if (ps->p < ps->end && *ps->p == '.') {
    ps->p++;
    if (!digits(ps)) return fail(ps, "invalid number");
  }
  if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
    ps->p++;
    if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
    if (!digits(ps)) return fail(ps, "invalid number");
  }
  int64_t n = ps->p - s;
  char stack[64];
  char *buf = n < (int64_t)sizeof stack ? stack : ant_alloc_atomic((size_t)n + 1);
  memcpy(buf, s, (size_t)n);
  buf[n] = 0;
  return ant_float(strtod(buf, NULL));
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* The four hex digits of a `\u` escape, or -1. */
static int32_t hex4(Parser *ps) {
  if (ps->end - ps->p < 4) return -1;
  int32_t v = 0;
  for (int i = 0; i < 4; i++) {
    int d = hex_digit(ps->p[i]);
    if (d < 0) return -1;
    v = v * 16 + d;
  }
  ps->p += 4;
  return v;
}

/* A `\u` escape's code point: a high surrogate must be followed at once by
 * a `\u` low surrogate, and the pair is one code point (G39). */
static int32_t parse_unicode_escape(Parser *ps) {
  int32_t cp = hex4(ps);
  if (cp < 0) { fail(ps, "invalid \\u escape"); return -1; }
  if (cp >= 0xDC00 && cp <= 0xDFFF) { fail(ps, "lone surrogate"); return -1; }
  if (cp < 0xD800 || cp > 0xDBFF) return cp;
  if (!literal(ps, "\\u")) { fail(ps, "lone surrogate"); return -1; }
  int32_t low = hex4(ps);
  if (low < 0xDC00 || low > 0xDFFF) { fail(ps, "lone surrogate"); return -1; }
  return 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
}

/* The text is an Ant string, hence valid UTF-8, so bytes that are not
 * escapes are copied through, a run at a time; a raw control character is
 * an error. */
static AntValue parse_string(Parser *ps) {
  ps->p++;   /* the opening quote */
  AntSb sb;
  ant_sb_init(&sb);
  for (;;) {
    const char *run = ps->p;
    while (ps->p < ps->end && (unsigned char)*ps->p >= 0x20 && *ps->p != '"' && *ps->p != '\\') ps->p++;
    if (ps->p > run) ant_sb_append(&sb, run, ps->p - run);
    if (ps->p >= ps->end) return fail(ps, "unterminated string");
    unsigned char c = (unsigned char)*ps->p;
    if (c == '"') { ps->p++; return ant_sb_finish(&sb); }
    if (c < 0x20) return fail(ps, "control character in string");
    ps->p++;
    if (ps->p >= ps->end) return fail(ps, "unterminated string");
    char e = *ps->p++;
    switch (e) {
      case '"': case '\\': case '/': ant_sb_append(&sb, &e, 1); break;
      case 'b': ant_sb_append(&sb, "\b", 1); break;
      case 'f': ant_sb_append(&sb, "\f", 1); break;
      case 'n': ant_sb_append(&sb, "\n", 1); break;
      case 'r': ant_sb_append(&sb, "\r", 1); break;
      case 't': ant_sb_append(&sb, "\t", 1); break;
      case 'u': {
        int32_t cp = parse_unicode_escape(ps);
        if (cp < 0) return ANT_VOID;
        ant_sb_append_cp(&sb, cp);
        break;
      }
      default: ps->p--; return fail(ps, "invalid escape");
    }
  }
}

static AntValue parse_value(Parser *ps);

static bool enter(Parser *ps) {
  if (++ps->depth > MAX_DEPTH) { fail(ps, "nesting too deep"); return false; }
  return true;
}

/* Past the closing bracket: the finished container. */
static AntValue leave(Parser *ps, AntValue out) {
  ps->p++;
  ps->depth--;
  return out;
}

static AntValue parse_array(Parser *ps) {
  ps->p++;   /* the `[` */
  if (!enter(ps)) return ANT_VOID;
  AntValue out = ant_array_new(4);
  skip_ws(ps);
  if (ps->p < ps->end && *ps->p == ']') return leave(ps, out);
  for (;;) {
    AntValue v = parse_value(ps);
    if (ps->msg) return ANT_VOID;
    ant_array_push(out, v);
    skip_ws(ps);
    if (ps->p >= ps->end) return fail(ps, "unexpected end");
    if (*ps->p == ']') return leave(ps, out);
    if (*ps->p != ',') return fail(ps, "expected ',' or ']'");
    ps->p++;
  }
}

/* Keys must be strings; a repeated key keeps the last value (G39). */
static AntValue parse_object(Parser *ps) {
  ps->p++;   /* the `{` */
  if (!enter(ps)) return ANT_VOID;
  AntValue out = ant_map_new();
  skip_ws(ps);
  if (ps->p < ps->end && *ps->p == '}') return leave(ps, out);
  for (;;) {
    skip_ws(ps);
    if (ps->p >= ps->end) return fail(ps, "unexpected end");
    if (*ps->p != '"') return fail(ps, "expected a string key");
    AntValue key = parse_string(ps);
    if (ps->msg) return ANT_VOID;
    skip_ws(ps);
    if (ps->p >= ps->end) return fail(ps, "unexpected end");
    if (*ps->p != ':') return fail(ps, "expected ':'");
    ps->p++;
    AntValue v = parse_value(ps);
    if (ps->msg) return ANT_VOID;
    ant_map_set(out, key, v);
    skip_ws(ps);
    if (ps->p >= ps->end) return fail(ps, "unexpected end");
    if (*ps->p == '}') return leave(ps, out);
    if (*ps->p != ',') return fail(ps, "expected ',' or '}'");
    ps->p++;
  }
}

static AntValue parse_value(Parser *ps) {
  skip_ws(ps);
  if (ps->p >= ps->end) return fail(ps, "unexpected end");
  char c = *ps->p;
  switch (c) {
    case '{': return parse_object(ps);
    case '[': return parse_array(ps);
    case '"': return parse_string(ps);
    case 'n': if (literal(ps, "null")) return ANT_NULL; break;
    case 't': if (literal(ps, "true")) return ANT_TRUE; break;
    case 'f': if (literal(ps, "false")) return ANT_FALSE; break;
    default: if (c == '-' || (c >= '0' && c <= '9')) return parse_number(ps); break;
  }
  return fail(ps, "unexpected character");
}

static AntValue j_parse(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], text);
  Parser ps = { text->data, text->data + text->nbytes, text->data, NULL, 0, 0 };
  AntValue v = parse_value(&ps);
  if (!ps.msg) {
    skip_ws(&ps);
    if (ps.p < ps.end) fail(&ps, "trailing characters");
  }
  if (ps.msg) return ant_err(ant_error(ant_fmt("%s at offset %lld", ps.msg, (long long)ps.at)));
  return ant_ok(v);
}

void ant_ns_json(void) {
  static const AntMember members[] = {
    { "parse", "text", j_parse },
    { "stringify", "value", j_stringify },
  };
  ANT_NAMESPACE("Json", members);
}
