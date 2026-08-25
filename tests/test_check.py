import datetime
from typing import Annotated, TypedDict

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


# ── typed dict field ──


class PointTD(TypedDict):
    x: int
    y: str


def test_typeddict_missing_required_key_raises():
    class Holder(Struct):
        td: PointTD

    with pytest.raises(
        ValidationError, match=r"Object missing required field `y` - at `\$\.td`"
    ):
        Holder.struct_validate({"td": {"x": 1}})


def test_typeddict_missing_required_key_check_types():
    class Holder(Struct):
        td: PointTD

    h = Holder({"x": 1, "y": "s"})
    h.td.pop("y")
    with pytest.raises(ValidationError):
        h.struct_check_types()


# ── unknown fields ──


def test_unknown_field_json_error():
    class Strict(Struct):
        struct_config = StructConfig(forbid_unknown_fields=True)
        a: int

    with pytest.raises(ValidationError, match="Object contains unknown field `b`"):
        Strict.struct_validate_json(b'{"a":1,"b":2}')


def test_unknown_field_object_error():
    class Strict(Struct):
        struct_config = StructConfig(forbid_unknown_fields=True)
        a: int

    with pytest.raises(ValidationError, match="Object contains unknown field `b`"):
        Strict.struct_validate({"a": 1, "b": 2})


# ── from_attributes fallbacks ──


def test_from_attributes_getitem_fallback():
    class Target(Struct):
        a: int
        b: str

    class GetItemOnly:
        def __getitem__(self, name):
            return {"a": 1, "b": "x"}[name]

    out = Target.struct_validate(GetItemOnly(), from_attributes=True)
    assert out == Target(1, "x")


def test_from_attributes_missing_attr_errors():
    class Target(Struct):
        a: int

    class Empty:
        pass

    with pytest.raises(ValidationError):
        Target.struct_validate(Empty(), from_attributes=True)



# ── validator type matrix ──

import decimal
import enum
from dataclasses import dataclass
from types import MappingProxyType
from typing import Any, NamedTuple, TypedDict as _TypedDict
from uuid import UUID

import structtype


class Color(enum.Enum):
    RED = "r"


class IntVal(enum.Enum):
    A = 5


class PointNT(NamedTuple):
    x: int
    y: str


class PairTD(_TypedDict):
    x: int


@dataclass
class DCPoint:
    x: int


class Inner(Struct):
    p: int


VALIDATOR_MATRIX = [
    # (id, annotation, valid_input, invalid_input_or_None)
    ("bool", bool, True, 0),
    ("int", int, 3, "3"),
    ("int-big", int, 2**70, 3.0),
    ("float", float, 1.5, "1.5"),
    ("float-coerce-int", float, 3, None),
    ("str", str, "x", b"x"),
    ("bytes", bytes, b"ab", "ab"),
    ("bytearray", bytearray, bytearray(b"x"), 5),
    ("memoryview", memoryview, memoryview(b"x"), 5),
    ("datetime", datetime.datetime, datetime.datetime(2020, 1, 1), 0),
    ("date", datetime.date, datetime.date(2020, 1, 1), 0),
    ("time", datetime.time, datetime.time(1, 30), 0),
    ("timedelta", datetime.timedelta, datetime.timedelta(seconds=1), 1),
    ("uuid", UUID, UUID(int=7), 7),
    ("decimal", decimal.Decimal, decimal.Decimal("1.5"), [1]),
    ("enum", Color, Color.RED, "nope"),
    ("raw", structtype.Raw, structtype.Raw(b"{}"), "{}"),
    ("any-set", Any, object(), None),
    ("set-coerce-list", set[int], [1, 2], ["x"]),
    ("frozenset", frozenset[int], frozenset([1]), [object()]),
    ("vartuple", tuple[int, ...], [1, 2], [1, "x"]),
    ("fixtuple", tuple[int, str], [1, "x"], ["x", 1]),
    ("namedtuple", PointNT, PointNT(1, "x"), (1,)),
    ("list-items-checked", list[Inner], [{"p": 1}], [{"p": "bad"}]),
    ("dict-values-checked", dict[str, Inner], {"k": {"p": 1}}, {"k": {}}),
    ("mapping-to-dict", dict[str, int], MappingProxyType({"a": 1}), [("a", 1)]),
    ("typeddict", PairTD, {"x": 1}, {"x": "bad"}),
    ("dataclass-field", DCPoint, DCPoint(x=1), 5),
    ("nested-struct", Inner, {"p": 5}, {"p": "bad"}),
]


