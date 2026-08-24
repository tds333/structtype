import datetime
from typing import Annotated

import pytest

from structtype import (
    Struct,
    StructConfig,
    ValidationError,
    Constraint,
    NumericConstraint,
    StrConstraint,
    TimezoneConstraint,
)


class Point(Struct):
    x: int
    y: int


class Ranged(Struct):
    age: Annotated[int, NumericConstraint(ge=0, le=150)]


class Named(Struct):
    name: Annotated[str, StrConstraint(min_length=1, max_length=100)]


class WithMultiple(Struct):
    val: Annotated[int, NumericConstraint(multiple_of=2)]


class Timed(Struct):
    ts: Annotated[datetime.datetime, TimezoneConstraint(tz=True)]


class Nested(Struct):
    inner: Point


class ListNested(Struct):
    items: list[Point]


class DictNested(Struct):
    mapping: dict[str, Point]


class TupleNested(Struct):
    items: tuple[Point, ...]


class SetNested(Struct):
    items: set[Point]


class FrozenPoint(Struct):
    struct_config = StructConfig(frozen=True)
    x: int
    y: int


class FrozenSetNested(Struct):
    items: set[FrozenPoint]


class FrozenFrozensetNested(Struct):
    items: frozenset[FrozenPoint]


class OptListNested(Struct):
    items: list[Point | None]


class ListNestedInit(Struct):
    struct_config = StructConfig(check_types_on_init=True)
    items: list[Point]


# ── basic type validation ──


def test_valid_struct_passes():
    p = Point(1, 2)
    assert p.struct_check_types() is None


def test_type_mismatch_raises():
    p = Point(1, 2)
    p.x = "hello"
    with pytest.raises(ValidationError, match="Expected `int`, got `str`"):
        p.struct_check_types()


# ── constraint validation ──


def test_ge_constraint_violation():
    p = Ranged(-1)
    with pytest.raises(ValidationError):
        p.struct_check_types()


def test_le_constraint_violation():
    p = Ranged(999)
    with pytest.raises(ValidationError):
        p.struct_check_types()


def test_valid_constraint_passes():
    p = Ranged(50)
    assert p.struct_check_types() is None


def test_multiple_of_violation():
    p = WithMultiple(3)
    with pytest.raises(ValidationError):
        p.struct_check_types()


def test_valid_multiple_of():
    p = WithMultiple(4)
    assert p.struct_check_types() is None


def test_min_length_violation():
    p = Named("")
    with pytest.raises(ValidationError):
        p.struct_check_types()


def test_max_length_violation():
    p = Named("a" * 200)
    with pytest.raises(ValidationError):
        p.struct_check_types()


def test_valid_length():
    p = Named("hello")
    assert p.struct_check_types() is None


# ── nested structs ──


def test_valid_nested():
    p = Nested(inner=Point(1, 2))
    assert p.struct_check_types() is None


def test_invalid_nested():
    p = Nested(inner=Point(1, 2))
    p.inner.x = "bad"
    with pytest.raises(ValidationError, match="Expected `int`, got `str`"):
        p.struct_check_types()


# ── nested structs inside containers ──


def test_valid_nested_in_list():
    p = ListNested(items=[Point(1, 2), Point(3, 4)])
    assert p.struct_check_types() is None


def test_invalid_nested_in_list():
    p = ListNested(items=[Point(1, 2), Point("bad", 3)])
    with pytest.raises(ValidationError, match=r"\$\.items\[1\]\.x"):
        p.struct_check_types()


def test_valid_nested_in_dict():
    p = DictNested(mapping={"a": Point(1, 2)})
    assert p.struct_check_types() is None


def test_invalid_nested_in_dict():
    p = DictNested(mapping={"a": Point("bad", 2)})
    with pytest.raises(ValidationError, match=r"\$\.mapping\[.a.\]\.x"):
        p.struct_check_types()


def test_valid_nested_in_tuple():
    p = TupleNested(items=(Point(1, 2),))
    assert p.struct_check_types() is None


def test_invalid_nested_in_tuple():
    p = TupleNested(items=(Point("bad", 2),))
    with pytest.raises(ValidationError, match=r"\$\.items\[0\]\.x"):
        p.struct_check_types()


def test_invalid_nested_in_set():
    p = FrozenSetNested(items={FrozenPoint("bad", 2)})
    with pytest.raises(ValidationError, match=r"\$\.items\[0\]\.x"):
        p.struct_check_types()


