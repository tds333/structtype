import datetime
import decimal
import enum
import sys
import uuid

import pytest

import structtype
from structtype import ALL_BUILTIN_TYPES, Field, Struct, StructAdapter
from typing import Annotated, Final, NewType


def test_validate_json_simple():
    ta = StructAdapter(int)
    assert ta.struct_validate_json(b"42") == 42


def test_validate_json_generic():
    ta = StructAdapter(list[int])
    assert ta.struct_validate_json(b"[1, 2, 3]") == [1, 2, 3]


def test_validate_json_constrained():
    ta = StructAdapter(Annotated[int, Field(ge=0)])
    assert ta.struct_validate_json(b"42") == 42


def test_validate_json_tagged_union():
    class Cat(Struct, tag="cat", tag_field="type"):
        meow: str

    class Dog(Struct, tag="dog", tag_field="type"):
        bark: str

    ta = StructAdapter(Cat | Dog)
    cat = ta.struct_validate_json(b'{"type":"cat","meow":"hello"}')
    assert isinstance(cat, Cat)
    assert cat.meow == "hello"

    dog = ta.struct_validate_json(b'{"type":"dog","bark":"woof"}')
    assert isinstance(dog, Dog)
    assert dog.bark == "woof"


def test_dump_json():
    ta = StructAdapter(list[int])
    assert ta.struct_dump_json([1, 2, 3]) == b"[1,2,3]"


def test_dump_json_tagged():
    class Cat(Struct, tag="cat", tag_field="type"):
        meow: str

    ta = StructAdapter(Cat)
    assert ta.struct_dump_json(Cat(meow="hi")) == b'{"type":"cat","meow":"hi"}'


def test_validate_python_struct():
    class Point(Struct):
        x: int
        y: int

    ta = StructAdapter(Point)
    result = ta.struct_validate({"x": 1, "y": 2})
    assert isinstance(result, Point)
    assert result.x == 1
    assert result.y == 2


def test_validate_python_generic():
    ta = StructAdapter(list[int])
    assert ta.struct_validate([1, 2, 3]) == [1, 2, 3]


def test_validate_python_constrained():
    ta = StructAdapter(Annotated[int, Field(ge=0)])
    assert ta.struct_validate(5) == 5


def test_dump_python_struct():
    class Point(Struct):
        x: int
        y: int

    ta = StructAdapter(Point)
    result = ta.struct_dump(Point(x=1, y=2))
    assert result == {"x": 1, "y": 2}


def test_dump_python_non_struct():
    ta = StructAdapter(int)
    assert ta.struct_dump(42) == 42


def test_dump_builtin_types():
    """struct_dump with builtin_types=ALL_BUILTIN_TYPES preserves native types."""

    class Color(enum.Enum):
        RED = 1
        GREEN = 2

    class Obj(Struct):
        ts: datetime.datetime
        d: datetime.date
        t: datetime.time
        td: datetime.timedelta
        b: bytes
        u: uuid.UUID
        dc: decimal.Decimal
        color: Color

    obj = Obj(
        ts=datetime.datetime(2024, 1, 15, 10, 30),
        d=datetime.date(2024, 1, 15),
        t=datetime.time(10, 30),
        td=datetime.timedelta(days=1),
        b=b"hello",
        u=uuid.UUID("12345678-1234-5678-1234-567812345678"),
        dc=decimal.Decimal("3.14"),
        color=Color.RED,
    )

    # Default: JSON-compatible (all converted to strings/basic types)
    r = obj.struct_dump()
    assert isinstance(r["ts"], str)
    assert isinstance(r["d"], str)
    assert isinstance(r["t"], str)
    assert isinstance(r["td"], str)
    assert isinstance(r["b"], str)
    assert isinstance(r["u"], str)
    assert isinstance(r["dc"], str)
    assert isinstance(r["color"], int)

    # ALL_BUILTIN_TYPES: native types preserved
    r = obj.struct_dump(builtin_types=ALL_BUILTIN_TYPES)
    assert isinstance(r["ts"], datetime.datetime)
    assert isinstance(r["d"], datetime.date)
    assert isinstance(r["t"], datetime.time)
    assert isinstance(r["td"], datetime.timedelta)
    assert isinstance(r["b"], bytes)
    assert isinstance(r["u"], uuid.UUID)
    assert isinstance(r["dc"], decimal.Decimal)
    assert isinstance(r["color"], Color)

    # Selective passthrough: only Color
    r = obj.struct_dump(builtin_types=[Color])
    assert isinstance(r["ts"], str)
    assert isinstance(r["b"], str)
    assert isinstance(r["color"], Color)


