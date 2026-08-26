import operator
import subprocess
import sys
import textwrap
import weakref

import pytest

import structtype

from .utils import requires_subprocess


def test_raw_noargs():
    r = structtype.Raw()
    assert bytes(r) == b""
    assert len(r) == 0
    assert not r


@pytest.mark.parametrize("typ", [bytes, bytearray, memoryview, str])
def test_raw_constructor(typ):
    msg = "test" if typ is str else typ(b"test")
    r = structtype.Raw(msg)
    assert bytes(r) == b"test"
    assert len(r) == 4
    assert r


def test_raw_constructor_errors():
    with pytest.raises(TypeError):
        structtype.Raw(1)

    with pytest.raises(TypeError):
        structtype.Raw(msg=b"test")

    with pytest.raises(TypeError):
        structtype.Raw(b"test", b"extra")


def test_raw_from_view():
    r = structtype.Raw(memoryview(b"123456")[:3])
    assert bytes(r) == b"123"
    assert len(r) == 3
    assert r


def test_raw_copy():
    r = structtype.Raw(b"test")
    c1 = sys.getrefcount(r)
    r2 = r.copy()
    c2 = sys.getrefcount(r)
    assert c1 + 1 == c2
    assert r2 is r

    r = structtype.Raw()
    assert r.copy() is r

    m = memoryview(b"test")
    ref = weakref.ref(m)
    r = structtype.Raw(m)
    del m
    # Raw holds a ref
    assert ref() is not None
    r2 = r.copy()
    # Actually copied
    assert r2 is not r
    assert bytes(r2) == b"test"
    # Copy doesn't accidentally release buffer
    assert ref() is not None
    del r
    # Copy doesn't hold a reference to original view
    assert ref() is None


@requires_subprocess
def test_raw_copy_doesnt_leak():
    """See https://github.com/tds333/structtype/pull/709"""
    script = textwrap.dedent(
        """
        import structtype
        import tracemalloc

        tracemalloc.start()

        raw = structtype.Raw(bytearray(1000))
        for _ in range(10000):
            raw.copy()

        _, peak = tracemalloc.get_traced_memory()
        print(peak)
        """
    )

    output = subprocess.check_output([sys.executable, "-c", script])
    peak = int(output.decode().strip())
    assert peak < 10_000  # should really be ~2000


def test_raw_pickle_bytes():
    orig_buffer = b"test"
    r = structtype.Raw(orig_buffer)
    o = r.__reduce__()
    assert o == (structtype.Raw, (b"test",))
    assert o[1][0] is orig_buffer


def test_raw_pickle_str():
    orig_buffer = "test"
    r = structtype.Raw(orig_buffer)
    o = r.__reduce__()
    assert o == (structtype.Raw, ("test",))
    assert o[1][0] is orig_buffer


def test_raw_pickle_view():
    r = structtype.Raw(memoryview(b"test")[:3])
    o = r.__reduce__()
    assert o == (structtype.Raw, (b"tes",))


def test_raw_comparison():
    r = structtype.Raw()
    assert r == r
    assert not r != r
    assert structtype.Raw() == structtype.Raw()
    assert structtype.Raw(b"") == structtype.Raw()
    assert not structtype.Raw(b"") == structtype.Raw(b"other")
    assert structtype.Raw(b"test") == structtype.Raw(memoryview(b"testy")[:4])
    assert structtype.Raw(b"test") != structtype.Raw(b"tesp")
    assert structtype.Raw(b"test") != structtype.Raw(b"")
    assert structtype.Raw(b"") != structtype.Raw(b"test")
    assert structtype.Raw() != 1
    assert 1 != structtype.Raw()

    for op in [operator.lt, operator.gt, operator.le, operator.ge]:
        with pytest.raises(TypeError):
            op(structtype.Raw(), structtype.Raw())
