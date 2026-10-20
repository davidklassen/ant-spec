"""The File namespace of stdlib.md: whole-file reads and writes, directory
listing, and handles for line-by-line and streamed I/O.

Every I/O failure is an `Err{ error: Error{ message } }`, never a panic
(stdlib.md: nothing here panics on bad input); only an argument of the wrong
kind panics `wrong kind`, as for any library function (section 12). Text is
UTF-8 both ways, strictly: a file that is not valid UTF-8 reads as an Err.

A handle is a reference value of kind "file": it compares by identity
(equal.py) and renders as its class name, `File` (render.py). The process
ends through os._exit (main's return, Os.exit, a panic), which skips
Python's buffered file flushing, so every write through a handle is flushed
at once; the whole-file functions close their file before returning.
"""
import functools
import io
import os

from antpy.values import VOID, KIND, need, need_string, need_bytes, byte_array, ok, err, error


class File:
    __slots__ = ("f", "path")

    def __init__(self, f, path):
        self.f = f            # the Python binary file object
        self.path = path      # for messages


KIND[File] = "file"

CHUNK = 1 << 16


def need_file(v):
    return need(v, File, "a File")


def failed(e, path):
    """The Err for the exception an I/O call on `path` raised: an OSError from
    the file system (its strerror), an io.UnsupportedOperation on a handle
    open the other way (its text is the operation's name, "read" or "write"),
    a ValueError on a closed handle, a UnicodeError on text that is not UTF-8."""
    if isinstance(e, UnicodeError):
        message = "invalid UTF-8"
    elif isinstance(e, io.UnsupportedOperation):
        message = "cannot " + str(e)
    elif isinstance(e, OSError) and e.strerror:
        message = e.strerror
    else:
        message = str(e) or type(e).__name__
    return err(error(message + ": " + path))


def io_result(fn):
    """Decorates a File native whose first argument names the file, as a path
    or a handle: the value it returns is the Ok, an I/O exception the Err
    against that path. The kind checks run inside and panic through, since
    AntPanic is no I/O exception."""
    @functools.wraps(fn)
    def wrapper(target, *args):
        try:
            return ok(fn(target, *args))
        except (OSError, ValueError) as e:
            return failed(e, target if type(target) is str else target.path)
    return wrapper


# ----------------------------------------------------------------------------- whole files

@io_result
def file_read(path):
    need_string(path)
    with open(path, "rb") as f:
        return f.read().decode("utf-8")


@io_result
def file_read_bytes(path):
    need_string(path)
    with open(path, "rb") as f:
        return byte_array(f.read())


@io_result
def file_write(path, text):
    need_string(path)
    need_string(text)
    with open(path, "wb") as f:
        f.write(text.encode("utf-8"))
    return VOID


@io_result
def file_write_bytes(path, bs):
    """The elements must be bytes (`uint8`), as for String.fromBytes."""
    need_string(path)
    data = need_bytes(bs)
    with open(path, "wb") as f:
        f.write(data)
    return VOID


def file_exists(path):
    return os.path.exists(need_string(path))     # a plain bool; false on any error


@io_result
def file_remove(path):
    need_string(path)
    os.remove(path)
    return VOID


@io_result
def file_list(dir):
    """The bare entry names, without `.` and `..`, in no particular order (G43)."""
    need_string(dir)
    return os.listdir(dir)


# ----------------------------------------------------------------------------- handles

@io_result
def file_open(path):
    """A handle for reading from the start of an existing file."""
    need_string(path)
    return File(open(path, "rb"), path)


@io_result
def file_create(path):
    """A handle for writing to a new or truncated file."""
    need_string(path)
    return File(open(path, "wb"), path)


@io_result
def file_read_line(f):
    """The next line without its `\\n`, or null at end of file (G44). A last
    line without a terminator is still a line; a `\\r` before the `\\n`
    stays in the text."""
    need_file(f)
    line = f.f.readline()
    if not line:
        return None
    if line.endswith(b"\n"):
        line = line[:-1]
    return line.decode("utf-8")


@io_result
def file_write_line(f, line):
    """Writes the line and a `\\n` (G44)."""
    need_file(f)
    need_string(line)
    f.f.write(line.encode("utf-8") + b"\n")
    f.f.flush()
    return VOID


def file_pipe(src, dst):
    """Copies everything from the source's current position to the end of
    the destination, in chunks, until the source is exhausted. A failure is
    reported against the side it happened on, so this does not use
    io_result (which names the first argument)."""
    need_file(src)
    need_file(dst)
    while True:
        try:
            chunk = src.f.read(CHUNK)
        except (OSError, ValueError) as e:
            return failed(e, src.path)
        try:
            if not chunk:
                dst.f.flush()
                return ok(VOID)
            dst.f.write(chunk)
        except (OSError, ValueError) as e:
            return failed(e, dst.path)


def file_close(f):
    """Closing a closed handle is a no-op; a failure to flush the last bytes
    has nowhere to go, since `close` returns void."""
    need_file(f)
    try:
        f.f.close()
    except OSError:
        pass
    return VOID


FILE = {
    "read": (["path"], file_read),
    "readBytes": (["path"], file_read_bytes),
    "write": (["path", "text"], file_write),
    "writeBytes": (["path", "bytes"], file_write_bytes),
    "exists": (["path"], file_exists),
    "remove": (["path"], file_remove),
    "list": (["dir"], file_list),
    "open": (["path"], file_open),
    "create": (["path"], file_create),
    "readLine": (["f"], file_read_line),
    "writeLine": (["f", "line"], file_write_line),
    "pipe": (["src", "dst"], file_pipe),
    "close": (["f"], file_close),
}
