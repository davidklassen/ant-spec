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

/* The one rule of every wrapper: a zero-size request is a one-byte block,
 * and exhaustion ends the process. */
static void *checked(void *p) {
  if (!p) out_of_memory();
  return p;
}

void *ant_alloc(size_t n) {
  return checked(GC_MALLOC(n ? n : 1));
}

/* Memory the collector never scans: string bytes, number arrays. */
void *ant_alloc_atomic(size_t n) {
  return checked(GC_MALLOC_ATOMIC(n ? n : 1));
}

/* The growing tables (array data, map entries and index): only their
 * owner's header points to them, so the collector need not keep them
 * reachable through interior pointers, which spares it the search for
 * blacklist-free space that makes a multi-megabyte block slow and noisy. */
void *ant_alloc_big(size_t n) {
  return checked(GC_MALLOC_IGNORE_OFF_PAGE(n ? n : 1));
}

void *ant_alloc_atomic_big(size_t n) {
  return checked(GC_MALLOC_ATOMIC_IGNORE_OFF_PAGE(n ? n : 1));
}

void *ant_realloc(void *p, size_t n) {
  return checked(GC_REALLOC(p, n ? n : 1));
}

/* Zeroed memory the collector scans but never frees: a goroutine's state. */
void *ant_alloc_uncollectable(size_t n) {
  return checked(GC_MALLOC_UNCOLLECTABLE(n ? n : 1));
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