def test_json_schema():
    assert structtype.json_schema(int) == {"type": "integer"}


def test_json_schema_constrained():
    assert structtype.json_schema(Annotated[int, Field(ge=0)]) == {
        "type": "integer",
        "minimum": 0,
    }


def test_json_schema_ref_template():
    result = structtype.json_schema(int, ref_template="#/components/{name}")
    assert result == {"type": "integer"}


def test_roundtrip_json():
    class Cat(Struct, tag="cat", tag_field="type"):
        meow: str
    class Dog(Struct, tag="dog", tag_field="type"):
        bark: str

    ta = StructAdapter(Cat | Dog)
    data = b'{"type":"dog","bark":"woof"}'
    assert ta.struct_dump_json(ta.struct_validate_json(data)) == data


def test_roundtrip_python():
    class Point(Struct):
        x: int
        y: int

    ta = StructAdapter(Point)
    obj = ta.struct_validate({"x": 1, "y": 2})
    assert ta.struct_dump(obj) == {"x": 1, "y": 2}


def test_dump_builtin_types_not_iterable():
    class Obj(Struct):
        x: int

    obj = Obj(x=1)
    with pytest.raises(TypeError, match="builtin_types must be an iterable"):
        obj.struct_dump(builtin_types=42)


def test_dump_builtin_types_non_type():
    class Obj(Struct):
        x: int

    obj = Obj(x=1)
    with pytest.raises(TypeError, match="builtin_types must be an iterable"):
        obj.struct_dump(builtin_types=[42])


def test_dump_builtin_types_unsupported():
    class Obj(Struct):
        x: int

    obj = Obj(x=1)
    with pytest.raises(TypeError, match="must be an iterable of types"):
        obj.struct_dump(builtin_types=42)


def test_dump_builtin_types_empty():
    class Obj(Struct):
        ts: datetime.datetime

    obj = Obj(ts=datetime.datetime(2024, 1, 15))
    r = obj.struct_dump(builtin_types=[])
    assert isinstance(r["ts"], str)


def test_dump_builtin_types_bytes_only():
    class Obj(Struct):
        b: bytes
        ts: datetime.datetime

    obj = Obj(b=b"hello", ts=datetime.datetime(2024, 1, 15))
    r = obj.struct_dump(builtin_types=[bytes])
    assert isinstance(r["b"], bytes)
    assert isinstance(r["ts"], str)


def test_dump_adapter_builtin_types():
    class Color(enum.Enum):
        RED = 1

    class Obj(Struct):
        color: Color

    obj = Obj(color=Color.RED)
    ta = StructAdapter(Obj)
    r = ta.struct_dump(obj, builtin_types=[Color])
    assert isinstance(r["color"], Color)


def test_dump_builtin_types_nested():
    class Inner(Struct):
        ts: datetime.datetime

    class Outer(Struct):
        inner: Inner

    obj = Outer(inner=Inner(ts=datetime.datetime(2024, 1, 15)))
    r = obj.struct_dump(builtin_types=ALL_BUILTIN_TYPES)
    assert isinstance(r["inner"], dict)
    assert isinstance(r["inner"]["ts"], datetime.datetime)


def test_adapter_rejects_codec_annotation():
    def dump(c):
        return (c.real, c.imag)

    def validate(obj):
        return complex(obj[0], obj[1])

    with pytest.raises(TypeError, match="not supported on StructAdapter"):
        StructAdapter(Annotated[complex, Field(dump=dump)])
    with pytest.raises(TypeError, match="not supported on StructAdapter"):
        StructAdapter(Annotated[complex, Field(validate=validate)])
    with pytest.raises(TypeError, match="not supported on StructAdapter"):
        StructAdapter(list[Annotated[complex, Field(dump=dump, validate=validate)]])


def test_adapter_rejects_newtype_wrapped_codec():
    def dump(c):
        return (c.real, c.imag)

    T = NewType("T", Annotated[complex, Field(dump=dump)])
    with pytest.raises(TypeError, match="not supported on StructAdapter"):
        StructAdapter(T)


def test_adapter_rejects_final_wrapped_codec():
    def dump(c):
        return (c.real, c.imag)

    with pytest.raises(TypeError, match="not supported on StructAdapter"):
        StructAdapter(Final[Annotated[complex, Field(dump=dump)]])