def test_invalid_nested_in_frozenset():
    p = FrozenFrozensetNested(items=frozenset({FrozenPoint("bad", 2)}))
    with pytest.raises(ValidationError, match=r"\$\.items\[0\]\.x"):
        p.struct_check_types()


def test_none_in_optional_list_passes():
    p = OptListNested(items=[None, Point(1, 2)])
    assert p.struct_check_types() is None


def test_invalid_nested_in_optional_list():
    p = OptListNested(items=[None, Point("bad", 2)])
    with pytest.raises(ValidationError, match=r"\$\.items\[1\]\.x"):
        p.struct_check_types()


def test_invalid_nested_check_types_on_init_list():
    with pytest.raises(ValidationError, match=r"\$\.items\[0\]\.x"):
        ListNestedInit(items=[Point("bad", 2)])


def test_nested_item_user_validator_called_once():
    calls = []

    def check(p):
        calls.append(p.x)
        return p

    class VList(Struct):
        items: list[Annotated[Point, Constraint(check)]]

    v = VList(items=[Point(1, 2), Point(3, 4)])
    v.struct_check_types()
    assert calls == [1, 3]


# ── no mutation ──


def test_no_mutation():
    p = Point(1, 2)
    orig_x = p.x
    p.struct_check_types()
    assert p.x == orig_x


# ── strict validation ──


def test_strict_rejects_mismatch():
    p = Point(1, 2)
    p.x = "123"
    with pytest.raises(ValidationError):
        p.struct_check_types()


# ── error handling ──


def test_error_on_non_struct():
    with pytest.raises(TypeError, match="doesn't apply to"):
        Point.struct_check_types("not a struct")


def test_extra_positional_args_raises():
    p = Point(1, 2)
    with pytest.raises(TypeError, match="takes no positional arguments"):
        p.struct_check_types(99)


def test_bad_kwarg_raises():
    p = Point(1, 2)
    with pytest.raises(TypeError, match="unexpected keyword argument"):
        p.struct_check_types(bad=True)


# ── union fields ──


def test_struct_union_valid_member_passes():
    class A(Struct):
        struct_config = StructConfig(tag="a")
        a: int

    class B(Struct):
        struct_config = StructConfig(tag="b")
        b: str

    class AB(Struct):
        struct_config = StructConfig(check_types_on_init=True)
        x: A | B

    ab = AB(A(1))
    assert ab.struct_check_types() is None

    ab2 = AB(B("hi"))
    assert ab2.struct_check_types() is None


def test_struct_union_none_passes():
    class A(Struct):
        struct_config = StructConfig(tag="a")
        a: int

    class AO(Struct):
        struct_config = StructConfig(check_types_on_init=True)
        x: A | None

    AO(None).struct_check_types()
    AO(A(1)).struct_check_types()


def test_struct_union_non_member_rejected():
    class A(Struct):
        struct_config = StructConfig(tag="a")
        a: int

    class B(Struct):
        struct_config = StructConfig(tag="b")
        b: str

    class AB(Struct):
        x: A | B

    class C(Struct):
        c: float

    ab = AB(A(1))
    ab.x = C(1.5)  # C is NOT a member of A|B
    with pytest.raises(ValidationError):
        ab.struct_check_types()


def test_struct_union_non_member_at_init():
    class A(Struct):
        struct_config = StructConfig(tag="a")
        a: int

    class B(Struct):
        struct_config = StructConfig(tag="b")
        b: str

    class C(Struct):
        c: float

    class AB(Struct):
        struct_config = StructConfig(check_types_on_init=True)
        x: A | B

    with pytest.raises(ValidationError):
        AB(C(1.5))


def test_struct_union_nested_field_mutation_caught():
    class A(Struct):
        struct_config = StructConfig(tag="a")
        a: int

    class AB(Struct):
        struct_config = StructConfig(check_types_on_init=True)
        x: A | None

    ab = AB(A(1))
    ab.x.a = "bad"
    with pytest.raises(ValidationError):
        ab.struct_check_types()


# ── frozen struct ──


def test_frozen_with_bad_value_raises():
    class Frozen(Struct):
        struct_config = StructConfig(frozen=True)
        x: int

    f = Frozen("bad")
    with pytest.raises(ValidationError):
        f.struct_check_types()


def test_frozen_valid():
    class Frozen(Struct):
        struct_config = StructConfig(frozen=True)
        x: int

    f = Frozen(1)
    assert f.struct_check_types() is None
