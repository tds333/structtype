import structtype
from structtype import Field, Struct, StructAdapter
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
