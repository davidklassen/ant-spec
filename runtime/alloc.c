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

/* Zeroed memory the collector scans but never frees: a goroutine's state. */
void *ant_alloc_uncollectable(size_t n) {
  void *p = GC_MALLOC_UNCOLLECTABLE(n ? n : 1);
  if (!p) out_of_memory();
  return p;
}

void ant_free(void *p) {
  GC_FREE(p);
}

void ant_gc_init(void) {
  GC_INIT();
}

/* Starts a thread with the given stack size, registered with the collector
 * through the pthread_create redirect; 0 or the errno. */
static int start_thread(pthread_t *thread, void *(*fn)(void *), void *arg, size_t stack, int detachstate) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack);
  pthread_attr_setdetachstate(&attr, detachstate);
  int rc = pthread_create(thread, &attr, fn, arg);
  pthread_attr_destroy(&attr);
  return rc;
}

/* A detached thread: a goroutine's. */
int ant_thread_spawn(void *(*fn)(void *), void *arg, size_t stack) {
  pthread_t thread;
  return start_thread(&thread, fn, arg, stack, PTHREAD_CREATE_DETACHED);
}

/* Runs fn on a thread with a 512 MB stack and waits for it: generated
 * frames are large and Ant programs recurse freely. */
void ant_run_on_big_stack(void *(*fn)(void *), void *arg) {
  pthread_t thread;
  if (start_thread(&thread, fn, arg, (size_t)512 << 20, PTHREAD_CREATE_JOINABLE) != 0) {
    fn(arg);      /* no thread: run here on the main thread's stack */
    return;
  }
  pthread_join(thread, NULL);
}
