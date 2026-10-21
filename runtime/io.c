/* print, panic, and the Os, Time, File and Http namespaces (stdlib.md);
 * Json is in json.c. Every failure of a File operation is an Err whose
 * message is `<reason>: <path>`; only an argument of the wrong kind panics,
 * and that check runs before any I/O. The whole-file functions close
 * their stream before returning; a handle flushes after every write,
 * since the process ends through _exit (ant_exit), which flushes nothing
 * but stdout and stderr. */
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

/* The Err `<what>: <path>`. */
static AntValue path_error(const char *what, const char *path) {
  return ant_err(ant_error(ant_fmt("%s: %s", what, path)));
}

/* The Err for the error code err of a call on path. */
static AntValue io_error(int err, const char *path) {
  return path_error(strerror(err), path);
}

/* ---------------------------------------------------------------- Os */

/* A fresh copy each time, so the caller's changes do not reach the next one. */
static AntValue os_args(AntFunc *self, int argc, AntValue *argv) {
  AntValue out = ant_array_new(0);
  ant_array_extend(out, ant_args);
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

void ant_ns_os(void) {
  static const AntMember members[] = {
    { "args", "", os_args },
    { "env", "name", os_env },
    { "exit", "status", os_exit },
    { "exec", "args", os_exec },
  };
  ANT_NAMESPACE("Os", members);
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

void ant_ns_time(void) {
  static const AntMember members[] = {
    { "now", "", time_now },
    { "sleep", "ms", time_sleep },
    { "monotonic", "", time_monotonic },
  };
  ANT_NAMESPACE("Time", members);
}

/* ---------------------------------------------------------------- File */

/* Reads the whole file at path into sb; on failure the Err is in *err. */
static bool read_file(const char *path, AntSb *sb, AntValue *err) {
  FILE *f = fopen(path, "rb");
  if (!f) { *err = io_error(errno, path); return false; }
  ant_sb_init(sb);
  char buf[1 << 16];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) ant_sb_append(sb, buf, (int64_t)n);
  bool failed = ferror(f);
  int code = errno;
  fclose(f);
  if (failed) *err = io_error(code, path);
  return !failed;
}

/* Makes the n bytes at p the content of the file at path. */
static AntValue write_file(const char *path, const char *p, int64_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return io_error(errno, path);
  bool failed = fwrite(p, 1, (size_t)n, f) != (size_t)n;
  if (fclose(f) != 0) failed = true;
  if (failed) return io_error(errno, path);
  return ant_ok(ANT_VOID);
}

static AntValue file_read(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], path);
  AntSb sb;
  AntValue err;
  if (!read_file(path->data, &sb, &err)) return err;
  if (!ant_utf8_valid(sb.data, sb.len)) return path_error("invalid UTF-8", path->data);
  return ant_ok(ant_sb_finish(&sb));
}

static AntValue file_read_bytes(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], path);
  AntSb sb;
  AntValue err;
  if (!read_file(path->data, &sb, &err)) return err;
  return ant_ok(ant_bytes_array(sb.data, sb.len));
}

static AntValue file_write(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], path);
  STR(argv[1], text);
  return write_file(path->data, text->data, text->nbytes);
}

static AntValue file_write_bytes(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], path);
  int64_t n;
  char *bytes = ant_bytes_of(argv[1], &n);
  if (!bytes) return ANT_VOID;
  return write_file(path->data, bytes, n);
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

/* A handle is a sync object (it renders as `File` and compares by
 * identity) around a libc stream, open for reading or for writing; fp is
 * NULL once closed, and every read or write on a closed handle is an Err.
 * A stream opened the other way fails its reads or writes with EBADF,
 * which the usual errno path reports. */
static const char FILE_NAME[] = "File";
typedef struct {
  AntSync base;
  FILE *fp;
  AntString *path;     /* kept for messages */
  char *line;          /* readLine's buffer, grown by getline, freed with the stream */
  size_t linecap;
} FileHandle;

static AntValue closed_error(FileHandle *h) {
  return path_error("file is closed", h->path->data);
}

static AntValue handle_open(AntString *path, const char *mode) {
  FILE *fp = fopen(path->data, mode);
  if (!fp) return io_error(errno, path->data);
  FileHandle *h = ant_alloc(sizeof(FileHandle));
  h->base.name = FILE_NAME;
  h->fp = fp;
  h->path = path;
  h->line = NULL;
  h->linecap = 0;
  return ant_ok(ant_ptr(K_SYNC, h));
}

