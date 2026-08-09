import gc
import sys

import pytest

import structtype


def test_array_like_dump_doesnt_leak_field_refs():
    class P(structtype.Struct, array_like=True):
        x: int
        y: int

    p = P(x=123456789012345678901234567890, y=5)
    x = p.x
    before = sys.getrefcount(x)
    for _ in range(100):
        p.struct_dump()
    assert sys.getrefcount(x) == before


def test_struct_class_annotations_not_leaked():
    shared = {"x": int, "y": int}
    gc.collect()
    base = sys.getrefcount(shared)
    for _ in range(50):
        ns = {"__annotations__": shared, "__module__": __name__}
        A = type("S", (structtype.Struct,), ns)
        del A
        del ns
    gc.collect()
    assert sys.getrefcount(shared) == base


def test_encoder_reinit_releases_old_hook():
    from structtype._core import JSONEncoder

    h1, h2 = lambda x: x, lambda x: x
    enc = JSONEncoder(enc_hook=h1)
    base = sys.getrefcount(h1)
    enc.__init__(enc_hook=h2)
    assert sys.getrefcount(h1) == base - 1


def test_decoder_reinit_releases_old_hooks():
    from structtype._core import JSONDecoder

    d1 = lambda x: x
    dec = JSONDecoder(dec_hook=d1)
    base = sys.getrefcount(d1)
    dec.__init__(dec_hook=None)
    assert sys.getrefcount(d1) == base - 1


def test_encode_set_iterator_exception_propagates():
    from structtype._core import JSONEncoder

    class BadSet(set):
        def __iter__(self):
            yield 1
            raise RuntimeError("boom")

    with pytest.raises(RuntimeError):
        JSONEncoder().encode(BadSet([1]))


@pytest.mark.skipif(
    sys.version_info >= (3, 12), reason="interned strings are immortal on 3.12+"
)
def test_struct_validate_json_type_str_not_leaked():
    t = sys.intern("type")
    gc.collect()
    base = sys.getrefcount(t)

    class P(structtype.Struct):
        x: int

    for _ in range(200):
        P.struct_validate_json(b'{"x":1}')
    gc.collect()
    assert sys.getrefcount(t) == base
