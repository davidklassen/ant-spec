/* Boehm GC wrappers. The only file that includes gc.h: GC_THREADS makes it
 * redirect pthread_create so that the big-stack main thread is registered. */
#define GC_THREADS
#include <gc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ant.h"

static void out_of_memory(void) {
  fputs("panic: out of memory\n", stderr);
  abort();
}

void *ant_alloc(size_t n) {
  void *p = GC_MALLOC(n ? n : 1);
  if (!p) out_of_memory();
  return p;
}

/* Memory the collector never scans: string bytes, number arrays. */
void *ant_alloc_atomic(size_t n) {
  void *p = GC_MALLOC_ATOMIC(n ? n : 1);
  if (!p) out_of_memory();
  return p;
}

void *ant_realloc(void *p, size_t n) {
  void *q = GC_REALLOC(p, n ? n : 1);
  if (!q) out_of_memory();
  return q;
}

void ant_gc_init(void) {
  GC_INIT();
}

/* Runs fn on a thread with a 512 MB stack and waits for it: generated
 * frames are large and Ant programs recurse freely. */
void ant_run_on_big_stack(void *(*fn)(void *), void *arg) {
  pthread_attr_t attr;
  pthread_t thread;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, (size_t)512 << 20);
  if (pthread_create(&thread, &attr, fn, arg) != 0) {
    fn(arg);      /* no thread: run here on the main thread's stack */
    return;
  }
  pthread_join(thread, NULL);
}
