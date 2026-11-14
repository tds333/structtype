import gc
import sys
import weakref
from dataclasses import dataclass
from typing import NamedTuple

import pytest

import structtype
from structtype import StructConfig

from .utils import temp_module


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


def test_non_struct_base_type_dict_not_leaked():
    class Base:
        pass

    type_dict = next(x for x in gc.get_referents(Base.__dict__) if isinstance(x, dict))
    gc.collect()
    base = sys.getrefcount(type_dict)
    for _ in range(50):
        A = type(
            "S",
            (Base, structtype.Struct),
            {"__annotations__": {"x": int}, "__module__": __name__},
        )
        del A
    gc.collect()
    assert sys.getrefcount(type_dict) == base


class _UnionLeakCustom:
    pass


class _UnionLeakPoint(structtype.Struct):
    x: int


@pytest.mark.parametrize(
    "member, other", [(_UnionLeakCustom, type(None)), (_UnionLeakPoint, int)]
)
def test_resolving_a_union_does_not_leak_its_args(member, other):
    from structtype._core import JSONDecoder

    union = member | other
    JSONDecoder(union)
    before = sys.getrefcount(member)
    for _ in range(10):
        JSONDecoder(member | other)
    assert sys.getrefcount(member) == before


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


def test_failed_recursive_typeddict_releases_field_types():
    source = """
    from typing import TypedDict

    class Custom:
        pass

    class A(TypedDict, total=False):
        first: Custom
        child: "B"
        bad: list | tuple

    class B(TypedDict):
        parent: A
    """
    with temp_module(source) as mod:
        ref = weakref.ref(mod.Custom)
        ta = structtype.StructAdapter(mod.A)
        with pytest.raises(TypeError, match="more than one array-like"):
            ta.struct_validate({})
        del ta
        del mod
    gc.collect()
    assert ref() is None


def test_nested_deep_check_does_not_leak():
    class Leaf(structtype.Struct):
        x: int

    @dataclass
    class Wrapper:
        leaf: Leaf

    class Pair(NamedTuple):
        leaf: Leaf

    class Holder(structtype.Struct):
        wrapper: Wrapper
        pair: Pair

    leaf = Leaf(1)
    holder = Holder(Wrapper(leaf), Pair(leaf))
    before = sys.getrefcount(leaf)
    assert holder.struct_check_types() is None
    assert sys.getrefcount(leaf) == before
    leaf.x = "bad"  # type: ignore[assignment]
    with pytest.raises(structtype.ValidationError):
        holder.struct_check_types()
    assert sys.getrefcount(leaf) == before
