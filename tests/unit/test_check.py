import datetime
from typing import Annotated

import pytest

from structtype import Struct, ValidationError, Field


class Point(Struct):
    x: int
    y: int


class Ranged(Struct):
    age: Annotated[int, Field(ge=0, le=150)]


class Named(Struct):
    name: Annotated[str, Field(min_length=1, max_length=100)]


class WithMultiple(Struct):
    val: Annotated[int, Field(multiple_of=2)]


class Timed(Struct):
    ts: Annotated[datetime.datetime, Field(tz=True)]


class Nested(Struct):
    inner: Point


# ── basic type validation ──


def test_valid_struct_passes():
    p = Point(1, 2)
    assert p.struct_validate_self() is None


def test_type_mismatch_raises():
    p = Point(1, 2)
    Struct.struct_force_setattr(p, "x", "hello")
    with pytest.raises(ValidationError, match="Expected `int`, got `str`"):
        p.struct_validate_self()


# ── constraint validation ──


def test_ge_constraint_violation():
    p = Ranged(-1)
    with pytest.raises(ValidationError):
        p.struct_validate_self()


def test_le_constraint_violation():
    p = Ranged(999)
    with pytest.raises(ValidationError):
        p.struct_validate_self()


def test_valid_constraint_passes():
    p = Ranged(50)
    assert p.struct_validate_self() is None


def test_multiple_of_violation():
    p = WithMultiple(3)
    with pytest.raises(ValidationError):
        p.struct_validate_self()


def test_valid_multiple_of():
    p = WithMultiple(4)
    assert p.struct_validate_self() is None


def test_min_length_violation():
    p = Named("")
    with pytest.raises(ValidationError):
        p.struct_validate_self()


def test_max_length_violation():
    p = Named("a" * 200)
    with pytest.raises(ValidationError):
        p.struct_validate_self()


def test_valid_length():
    p = Named("hello")
    assert p.struct_validate_self() is None


# ── nested structs ──


def test_valid_nested():
    p = Nested(inner=Point(1, 2))
    assert p.struct_validate_self() is None


def test_invalid_nested():
    p = Nested(inner=Point(1, 2))
    Struct.struct_force_setattr(p.inner, "x", "bad")
    with pytest.raises(ValidationError, match="Expected `int`, got `str`"):
        p.struct_validate_self()


# ── no mutation ──


def test_no_mutation():
    p = Point(1, 2)
    orig_x = p.x
    p.struct_validate_self()
    assert p.x == orig_x


# ── strict validation ──


def test_strict_rejects_mismatch():
    p = Point(1, 2)
    Struct.struct_force_setattr(p, "x", "123")
    with pytest.raises(ValidationError):
        p.struct_validate_self()


# ── error handling ──


def test_error_on_non_struct():
    with pytest.raises(TypeError, match="doesn't apply to"):
        Point.struct_validate_self("not a struct")


def test_extra_positional_args_raises():
    p = Point(1, 2)
    with pytest.raises(TypeError, match="takes no positional arguments"):
        p.struct_validate_self(99)


def test_bad_kwarg_raises():
    p = Point(1, 2)
    with pytest.raises(TypeError, match="unexpected keyword argument"):
        p.struct_validate_self(bad=True)


# ── frozen struct with force_setattr ──


def test_frozen_with_force_setattr():
    class Frozen(Struct, frozen=True):
        x: int

    f = Frozen(1)
    Struct.struct_force_setattr(f, "x", "bad")
    with pytest.raises(ValidationError):
        f.struct_validate_self()


def test_frozen_valid():
    class Frozen(Struct, frozen=True):
        x: int

    f = Frozen(1)
    assert f.struct_validate_self() is None