def _matrix_struct(label, annotation):
    return type(
        "V_" + label.replace("-", "_").replace(".", "_"),
        (Struct,),
        {"__annotations__": {"v": annotation}},
    )


@pytest.mark.parametrize(
    "label,annotation,good,bad",
    VALIDATOR_MATRIX,
    ids=[m[0] for m in VALIDATOR_MATRIX],
)
def test_validator_matrix_accepts_valid(label, annotation, good, bad):
    cls = _matrix_struct(label, annotation)
    out = cls.struct_validate({"v": good})
    assert isinstance(out, Struct)


@pytest.mark.parametrize(
    "label,annotation,good,bad",
    [m for m in VALIDATOR_MATRIX if m[3] is not None],
    ids=[m[0] for m in VALIDATOR_MATRIX if m[3] is not None],
)
def test_validator_matrix_rejects_invalid(label, annotation, good, bad):
    cls = _matrix_struct(label, annotation)
    with pytest.raises(ValidationError):
        cls.struct_validate({"v": bad})


@pytest.mark.parametrize(
    "ann,value,expected",
    [
        (datetime.datetime, "2020-01-01T00:00:00", datetime.datetime(2020, 1, 1)),
        (
            datetime.datetime,
            "2020-01-01T00:00:00Z",
            datetime.datetime(2020, 1, 1, tzinfo=datetime.timezone.utc),
        ),
        (datetime.date, "2020-01-01", datetime.date(2020, 1, 1)),
        (datetime.time, "01:30:00", datetime.time(1, 30)),
        (datetime.timedelta, "PT1S", datetime.timedelta(seconds=1)),
        (Color, "r", Color.RED),
        (IntVal, 5, IntVal.A),
        (decimal.Decimal, 3, decimal.Decimal("3")),
        (decimal.Decimal, "1.5", decimal.Decimal("1.5")),
        (decimal.Decimal, 1.5, decimal.Decimal("1.5")),
        (float, 3, 3.0),
    ],
)
def test_validator_coercions(ann, value, expected):
    cls = _matrix_struct("coerce_" + expected.__class__.__name__, ann)
    out = cls.struct_validate({"v": value})
    assert out.v == expected


# ── tagged unions through the validator ──


class ArrI(Struct):
    struct_config = StructConfig(array_like=True, tag="i")
    a: int


class ArrJ(Struct):
    struct_config = StructConfig(array_like=True, tag="j")
    b: str


class Tag1(Struct):
    struct_config = StructConfig(tag=1)
    a: int


class Tag2(Struct):
    struct_config = StructConfig(tag=2)
    b: str


def test_validator_array_like_tagged_union():
    union = ArrI | ArrJ

    class Holder(Struct):
        v: union

    assert Holder.struct_validate({"v": ["i", 5]}).v == ArrI(5)
    assert Holder.struct_validate({"v": ["j", "hi"]}).v == ArrJ("hi")
    with pytest.raises(ValidationError):
        Holder.struct_validate({"v": ["zz", 1]})


def test_validator_int_tagged_union():
    union = Tag1 | Tag2

    class Holder(Struct):
        v: union

    assert Holder.struct_validate({"v": {"type": 2, "b": "x"}}).v == Tag2("x")
    with pytest.raises(ValidationError):
        Holder.struct_validate({"v": {"type": 9, "b": "x"}})


