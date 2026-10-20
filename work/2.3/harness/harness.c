/* Dev-only harness: links libantrt.a and prints the runtime's rendering or
 * case mapping for inputs read from stdin, one per line.
 *   f <hex64>  -> ant_render of the double with those bits
 *   g <hex32>  -> ant_render of the float32 with those bits
 *   u <text>   -> String.toUpper(text)
 *   l <text>   -> String.toLower(text) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ant.h"

int main(void) {
  ant_gc_init();
  char line[4096];
  AntValue ns = ant_ns_string();
  AntFieldCache cu = {0}, cl = {0};
  AntValue up = ant_field_get(ns, ant_sym("toUpper"), &cu);
  AntValue lo = ant_field_get(ns, ant_sym("toLower"), &cl);
  while (fgets(line, sizeof line, stdin)) {
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n') line[--n] = 0;
    AntValue out;
    if (line[0] == 'f') {
      AntValue v = ant_mk(K_FLOAT64);
      v.as.u = strtoull(line + 2, NULL, 16);
      out = ant_render(v);
    } else if (line[0] == 'g') {
      AntValue v = ant_mk(K_FLOAT32);
      uint32_t bits = (uint32_t)strtoul(line + 2, NULL, 16);
      memcpy(&v.as.f32, &bits, 4);
      out = ant_render(v);
    } else {
      AntValue s = ant_str_new(line + 2, (int64_t)(n - 2));
      out = ant_call(line[0] == 'u' ? up : lo, 1, &s);
    }
    AntString *s = out.as.p;
    fwrite(s->data, 1, (size_t)s->nbytes, stdout);
    fputc('\n', stdout);
  }
  return 0;
}
