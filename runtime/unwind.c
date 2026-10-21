/* Frames, panics, `return`, deferred blocks, traces, and the program root
 * (section 12, "Deferred blocks and panics"; design section 2). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ant.h"

AntValue ant_args;

void ant_set_return(AntFrame *target, AntValue v) {
  AntUnwind *u = &ant_g->unwind;
  u->mode = U_RETURN;
  u->value = v;
  u->target = target;
}

/* The module initializer's epilogue: a top-level `return` aimed at it panics (G24). */
void ant_finish_init(AntFrame *fr) {
  AntUnwind *u = &ant_g->unwind;
  if (u->mode == U_RETURN && u->target == fr) {
    u->mode = U_NONE;
    ant_panic("return outside a function");
  }
}

/* The trace is taken when the panic starts, so unwinding does not lose it. */
static AntTrace *snapshot(void) {
  int n = 0;
  for (AntFrame *f = ant_g->top; f; f = f->parent) n++;
  AntTrace *t = ant_alloc(sizeof(AntTrace));
  t->n = n;
  t->lines = ant_alloc(sizeof(char *) * (size_t)(n ? n : 1));
  int i = 0;
  for (AntFrame *f = ant_g->top; f; f = f->parent) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s (%s:%d)", f->info->name, f->info->file, f->line);
    size_t len = strlen(buf);
    char *line = ant_alloc_atomic(len + 1);
    memcpy(line, buf, len + 1);
    t->lines[i++] = line;
  }
  return t;
}

void ant_panic_value(AntValue msg) {
  AntUnwind *u = &ant_g->unwind;
  u->mode = U_PANIC;
  u->value = msg;
  u->target = NULL;
  u->trace = snapshot();
}

void ant_panic(const char *msg) {
  ant_panic_value(ant_str_cstr(msg));
}

void ant_panicf(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  ant_panic(buf);
}

void ant_defer_push(AntDefers *d, AntValue closure) {
  if (d->n == d->cap) {
    d->cap = d->cap ? d->cap * 2 : 4;
    d->items = ant_realloc(d->items, sizeof(AntValue) * (size_t)d->cap);
  }
  d->items[d->n++] = closure;
}

/* Runs a block's deferred closures in reverse order. Each runs with the
 * pending unwind set aside; a panic inside one replaces it (G59), anything
 * else restores it, and the remaining closures still run. */
void ant_run_defers(AntDefers *d) {
  AntUnwind *u = &ant_g->unwind;
  for (int i = d->n - 1; i >= 0; i--) {
    AntUnwind saved = *u;
    u->mode = U_NONE;
    ant_call(d->items[i], 0, NULL);
    if (u->mode != U_PANIC) *u = saved;
  }
  d->n = 0;
}

/* Writes the pending panic and its trace to stderr and clears it. */
void ant_report_panic(void) {
  AntUnwind *u = &ant_g->unwind;
  fflush(stdout);
  AntString *msg = u->value.kind == K_STRING ? u->value.as.p : NULL;
  fprintf(stderr, "panic: %s\n", msg ? msg->data : "?");
  if (u->trace) {
    for (int i = 0; i < u->trace->n; i++) fprintf(stderr, "  at %s\n", u->trace->lines[i]);
  }
  fflush(stderr);
  u->mode = U_NONE;
  u->trace = NULL;
}

_Noreturn void ant_exit(int status) {
  fflush(stdout);
  fflush(stderr);
  _exit(status);
}

/* ---------------------------------------------------------------- program root */

void ant_runtime_init(int argc, char **argv) {
  ant_gc_init();
  static char outbuf[1 << 16];
  setvbuf(stdout, outbuf, _IOFBF, sizeof outbuf);
  ant_args = ant_array_new(argc);
  for (int i = 1; i < argc; i++) ant_array_push(ant_args, ant_str_cstr(argv[i]));
}

typedef struct { void (*init)(void); AntValue *module; int main; } MainRun;

/* The main goroutine. It ends the process from its own thread while it
 * holds the GIL, so no other goroutine is mid-operation (section 12: the
 * program ends when main returns, whatever other goroutines are doing). */
static void *run_main_thread(void *p) {
  MainRun *run = p;
  ant_sched_enter_main();
  run->init();
  if (!ant_g->unwind.mode) ant_call(ant_module_get(*run->module, run->main), 0, NULL);
  if (ant_g->unwind.mode == U_PANIC) {
    ant_report_panic();
    ant_exit(2);
  }
  ant_exit(0);
}

/* Initializes the entry module and calls its `main` (the module's field
 * main_idx) on the big-stack thread, which exits with 0, or 2 after a
 * reported panic. */
int ant_run_main(void (*init)(void), AntValue *module, int main_idx) {
  MainRun run = { init, module, main_idx };
  ant_run_on_big_stack(run_main_thread, &run);
  ant_exit(2);   /* not reached: the main goroutine exits the process */
}
