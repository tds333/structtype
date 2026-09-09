import gc
import sys

import pytest

import structtype
from structtype import StructConfig


def test_array_like_dump_doesnt_leak_field_refs():
    class P(structtype.Struct):
        struct_config = StructConfig(array_like=True)
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


def test_codec_serializer_freed_with_decoder():
    import datetime

    from typing import Annotated

    from structtype import Serializer
    from structtype._core import JSONDecoder

    def dump(d):
        return int(d.timestamp())

    def load(v):
        return datetime.datetime.fromtimestamp(v, datetime.timezone.utc)

    # Keep the serializer in a heap-held container so its refcount is stable
    # under free-threaded builds (stack-only references may not be counted).
    ser = Serializer(dump=dump, load=load)
    keep = [ser]
    # Build the annotation first: typing's alias cache holds the serializer,
    # so those references must be part of the baseline.
    ann = Annotated[datetime.datetime, ser]
    gc.collect()
    base = sys.getrefcount(ser)

    decoder = JSONDecoder(ann)
    gc.collect()
    # The decoder's TypeNode holds exactly one extra reference (the codec
    # detail slot); it must be released when the decoder is freed.
    del decoder
    gc.collect()
    assert sys.getrefcount(ser) == base


def test_codec_decode_does_not_leak_values():
    import datetime

    from typing import Annotated

    from structtype import Serializer
    from structtype._core import JSONDecoder

    def dump(d):
        return int(d.timestamp())

    def load(v):
        return datetime.datetime.fromtimestamp(v, datetime.timezone.utc)

    # Element-level codec inside a container: exercises both the container
    # node and a codec'd element node in the TypeNode tree.
    decoder = JSONDecoder(
        set[Annotated[datetime.datetime, Serializer(dump=dump, load=load)]]
    )

    gc.collect()
    out = decoder.decode("[1767225600]")
    assert out == {
        datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc)
    }
    for _ in range(200):
        decoder.decode("[1767225600]")
    gc.collect()


def test_codec_encode_does_not_leak_values():
    import datetime
    from typing import Annotated

    import structtype
    from structtype import Serializer

    def dump(d):
        return int(d.timestamp())

    def load(v):
        return datetime.datetime.fromtimestamp(v, datetime.timezone.utc)

    ann = Annotated[datetime.datetime, Serializer(dump=dump, load=load)]
    ns = {"__annotations__": {"d": ann}, "__module__": __name__}
    Msg = type("Msg", (structtype.Struct,), ns)

    msg = Msg(datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc))
    gc.collect()
    for _ in range(200):
        buf = msg.struct_dump_json()
        assert Msg.struct_validate_json(buf) == msg
    gc.collect()
