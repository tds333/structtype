"""Tests for attrs interop — validates structtype handles attrs classes
with validators, converters, and __attrs_post_init__ hooks."""

import pytest

attr = pytest.importorskip("attr")

import structtype
from structtype import Struct, StructAdapter
from structtype._core import _json_decode

from .utils import unmaterialized_str


# ------------------------------------------------------------------
# attrs validator + __attrs_post_init__ runner
# ------------------------------------------------------------------


@attr.define
class _AV:
    x: int = attr.field(validator=attr.validators.gt(0))

    def __attrs_post_init__(self):
        self.x = self.x


class _SAV(Struct):
    a: _AV


def test_attrs_validator_postinit_ok():
    result = _SAV.struct_validate({"a": {"x": 5}})
    assert result.a.x == 5


def test_attrs_validator_postinit_bad():
    with pytest.raises(structtype.ValidationError):
        _SAV.struct_validate({"a": {"x": -1}})


# ------------------------------------------------------------------
# attrs Factory(takes_self=True) raises NotImplementedError
# ------------------------------------------------------------------


@attr.define
class _ASelf:
    x: int = 0
    y: list = attr.Factory(lambda self: [self.x], takes_self=True)


def test_attrs_takes_self_raises():
    with pytest.raises(NotImplementedError, match="takes_self=True"):
        StructAdapter(_ASelf).struct_validate({"x": 3})


# ------------------------------------------------------------------
# attrs encoding (`dump_object` / `json_encode_object` paths)
# ------------------------------------------------------------------


def test_attrs_dump_roundtrip():
    obj = _SAV(_AV(5))
    assert obj.struct_dump_json() == b'{"a":{"x":5}}'
    assert obj.struct_dump() == {"a": {"x": 5}}
    assert _SAV.struct_validate_json(obj.struct_dump_json()) == obj


def test_attrs_adapter_dump():
    ta = StructAdapter(_AV)
    assert ta.struct_dump_json(_AV(3)) == b'{"x":3}'
    assert ta.struct_dump(_AV(3)) == {"x": 3}


@attr.define
class _AMulti:
    x: int
    y: str = "d"


class _SMulti(Struct):
    a: _AMulti


def test_attrs_multi_field_dump():
    import json as _json

    obj = _SMulti.struct_validate({"a": {"x": 5}})
    assert obj.a == _AMulti(5, "d")
    assert _json.loads(obj.struct_dump_json()) == {"a": {"x": 5, "y": "d"}}
    assert obj.struct_dump() == {"a": {"x": 5, "y": "d"}}


def test_attrs_nonascii_field_name_decodes_json():
    key = unmaterialized_str("fïeld")
    A = attr.make_class("A", {key: attr.ib(type=int)})
    out = _json_decode(b'{"f\xc3\xafeld": 1}', type=A)
    assert getattr(out, key) == 1


def test_attrs_object_dict_keys_are_escaped():
    import json

    from structtype._core import _json_encode

    class A:
        __attrs_attrs__ = ()

        def __init__(self):
            object.__setattr__(self, 'a"b', 1)
            object.__setattr__(self, "c\nd", 2)
            object.__setattr__(self, "tab\tkey", 3)
            object.__setattr__(self, "back\\slash", 4)
            object.__setattr__(self, "nönascii", 5)
            object.__setattr__(self, "_private", 6)

    out = _json_encode(A())
    assert json.loads(out) == {
        'a"b': 1,
        "c\nd": 2,
        "tab\tkey": 3,
        "back\\slash": 4,
        "nönascii": 5,
    }
