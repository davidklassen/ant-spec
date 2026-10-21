/* print, panic, and the Os, Time and File namespaces (stdlib.md). The
 * File handles (open, readLine, ...) and Json arrive with step 2.7. */
#include <dirent.h>
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ant.h"

extern char **environ;

AntValue ant_native_print(AntFunc *self, int argc, AntValue *argv) {
  AntValue s = ant_render(argv[0]);
  AntString *str = s.as.p;
  fwrite(str->data, 1, (size_t)str->nbytes, stdout);
  fputc('\n', stdout);
  return ANT_VOID;
}

AntValue ant_native_panic(AntFunc *self, int argc, AntValue *argv) {
  if (argv[0].kind != K_STRING) {
    ant_panic("wrong kind: panic needs a string");
    return ANT_VOID;
  }
  ant_panic_value(argv[0]);
  return ANT_VOID;
}

/* The Err for the error code err of a call on path: `strerror: path`. */
static AntValue io_error(int err, const char *path) {
  char buf[512];
  snprintf(buf, sizeof buf, "%s: %s", strerror(err), path);
  return ant_err(ant_error(buf));
}

/* ---------------------------------------------------------------- Os */

static AntValue os_args(AntFunc *self, int argc, AntValue *argv) {
  AntArray *a = ant_args.as.p;
  AntValue out = ant_array_new(a->len);
  for (int64_t i = 0; i < a->len; i++) ant_array_push(out, a->data[i]);
  return out;
}

static AntValue os_env(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], name);
  const char *v = getenv(name->data);
  return v ? ant_str_cstr(v) : ANT_NULL;
}

static AntValue os_exit(AntFunc *self, int argc, AntValue *argv) {
  INT(argv[0], status);
  ant_exit((int)(status & 0xFF));
  return ANT_VOID;
}

/* Os.exec(args): runs args[0] with the rest as arguments, stdio inherited;
 * Ok(exit status) or Err when it cannot be started. */
static AntValue os_exec(AntFunc *self, int argc, AntValue *argv) {
  ARR(argv[0], xs);
  char **items = ant_alloc(sizeof(char *) * (size_t)(xs->len + 1));
  for (int64_t i = 0; i < xs->len; i++) {
    STR(xs->data[i], s);
    items[i] = s->data;
  }
  items[xs->len] = NULL;
  if (xs->len == 0) return ant_err(ant_error("no program"));
  fflush(stdout);
  fflush(stderr);
  pid_t pid;
  int rc = posix_spawnp(&pid, items[0], NULL, NULL, items, environ);
  if (rc != 0) return io_error(rc, items[0]);
  int status = 0;
  ant_gil_release();   /* blocks only the caller, like Time.sleep: pid and status are C data */
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  ant_gil_acquire();
  if (WIFSIGNALED(status)) return ant_ok(ant_int(128 + WTERMSIG(status)));
  return ant_ok(ant_int(WEXITSTATUS(status)));
}

AntValue ant_ns_os(void) {
  static const AntMember members[] = {
    { "args", "", os_args },
    { "env", "name", os_env },
    { "exit", "status", os_exit },
    { "exec", "args", os_exec },
  };
  return ANT_NAMESPACE("Os", members);
}

/* ---------------------------------------------------------------- Time */

static AntValue clock_ms(clockid_t clock) {
  struct timespec ts;
  clock_gettime(clock, &ts);
  return ant_int((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static AntValue time_now(AntFunc *self, int argc, AntValue *argv) { return clock_ms(CLOCK_REALTIME); }
static AntValue time_monotonic(AntFunc *self, int argc, AntValue *argv) { return clock_ms(CLOCK_MONOTONIC); }

/* Blocks only the caller: the GIL is released around the sleep, and
 * nothing in between touches an AntValue. */
static AntValue time_sleep(AntFunc *self, int argc, AntValue *argv) {
  INT(argv[0], ms);
  if (ms <= 0) return ANT_VOID;
  struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
  ant_gil_release();
  while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
  ant_gil_acquire();
  return ANT_VOID;
}

AntValue ant_ns_time(void) {
  static const AntMember members[] = {
    { "now", "", time_now },
    { "sleep", "ms", time_sleep },
    { "monotonic", "", time_monotonic },
  };
  return ANT_NAMESPACE("Time", members);
}

/* ---------------------------------------------------------------- File */

static AntValue file_read(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], path);
  FILE *f = fopen(path->data, "rb");
  if (!f) return io_error(errno, path->data);
  AntSb sb;
  ant_sb_init(&sb);
  char buf[1 << 16];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) ant_sb_append(&sb, buf, (int64_t)n);
  bool failed = ferror(f);
  fclose(f);
  if (failed) return io_error(errno, path->data);
  if (!ant_utf8_valid(sb.data, sb.len)) {
    char msg[512];
    snprintf(msg, sizeof msg, "invalid UTF-8: %s", path->data);
    return ant_err(ant_error(msg));
  }
  return ant_ok(ant_sb_finish(&sb));
}

static AntValue file_write(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], path);
  STR(argv[1], text);
  FILE *f = fopen(path->data, "wb");
  if (!f) return io_error(errno, path->data);
  bool failed = fwrite(text->data, 1, (size_t)text->nbytes, f) != (size_t)text->nbytes;
  if (fclose(f) != 0) failed = true;
  if (failed) return io_error(errno, path->data);
  return ant_ok(ANT_VOID);
}

static AntValue file_exists(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], path);
  struct stat st;
  return ant_bool(stat(path->data, &st) == 0);
}

static AntValue file_remove(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], path);
  if (remove(path->data) != 0) return io_error(errno, path->data);
  return ant_ok(ANT_VOID);
}

static AntValue file_list(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], path);
  DIR *d = opendir(path->data);
  if (!d) return io_error(errno, path->data);
  AntValue out = ant_array_new(8);
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    ant_array_push(out, ant_str_cstr(e->d_name));
  }
  closedir(d);
  return ant_ok(out);
}

AntValue ant_ns_file(void) {
  static const AntMember members[] = {
    { "read", "path", file_read },
    { "write", "path,text", file_write },
    { "exists", "path", file_exists },
    { "remove", "path", file_remove },
    { "list", "dir", file_list },
  };
  return ANT_NAMESPACE("File", members);
}
