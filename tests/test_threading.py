"""Threading regressions that must hold on all builds (GIL and free-threaded).

These reproduce a hang that occurred when a lazily-built ``*Info`` object
failed to initialize while a previously-cached type still referenced it:
readers busy-waited forever on an ``initialized`` flag that would never be set.

Skipped on Emscripten/Pyodide, which cannot start threads.
"""

import threading
from dataclasses import dataclass
from typing import NamedTuple, TypedDict

import pytest

from structtype import Struct, StructAdapter, StructConfig

from .utils import requires_threads

pytestmark = requires_threads

# A valid annotation whose conversion fails deterministically on every supported
# Python version: structtype rejects unions with more than one dict-like type.
BadType = dict[str, int] | dict[int, str]

# Mutually-recursive types whose first build fails partway through. The failing
# field (`bad`) is declared *after* the recursive field so that the peer type is
# built and cached before the failure, capturing the partial info.
#
# These are module-level (not local to a test) so that the forward-reference
# string annotations resolve against the module globals.


class Recursive(Struct):
    peer: "RecursivePeer"
    bad: BadType


class RecursivePeer(Struct):
    back: Recursive


class RecursiveTD(TypedDict):
    peer: "RecursiveTDPeer"
    bad: BadType


class RecursiveTDPeer(TypedDict):
    back: RecursiveTD


@dataclass
class RecursiveDC:
    peer: "RecursiveDCPeer"
    bad: BadType


@dataclass
class RecursiveDCPeer:
    back: RecursiveDC


class RecursiveNT(NamedTuple):
    peer: "RecursiveNTPeer"
    bad: BadType


class RecursiveNTPeer(NamedTuple):
    back: RecursiveNT


def completes_within(fn, timeout=5.0):
    """Run `fn` in a daemon thread and report whether it finished in time.

    A regression that reintroduces the busy-wait makes this return False
    instead of hanging the test process forever.
    """
    finished = threading.Event()
    caught = []

    def worker():
        try:
            fn()
        except Exception as exc:  # noqa: BLE001
            caught.append(exc)
        finally:
            finished.set()

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    thread.join(timeout)
    return finished.is_set()


def test_recursive_struct_failure_does_not_hang():
    with pytest.raises(TypeError):
        Recursive.struct_validate_json(b"{}")

    assert completes_within(
        lambda: RecursivePeer.struct_validate_json(b'{"back":{}}')
    )
    with pytest.raises(RuntimeError):
        RecursivePeer.struct_validate_json(b'{"back":{}}')


def test_recursive_typeddict_failure_does_not_hang():
    adapter = StructAdapter(RecursiveTD)
    peer = StructAdapter(RecursiveTDPeer)

    with pytest.raises(TypeError):
        adapter.struct_validate_json(b"{}")

    assert completes_within(lambda: peer.struct_validate_json(b'{"back":{}}'))
    with pytest.raises(RuntimeError):
        peer.struct_validate_json(b'{"back":{}}')


def test_recursive_dataclass_failure_does_not_hang():
    adapter = StructAdapter(RecursiveDC)
    peer = StructAdapter(RecursiveDCPeer)

    with pytest.raises(TypeError):
        adapter.struct_validate_json(b"{}")

    assert completes_within(lambda: peer.struct_validate_json(b'{"back":{}}'))
    with pytest.raises(RuntimeError):
        peer.struct_validate_json(b'{"back":{}}')


def test_recursive_namedtuple_failure_does_not_hang():
    adapter = StructAdapter(RecursiveNT)
    peer = StructAdapter(RecursiveNTPeer)

    with pytest.raises(TypeError):
        adapter.struct_validate_json(b"[]")

    assert completes_within(lambda: peer.struct_validate_json(b"[[]]"))
    with pytest.raises(RuntimeError):
        peer.struct_validate_json(b"[[]]")


class FrozenHash(Struct):
    struct_config = StructConfig(frozen=True, cache_hash=True)

    x: int
    y: int


def test_concurrent_cached_hash_is_consistent():
    """Concurrent first-hash of a `cache_hash=True` instance must not corrupt
    the cached value or leak it (the store is an atomic compare-exchange)."""
    obj = FrozenHash(3, 4)
    expected = hash(obj)

    nthreads = 8
    barrier = threading.Barrier(nthreads)
    results = []
    errors = []
    lock = threading.Lock()

    def worker():
        try:
            barrier.wait()
            local = [hash(obj) for _ in range(2000)]
            with lock:
                results.extend(local)
        except BaseException as exc:  # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=worker) for _ in range(nthreads)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    assert errors == []
    assert set(results) == {expected}
