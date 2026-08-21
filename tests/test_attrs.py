"""Tests for attrs interop — validates structtype handles attrs classes
with validators, converters, and __attrs_post_init__ hooks."""

import pytest

attr = pytest.importorskip("attr")

import structtype
from structtype import Struct, StructAdapter


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