def test_adapter_rejects_type_alias_wrapped_codec():
    if sys.version_info < (3, 12):
        return
    from typing import TypeAliasType

    def dump(c):
        return (c.real, c.imag)

    def validate(obj):
        return complex(obj[0], obj[1])

    C = TypeAliasType("C", Annotated[complex, Field(dump=dump, validate=validate)])
    with pytest.raises(TypeError, match="not supported on StructAdapter"):
        StructAdapter(C)


def test_adapter_newtype_constraint_only_accepted():
    NT = NewType("NT", Annotated[int, Field(gt=0)])
    ta = StructAdapter(NT)
    assert ta.struct_validate_json(b"42") == 42


def test_adapter_rejects_hooks():
    ta = StructAdapter(int)
    with pytest.raises(TypeError):
        ta.struct_validate_json(b"1", dec_hook=str)
    with pytest.raises(TypeError):
        ta.struct_dump_json(1, enc_hook=str)


def test_adapter_protocol_roundtrip():
    class Point:
        def __init__(self, x, y):
            self.x = x
            self.y = y

        def __eq__(self, other):
            return (self.x, self.y) == (other.x, other.y)

        def struct_dump(self):
            return {"x": self.x, "y": self.y}

        @classmethod
        def struct_validate(cls, obj):
            return cls(obj["x"], obj["y"])

    ta = StructAdapter(Point)
    buf = ta.struct_dump_json(Point(1, 2))
    assert buf == b'{"x":1,"y":2}'
    assert ta.struct_validate_json(buf) == Point(1, 2)
    assert ta.struct_dump(Point(3, 4)) == {"x": 3, "y": 4}
    assert ta.struct_validate({"x": 3, "y": 4}) == Point(3, 4)


# ------------------------------------------------------------------
# Coverage: dataclass with InitVar field → TypeError
# ------------------------------------------------------------------
from dataclasses import InitVar, dataclass as _dc


@_dc
class _DCInit:
    x: int
    post: InitVar[int] = 0


def test_initvar_rejected():
    class _SInit(Struct):
        d: _DCInit

    with pytest.raises(TypeError, match="InitVar"):
        _SInit.struct_validate({"d": {"x": 1}})


# ------------------------------------------------------------------
# Coverage: dataclass inheriting parametrised Mapping generic
# (_utils.py:120 — builtin-generic scope branch)
# ------------------------------------------------------------------
from collections.abc import Mapping
from typing import Generic, TypeVar

_T = TypeVar("T")


class _MBase(Mapping[str, _T], Generic[_T]):
    def __init__(self, d=None):
        self._d = dict(d or {})

    def __getitem__(self, k):
        return self._d[k]

    def __iter__(self):
        return iter(self._d)

    def __len__(self):
        return len(self._d)


@_dc
class _DCM(_MBase[int]):
    extra: int = 0


def test_dc_inherits_mapping_generic():
    from structtype._utils import get_class_annotations

    hints = get_class_annotations(_DCM)
    assert "extra" in hints


# ------------------------------------------------------------------
# Coverage: TypedDict with Required/NotRequired via typing_extensions
# (_utils.py:244-249)
# ------------------------------------------------------------------


def _make_td_classes(te):
    class TDWrappers(te.TypedDict):
        a: int
        b: te.NotRequired[str]

    class TDRRequired(te.TypedDict, total=False):
        x: float
        y: te.Required[int]

    return TDWrappers, TDRRequired


def test_typeddict_notrequired():
    te = pytest.importorskip("typing_extensions")
    TDWrappers, _ = _make_td_classes(te)

    class STDW(Struct):
        t: TDWrappers

    result = STDW.struct_validate({"t": {"a": 1}})
    assert result.t == {"a": 1}


def test_typeddict_required_wrapper():
    te = pytest.importorskip("typing_extensions")
    _, TDRRequired = _make_td_classes(te)

    class STDR(Struct):
        t: TDRRequired

    result = STDR.struct_validate({"t": {"y": 2}})
    assert result.t == {"y": 2}


def test_typeddict_total_false_info():
    from structtype._inspect import type_info

    te = pytest.importorskip("typing_extensions")
    _, TDRRequired = _make_td_classes(te)

    ti = type_info(TDRRequired)
    assert "x" in {f.name for f in ti.fields}
    assert "y" in {f.name for f in ti.fields}
