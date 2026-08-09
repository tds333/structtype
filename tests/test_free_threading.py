import sys
import threading

import pytest

pytestmark = pytest.mark.skipif(
    not (hasattr(sys, "_is_gil_enabled") and not sys._is_gil_enabled()),
    reason="Requires a free-threaded Python build",
)

from dataclasses import dataclass
from typing import NamedTuple, Optional, TypedDict

from structtype import Struct


class Point(Struct):
    x: int
    y: int


class Tree(TypedDict):
    value: int
    left: Optional["Tree"]
    right: Optional["Tree"]


@dataclass
class DNode:
    value: int
    next: Optional["DNode"] = None


class NNode(NamedTuple):
    value: int
    next: Optional["NNode"] = None


def test_import_works():
    p = Point(1, 2)
    assert p.x == 1


def test_dump_json():
    data = Point(1, 2).struct_dump_json()
    assert data == b'{"x":1,"y":2}'


def test_validate_json():
    p = Point.struct_validate_json(b'{"x":3,"y":4}')
    assert p == Point(3, 4)


def test_concurrent_dump_json():
    def worker(num):
        for _ in range(10):
            result = Point(num, num).struct_dump_json()
            assert result == f'{{"x":{num},"y":{num}}}'.encode()

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


def test_concurrent_validate_json():
    data = b'{"x":1,"y":2}'

    def worker():
        for _ in range(100):
            result = Point.struct_validate_json(data)
            assert result == Point(1, 2)

    threads = [threading.Thread(target=worker) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


def test_concurrent_dict_decode():
    """Stress concurrent untyped dict decoding from multiple threads.

    Repeated keys exercise shared-string reuse while distinct keys force fresh
    allocations; this guards against cross-thread corruption in the decoder's
    shared module state."""
    from structtype import StructAdapter

    adapter = StructAdapter(dict)
    repeated = [b'{"shared_key": 1}', b'{"shared_key": 2}']
    distinct = [(f'{{"key{i:03d}": {i}}}').encode() for i in range(2000)]

    def worker(seed):
        for i in range(2000):
            if i % 2 == 0:
                adapter.struct_validate_json(repeated[i % 2])
            else:
                adapter.struct_validate_json(distinct[(i + seed) % len(distinct)])

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


def test_concurrent_sorted_encode_dict_mutation():
    """Stress the sorted-encode AssocList path while another thread mutates
    the dict. Captured keys/values must stay alive for the whole encode, so a
    concurrent mutation can't free them out from under the encoder."""
    import random

    from structtype import StructAdapter

    shared = {f"key{i:03d}": i for i in range(100)}
    adapter = StructAdapter(dict)
    stop = threading.Event()

    def encoder():
        while not stop.is_set():
            adapter.struct_dump_json(shared, sort_keys=True)

    def mutator():
        while not stop.is_set():
            key = f"key{random.randrange(100):03d}"
            shared[key] = random.randrange(1000)
            shared.pop(f"key{random.randrange(100):03d}", None)

    threads = [threading.Thread(target=encoder) for _ in range(4)] + [
        threading.Thread(target=mutator) for _ in range(4)
    ]
    for t in threads:
        t.start()
    for _ in range(20000):
        adapter.struct_dump_json(shared, sort_keys=True)
    stop.set()
    for t in threads:
        t.join()


def test_concurrent_self_referential_info_build():
    """Stress concurrent conversion of self-referential TypedDict/Dataclass/
    NamedTuple types. The info objects are cached before fields are built, so
    concurrent readers must wait for full initialization (no torn reads)."""
    from structtype import StructAdapter

    tree_adapter = StructAdapter(Tree)
    dnode_adapter = StructAdapter(DNode)
    nnode_adapter = StructAdapter(NNode)

    def worker():
        for _ in range(5000):
            tree_adapter.struct_validate_json(
                b'{"value":1,"left":{"value":2,"left":null,"right":null},'
                b'"right":{"value":3,"left":null,"right":null}}'
            )
            dnode_adapter.struct_validate_json(
                b'{"value":1,"next":{"value":2,"next":null}}'
            )
            nnode_adapter.struct_validate_json(b"[1,[2,[3]]]")

    threads = [threading.Thread(target=worker) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
