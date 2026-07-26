import pytest

pytest.importorskip("msgspec")

import msgspec
from structtype import Struct, StructAdapter


class MsgspecUser(msgspec.Struct):
    name: str
    age: int = 0


class MsgspecPoint(msgspec.Struct):
    x: float
    y: float


class MsgspecItem(msgspec.Struct, kw_only=True):
    a: int
    b: bool = False


class Container(Struct):
    owner: MsgspecUser
    items: list[int]


def test_encode_msgspec_field_nested():
    """Nested msgspec struct field in a struct — encoded via structtype encoder."""
    c = Container(owner=MsgspecUser(name="Alice", age=30), items=[1, 2])
    assert c.struct_dump_json() == b'{"owner":{"name":"Alice","age":30},"items":[1,2]}'


def test_encode_msgspec_direct():
    """Direct msgspec struct encode via StructAdapter."""
    u = MsgspecUser(name="Bob", age=25)
    assert StructAdapter(MsgspecUser).struct_dump_json(u) == b'{"name":"Bob","age":25}'


def test_decode_msgspec_field_nested():
    """Nested msgspec struct field decoded via structtype validator."""
    c = Container.struct_validate_json(
        b'{"owner":{"name":"Alice","age":30},"items":[1,2]}'
    )
    assert isinstance(c.owner, MsgspecUser)
    assert c.owner.name == "Alice"
    assert c.owner.age == 30
    assert c.items == [1, 2]


def test_decode_msgspec_direct():
    """Direct msgspec decode via StructAdapter."""
    u = StructAdapter(MsgspecUser).struct_validate_json(b'{"name":"Bob","age":25}')
    assert isinstance(u, MsgspecUser)
    assert u.name == "Bob"
    assert u.age == 25


def test_struct_to_dict_msgspec_field():
    """struct_dump converts msgspec struct fields to dicts."""
    c = Container(owner=MsgspecUser(name="Alice"), items=[])
    d = c.struct_dump()
    assert d == {"owner": {"name": "Alice", "age": 0}, "items": []}


def test_python_validate_msgspec():
    """Python→Python conversion works for msgspec types."""
    u = StructAdapter(MsgspecUser).struct_validate({"name": "Bob", "age": 30})
    assert isinstance(u, MsgspecUser)
    assert u.name == "Bob"
    assert u.age == 30


def test_msgspec_with_default():
    """msgspec struct with default field."""
    u = StructAdapter(MsgspecUser).struct_validate_json(b'{"name":"Alice"}')
    assert u.name == "Alice"
    assert u.age == 0


def test_msgspec_kw_only():
    """msgspec kw_only=True struct construction."""
    item = StructAdapter(MsgspecItem).struct_validate_json(b'{"a": 1, "b": true}')
    assert isinstance(item, MsgspecItem)
    assert item.a == 1
    assert item.b is True


def test_structadapter_msgspec():
    """StructAdapter works with msgspec types."""
    adapter = StructAdapter(MsgspecUser)
    u = adapter.struct_validate_json(b'{"name":"Alice","age":30}')
    assert isinstance(u, MsgspecUser)
    assert u.name == "Alice"
    assert u.age == 30
    assert adapter.struct_dump_json(u) == b'{"name":"Alice","age":30}'

    u2 = adapter.struct_validate({"name": "Bob"})
    assert u2.name == "Bob"
    assert u2.age == 0
    # StructAdapter.struct_dump falls through to return obj for msgspec types
    assert adapter.struct_dump(u2) is u2


def test_msgspec_roundtrip():
    """Full JSON roundtrip preserves msgspec data."""
    original = Container(owner=MsgspecUser(name="Alice", age=30), items=[10, 20, 30])
    buf = original.struct_dump_json()
    restored = Container.struct_validate_json(buf)
    assert restored.owner.name == original.owner.name
    assert restored.owner.age == original.owner.age
    assert restored.items == original.items