def test_validator_object_to_struct_from_attributes():
    class Target(Struct):
        a: int
        b: str

    class Plain:
        a = 5
        b = "z"

    out = Target.struct_validate(Plain(), from_attributes=True)
    assert out == Target(5, "z")


def test_validator_uuid_from_16_byte_buffer():
    class Holder(Struct):
        v: UUID

    out = Holder.struct_validate({"v": b"0123456789abcdef"})
    assert out.v == UUID("30313233-3435-3637-3839-616263646566")


# ── object-input dispatch tails ──


def test_validator_object_to_dataclass_field():
    class Holder(Struct):
        v: DCPoint

    class Obj:
        x = 5

    out = Holder.struct_validate(Holder(Obj()), from_attributes=True)
    assert out.v.x == 5


def test_validator_array_like_struct_from_list():
    class Pair(Struct):
        struct_config = StructConfig(array_like=True)
        a: int
        b: str

    class Holder(Struct):
        v: Pair

    assert Holder.struct_validate({"v": [1, "x"]}).v == Pair(1, "x")


# ── plain-object conversion dispatch ──


@dataclass
class DCSlots:
    x: int
    y: str = "d"


class _SlotsObj:
    __slots__ = ("x", "y")

    def __init__(self):
        self.x = 5
        self.y = "s"


def test_validator_top_level_slots_object_to_dataclass():
    adapter = structtype.StructAdapter(DCSlots)
    out = adapter.struct_validate(_SlotsObj(), from_attributes=True)
    assert isinstance(out, DCSlots)
    assert out == DCSlots(5, "s")


def test_validator_nested_slots_object_to_dataclass_field():
    class Holder(Struct):
        v: DCSlots

    out = Holder.struct_validate({"v": _SlotsObj()}, from_attributes=True)
    assert isinstance(out.v, DCSlots)
    assert out.v == DCSlots(5, "s")


def test_validator_tagged_struct_from_plain_object():
    class TaggedP(Struct):
        struct_config = StructConfig(tag="tg")
        a: int

    class ObjTag:
        type = "tg"
        a = 1

    out = TaggedP.struct_validate(ObjTag(), from_attributes=True)
    assert out == TaggedP(1)


def test_validator_tagged_struct_wrong_tag_from_object_errors():
    class TaggedW(Struct):
        struct_config = StructConfig(tag="tg")
        a: int

    class ObjWrong:
        type = "zz"
        a = 1

    with pytest.raises(
        structtype.ValidationError, match="Invalid value 'zz' - at `\\$\\.type`"
    ):
        TaggedW.struct_validate(ObjWrong(), from_attributes=True)


def test_from_attributes_getter_failures_fall_back_to_defaults():
    class Target(Struct):
        a: int = 7

    class RaisingAttr:
        def __getattr__(self, name):
            raise RuntimeError("nope")

    assert Target.struct_validate(RaisingAttr(), from_attributes=True).a == 7

    class RaisingGetitem:
        def __getitem__(self, k):
            raise KeyError(k)

    assert Target.struct_validate(RaisingGetitem(), from_attributes=True).a == 7


def test_validator_top_level_struct_union_from_plain_object():
    class U1(Struct):
        struct_config = StructConfig(tag="u1")
        q: int

    class U2(Struct):
        struct_config = StructConfig(tag="u2")
        r: str

    adapter = structtype.StructAdapter(U1 | U2)

    class ObjU2:
        type = "u2"
        r = "hi"

    out = adapter.struct_validate(ObjU2(), from_attributes=True)
    assert isinstance(out, U2)
    assert out == U2("hi")


def test_validator_float_constraint_checked():
    class RangedF(Struct):
        x: Annotated[float, NumericConstraint(gt=0)]

    assert RangedF.struct_validate({"x": 1.5}).x == 1.5
    with pytest.raises(
        structtype.ValidationError, match=r"Expected `float` > 0\.0"
    ):
        RangedF.struct_validate({"x": -1.0})
    with pytest.raises(structtype.ValidationError):
        RangedF.struct_validate({"x": 0.0})
