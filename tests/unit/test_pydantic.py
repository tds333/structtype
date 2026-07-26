import datetime

import pytest

pytest.importorskip("pydantic")

from pydantic import BaseModel
from structtype import Struct, StructAdapter


class User(BaseModel):
    name: str
    age: int = 0


class Point(BaseModel):
    x: float
    y: float


class Container(Struct):
    owner: User
    items: list[int]


def test_encode_pydantic_field_nested():
    """Nested pydantic field in a struct — encoded via structtype encoder."""
    c = Container(owner=User(name="Alice", age=30), items=[1, 2])
    assert c.struct_dump_json() == b'{"owner":{"name":"Alice","age":30},"items":[1,2]}'


def test_encode_pydantic_direct():
    """Direct pydantic encode via StructAdapter."""
    u = User(name="Bob", age=25)
    assert StructAdapter(User).struct_dump_json(u) == b'{"name":"Bob","age":25}'


def test_decode_pydantic_field_nested():
    """Nested pydantic field decoded via structtype validator."""
    c = Container.struct_validate_json(
        b'{"owner":{"name":"Alice","age":30},"items":[1,2]}'
    )
    assert isinstance(c.owner, BaseModel)
    assert c.owner.name == "Alice"
    assert c.owner.age == 30
    assert c.items == [1, 2]


def test_decode_pydantic_direct():
    """Direct pydantic decode via StructAdapter."""
    u = StructAdapter(User).struct_validate_json(b'{"name":"Bob","age":25}')
    assert isinstance(u, BaseModel)
    assert u.name == "Bob"
    assert u.age == 25


def test_struct_to_dict_pydantic_field():
    """struct_dump converts pydantic fields to dicts."""
    c = Container(owner=User(name="Alice"), items=[])
    d = c.struct_dump()
    assert d == {"owner": {"name": "Alice", "age": 0}, "items": []}


def test_python_validate_pydantic():
    """Python→Python conversion works for pydantic types."""
    u = StructAdapter(User).struct_validate({"name": "Bob", "age": 30})
    assert isinstance(u, BaseModel)
    assert u.name == "Bob"
    assert u.age == 30


def test_pydantic_with_datetime():
    """Pydantic models with datetime fields."""
    class Event(BaseModel):
        name: str
        ts: datetime.datetime

    class Log(Struct):
        entries: list[Event]

    log = Log.struct_validate_json(
        b'{"entries":[{"name":"start","ts":"2024-01-15T12:00:00"}]}'
    )
    assert len(log.entries) == 1
    assert log.entries[0].name == "start"
    assert log.entries[0].ts == datetime.datetime(2024, 1, 15, 12, 0)


def test_pydantic_with_default():
    """Pydantic model with default field."""
    u = StructAdapter(User).struct_validate_json(b'{"name":"Alice"}')
    assert u.name == "Alice"
    assert u.age == 0  # default


def test_pydantic_with_alias():
    """Pydantic model with alias field."""
    from pydantic import Field

    class Aliased(BaseModel):
        full_name: str = Field(alias="name")
        age_years: int = Field(default=0, alias="age")

    class Wrapper(Struct):
        person: Aliased

    w = Wrapper.struct_validate_json(
        b'{"person":{"name":"Alice","age":30}}'
    )
    assert w.person.full_name == "Alice"
    assert w.person.age_years == 30


def test_structadapter_pydantic():
    """StructAdapter works with pydantic types."""
    adapter = StructAdapter(User)
    u = adapter.struct_validate_json(b'{"name":"Alice","age":30}')
    assert isinstance(u, BaseModel)
    assert u.name == "Alice"
    assert u.age == 30
    assert adapter.struct_dump_json(u) == b'{"name":"Alice","age":30}'

    u2 = adapter.struct_validate({"name": "Bob"})
    assert u2.name == "Bob"
    assert u2.age == 0
    assert adapter.struct_dump(u2) == {"name": "Bob", "age": 0}


def test_pydantic_roundtrip():
    """Full JSON roundtrip preserves pydantic data."""
    original = Container(owner=User(name="Alice", age=30), items=[10, 20, 30])
    buf = original.struct_dump_json()
    restored = Container.struct_validate_json(buf)
    assert restored.owner.name == original.owner.name
    assert restored.owner.age == original.owner.age
    assert restored.items == original.items