static AntValue file_open(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], path);
  return handle_open(path, "rb");
}

static AntValue file_create(AntFunc *self, int argc, AntValue *argv) {
  STR(argv[0], path);
  return handle_open(path, "wb");
}

/* The next line without its `\n`, or null at end of file (G44): a last
 * line without a terminator is still a line, and a `\r` stays. */
static AntValue file_read_line(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], FileHandle, FILE_NAME, h);
  if (!h->fp) return closed_error(h);
  ssize_t n = getline(&h->line, &h->linecap, h->fp);
  if (n < 0) {
    if (ferror(h->fp)) return io_error(errno, h->path->data);
    return ant_ok(ANT_NULL);
  }
  if (n > 0 && h->line[n - 1] == '\n') n--;
  if (!ant_utf8_valid(h->line, n)) return path_error("invalid UTF-8", h->path->data);
  return ant_ok(ant_str_new(h->line, n));
}

/* Writes the line and a `\n` (G44), flushed at once. */
static AntValue file_write_line(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], FileHandle, FILE_NAME, h);
  STR(argv[1], line);
  if (!h->fp) return closed_error(h);
  bool failed = fwrite(line->data, 1, (size_t)line->nbytes, h->fp) != (size_t)line->nbytes ||
                fputc('\n', h->fp) == EOF || fflush(h->fp) != 0;
  if (failed) return io_error(errno, h->path->data);
  return ant_ok(ANT_VOID);
}

/* Copies from the source's position to its end onto the destination; a
 * failure is reported against the side it happened on. */
static AntValue file_pipe(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], FileHandle, FILE_NAME, src);
  SYNC(argv[1], FileHandle, FILE_NAME, dst);
  if (!src->fp) return closed_error(src);
  if (!dst->fp) return closed_error(dst);
  char buf[1 << 16];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, src->fp)) > 0) {
    if (fwrite(buf, 1, n, dst->fp) != n) return io_error(errno, dst->path->data);
  }
  if (ferror(src->fp)) return io_error(errno, src->path->data);
  if (fflush(dst->fp) != 0) return io_error(errno, dst->path->data);
  return ant_ok(ANT_VOID);
}

/* Closing twice is a no-op; a failure to write the last bytes has nowhere to go. */
static AntValue file_close(AntFunc *self, int argc, AntValue *argv) {
  SYNC(argv[0], FileHandle, FILE_NAME, h);
  if (h->fp) {
    fclose(h->fp);
    h->fp = NULL;
    free(h->line);
    h->line = NULL;
  }
  return ANT_VOID;
}

void ant_ns_file(void) {
  static const AntMember members[] = {
    { "read", "path", file_read },
    { "readBytes", "path", file_read_bytes },
    { "write", "path,text", file_write },
    { "writeBytes", "path,bytes", file_write_bytes },
    { "exists", "path", file_exists },
    { "remove", "path", file_remove },
    { "list", "dir", file_list },
    { "open", "path", file_open },
    { "create", "path", file_create },
    { "readLine", "f", file_read_line },
    { "writeLine", "f,line", file_write_line },
    { "pipe", "src,dst", file_pipe },
    { "close", "f", file_close },
  };
  ANT_NAMESPACE("File", members);
}

/* ---------------------------------------------------------------- Http */

/* Not implemented (design section 5): the arguments are checked and every
 * request is an Err. */
static AntValue http_unsupported(void) {
  return ant_err(ant_error("Http is not supported"));
}

static AntValue http_get(AntFunc *self, int argc, AntValue *argv) {
  if (!ant_str(argv[0])) return ANT_VOID;
  return http_unsupported();
}

static AntValue http_post(AntFunc *self, int argc, AntValue *argv) {
  if (!ant_str(argv[0]) || !ant_str(argv[1]) || !ant_need_map(argv[2])) return ANT_VOID;
  return http_unsupported();
}

void ant_ns_http(void) {
  static const AntMember members[] = {
    { "get", "url", http_get },
    { "post", "url,body,headers", http_post },
  };
  ANT_NAMESPACE("Http", members);
}
