import sys
import threading

import pytest

pytestmark = pytest.mark.skipif(
    not (hasattr(sys, "_is_gil_enabled") and not sys._is_gil_enabled()),
    reason="Requires a free-threaded Python build",
)

from structtype import Struct


class Point(Struct):
    x: int
    y: int


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
