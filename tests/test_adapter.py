import datetime
import decimal
import enum
import uuid

import structtype
from structtype import ALL_BUILTIN_TYPES, Field, Struct, StructAdapter
from typing import Annotated


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
