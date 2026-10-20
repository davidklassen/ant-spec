"""The sync objects (section 9, "Synchronization"; section 11). Goroutines
are OS threads under the GIL, so each object is a thin wrapper over the
threading module's primitives.

Every sync object is a reference value: it compares by identity (equal.py
treats kind "sync" like a function) and renders as its class name, so the
class names below are exactly the namespace names of section 11.

"not locked" means the lock is free, not "not held by the caller": there is
no owner tracking, and one goroutine may unlock what another locked, as in Go.
The `with` forms are `with_held` below."""
import collections
import threading

from antpy.values import VOID, KIND, panic, need, need_int, need_func, need_number
from antpy.equal import eq
from antpy import arith


class Mutex:
    __slots__ = ("lock",)

    def __init__(self):
        self.lock = threading.Lock()


class RwMutex:
    """Readers share the lock; a writer excludes readers and writers (G51).
    `readers` and `writer` change only under `cond`."""
    __slots__ = ("cond", "readers", "writer")

    def __init__(self):
        self.cond = threading.Condition()
        self.readers = 0
        self.writer = False


class WaitGroup:
    __slots__ = ("count", "cond")

    def __init__(self):
        self.count = 0
        self.cond = threading.Condition()


class Once:
    """`lock` is held while `f` runs, so later callers wait for it."""
    __slots__ = ("lock", "done")

    def __init__(self):
        self.lock = threading.Lock()
        self.done = False


class Cond:
    """A condition variable on a Mutex. `waiters` holds one acquired Lock per
    waiting goroutine, in arrival order; a signal releases the oldest. Not a
    threading.Condition: its notify requires the lock, while section 11 lets
    `signal` and `broadcast` run anywhere."""
    __slots__ = ("mutex", "waiters", "guard")

    def __init__(self, mutex):
        self.mutex = mutex
        self.waiters = collections.deque()
        self.guard = threading.Lock()


class Atomic:
    __slots__ = ("value", "lock")

    def __init__(self, value):
        self.value = value
        self.lock = threading.Lock()


for cls in (Mutex, RwMutex, WaitGroup, Once, Cond, Atomic):
    KIND[cls] = "sync"


def need_sync(v, cls):
    article = "an" if cls is Atomic else "a"
    return need(v, cls, article + " " + cls.__name__)


def with_held(interp, frame, lock, unlock, m, f):
    """The shape of `withLock`, `withRead`, and `withWrite`: checks `f`, locks
    `m` through the namespace's own `lock` call (which checks `m`'s kind
    before anything is acquired), calls `f` with no argument, and unlocks in a
    `finally` through the namespace's own `unlock` call, so a panic in `f`
    propagates after the release."""
    need_func(f)
    lock(m)
    try:
        return interp.call(f, [], (), frame)
    finally:
        unlock(m)


# ----------------------------------------------------------------------------- Mutex

def mutex_lock(m):
    """Blocks until the lock is free; not reentrant (a goroutine that locks
    twice waits forever, as in Go). The GIL is released while it waits."""
    need_sync(m, Mutex)
    m.lock.acquire()
    return VOID


def mutex_unlock(m):
    need_sync(m, Mutex)
    try:
        m.lock.release()
    except RuntimeError:
        panic("not locked")
    return VOID


def mutex_with_lock(interp, frame, m, f):
    return with_held(interp, frame, mutex_lock, mutex_unlock, m, f)


MUTEX = {
    "new": ([], Mutex),
    "lock": (["m"], mutex_lock),
    "unlock": (["m"], mutex_unlock),
    "withLock": (["m", "f"], mutex_with_lock, True),
}


# ----------------------------------------------------------------------------- RwMutex

def rwmutex_read_lock(m):
    need_sync(m, RwMutex)
    with m.cond:
        while m.writer:
            m.cond.wait()
        m.readers += 1
    return VOID


def rwmutex_read_unlock(m):
    """`not locked` when no reader holds it, including when a writer does."""
    need_sync(m, RwMutex)
    with m.cond:
        if m.readers == 0:
            panic("not locked")
        m.readers -= 1
        if m.readers == 0:
            m.cond.notify_all()
    return VOID


def rwmutex_write_lock(m):
    """Waits for the writer and every reader to leave; no preference between
    a waiting writer and new readers."""
    need_sync(m, RwMutex)
    with m.cond:
        while m.writer or m.readers:
            m.cond.wait()
        m.writer = True
    return VOID


def rwmutex_write_unlock(m):
    need_sync(m, RwMutex)
    with m.cond:
        if not m.writer:
            panic("not locked")
        m.writer = False
        m.cond.notify_all()
    return VOID


def rwmutex_with_read(interp, frame, m, f):
    return with_held(interp, frame, rwmutex_read_lock, rwmutex_read_unlock, m, f)


def rwmutex_with_write(interp, frame, m, f):
    return with_held(interp, frame, rwmutex_write_lock, rwmutex_write_unlock, m, f)


RWMUTEX = {
    "new": ([], RwMutex),
    "readLock": (["m"], rwmutex_read_lock),
    "readUnlock": (["m"], rwmutex_read_unlock),
    "writeLock": (["m"], rwmutex_write_lock),
    "writeUnlock": (["m"], rwmutex_write_unlock),
    "withRead": (["m", "f"], rwmutex_with_read, True),
    "withWrite": (["m", "f"], rwmutex_with_write, True),
}


# ----------------------------------------------------------------------------- WaitGroup

def wait_group_add(wg, n):
    """A negative `n` is fine while the counter stays at or above zero."""
    need_sync(wg, WaitGroup)
    need_int(n)
    with wg.cond:
        if wg.count + n < 0:
            panic("negative counter")
        wg.count += n
        if wg.count == 0:
            wg.cond.notify_all()
    return VOID


def wait_group_done(wg):
    return wait_group_add(wg, -1)


def wait_group_wait(wg):
    need_sync(wg, WaitGroup)
    with wg.cond:
        while wg.count > 0:
            wg.cond.wait()
    return VOID


WAITGROUP = {
    "new": ([], WaitGroup),
    "add": (["wg", "n"], wait_group_add),
    "done": (["wg"], wait_group_done),
    "wait": (["wg"], wait_group_wait),
}


# ----------------------------------------------------------------------------- Once

def once_do(interp, frame, once, f):
    """Runs `f` under the lock the first time only, so a call that arrives
    while `f` runs returns after it finishes. A panicking `f` counts as done
    (G30); the panic propagates. `f`'s value is discarded."""
    need_sync(once, Once)
    need_func(f)
    with once.lock:
        if not once.done:
            try:
                interp.call(f, [], (), frame)
            finally:
                once.done = True
    return VOID


ONCE = {
    "new": ([], Once),
    "do": (["once", "f"], once_do, True),
}


# ----------------------------------------------------------------------------- Cond

def cond_new(m):
    need_sync(m, Mutex)
    return Cond(m)


def cond_wait(c):
    """Registers the waiter before releasing the mutex, so a signal sent
    between the two cannot be lost; reacquires the mutex before returning.
    There are no spurious wakeups, but callers loop on their condition anyway."""
    need_sync(c, Cond)
    lock = c.mutex.lock
    if not lock.locked():
        panic("not locked")
    waiter = threading.Lock()
    waiter.acquire()
    with c.guard:
        c.waiters.append(waiter)
    try:
        lock.release()
    except RuntimeError:                  # unlocked by another goroutine meanwhile: a data race
        with c.guard:
            if waiter in c.waiters:
                c.waiters.remove(waiter)
        panic("not locked")
    waiter.acquire()                      # blocks until a signal releases it
    lock.acquire()
    return VOID


def cond_signal(c):
    """Wakes the oldest waiter, if any. Never panics: the mutex need not be held."""
    need_sync(c, Cond)
    with c.guard:
        if c.waiters:
            c.waiters.popleft().release()
    return VOID


def cond_broadcast(c):
    need_sync(c, Cond)
    with c.guard:
        while c.waiters:
            c.waiters.popleft().release()
    return VOID


COND = {
    "new": (["m"], cond_new),
    "wait": (["c"], cond_wait),
    "signal": (["c"], cond_signal),
    "broadcast": (["c"], cond_broadcast),
}


# ----------------------------------------------------------------------------- Atomic

def atomic_load(a):
    need_sync(a, Atomic)
    with a.lock:
        return a.value


def atomic_store(a, v):
    need_sync(a, Atomic)
    with a.lock:
        a.value = v
    return VOID


def atomic_add(a, d):
    """Through the `+` operator, so an int64 overflow is `integer overflow`
    and a delta of another kind `kind mismatch`; returns the new value."""
    need_sync(a, Atomic)
    need_number(d)
    with a.lock:
        need_number(a.value)
        a.value = arith.binary("+", a.value, d)
        return a.value


def atomic_compare_and_swap(a, expected, v):
    """Structural `==` (G40): an equal but distinct record swaps."""
    need_sync(a, Atomic)
    with a.lock:
        if not eq(a.value, expected):
            return False
        a.value = v
        return True


ATOMIC = {
    "new": (["v"], Atomic),
    "load": (["a"], atomic_load),
    "store": (["a", "v"], atomic_store),
    "add": (["a", "d"], atomic_add),
    "compareAndSwap": (["a", "expected", "v"], atomic_compare_and_swap),
}

