import collections
import datetime
import decimal
import enum
import sys
import typing
import uuid
from collections import namedtuple
from copy import deepcopy
from dataclasses import dataclass, field
from typing import (
    Annotated,
    Any,
    Dict,
    Final,
    FrozenSet,
    Generic,
    List,
    Literal,
    NamedTuple,
    NewType,
    Set,
    Tuple,
    TypedDict,
    TypeVar,
    Union,
)

import pytest

import structtype
import structtype._inspect as mi
from structtype import Factory, Field, NumericValidator, StrValidator, BytesValidator, CollectionValidator, TimezoneValidator, Struct, StructConfig

from .utils import py315_or_later_only, temp_module

if sys.version_info >= (3, 15):
    # This is needed for `ruff` to recognize `frozendict` name
    # and to not raise `F821`:
    from builtins import frozendict

PY312 = sys.version_info[:2] >= (3, 12)
py312_plus = pytest.mark.skipif(not PY312, reason="3.12+ only")

T = TypeVar("T")


@pytest.mark.parametrize(
    "a,b,sol",
    [
        (
            {"a": {"b": {"c": 1}}},
            {"a": {"b": {"d": 2}}},
            {"a": {"b": {"c": 1, "d": 2}}},
        ),
        ({"a": {"b": {"c": 1}}}, {"a": {"b": 2}}, {"a": {"b": 2}}),
        ({"a": [1, 2]}, {"a": [3, 4]}, {"a": [1, 2, 3, 4]}),
        ({"a": {"b": 1}}, {"a2": 3}, {"a": {"b": 1}, "a2": 3}),
        ({"a": 1}, {}, {"a": 1}),
    ],
)
def test_merge_json(a, b, sol):
    a_orig = deepcopy(a)
    b_orig = deepcopy(b)
    res = mi._merge_json(a, b)
    assert res == sol
    assert a == a_orig
    assert b == b_orig


def test_inspect_module_dir():
    assert mi.__dir__() == mi.__all__


def test_any():
    assert mi.type_info(Any) == mi.AnyType()


def test_typevar():
    assert mi.type_info(T) == mi.AnyType()


def test_bound_typevar():
    T = TypeVar("T", bound=int | str)
    assert mi.type_info(T) == mi.UnionType((mi.IntType(), mi.StrType()))


def test_none():
    assert mi.type_info(None) == mi.NoneType()


def test_bool():
    assert mi.type_info(bool) == mi.BoolType()


@pytest.mark.parametrize(
    "kw", [{}, dict(ge=2), dict(gt=2), dict(le=2), dict(lt=2), dict(multiple_of=2)]
)
@pytest.mark.parametrize("typ, info_type", [(int, mi.IntType), (float, mi.FloatType)])
def test_numeric(kw, typ, info_type):
    if kw:
        typ = Annotated[typ, NumericValidator(**kw)]
    assert mi.type_info(typ) == info_type(**kw)


@pytest.mark.parametrize(
    "kw",
    [{}, dict(pattern="[a-z]*"), dict(min_length=0), dict(max_length=3)],
)
def test_string(kw):
    typ = str
    if kw:
        typ = Annotated[typ, StrValidator(**kw)]
    assert mi.type_info(typ) == mi.StrType(**kw)


@pytest.mark.parametrize(
    "kw",
    [{}, dict(min_length=0), dict(max_length=3)],
)
@pytest.mark.parametrize(
    "typ, info_type",
    [
        (bytes, mi.BytesType),
        (bytearray, mi.ByteArrayType),
        (memoryview, mi.MemoryViewType),
    ],
)
def test_binary(kw, typ, info_type):
    if kw:
        typ = Annotated[typ, BytesValidator(**kw)]
    assert mi.type_info(typ) == info_type(**kw)


@pytest.mark.parametrize(
    "kw",
    [{}, dict(tz=True), dict(tz=False)],
)
def test_datetime(kw):
    typ = datetime.datetime
    if kw:
        typ = Annotated[typ, TimezoneValidator(**kw)]
    assert mi.type_info(typ) == mi.DateTimeType(**kw)


@pytest.mark.parametrize(
    "kw",
    [{}, dict(tz=True), dict(tz=False)],
)
def test_time(kw):
    typ = datetime.time
    if kw:
        typ = Annotated[typ, TimezoneValidator(**kw)]
    assert mi.type_info(typ) == mi.TimeType(**kw)


def test_date():
    assert mi.type_info(datetime.date) == mi.DateType()


def test_timedelta():
    assert mi.type_info(datetime.timedelta) == mi.TimeDeltaType()


def test_uuid():
    assert mi.type_info(uuid.UUID) == mi.UUIDType()


def test_decimal():
    assert mi.type_info(decimal.Decimal) == mi.DecimalType()


def test_raw():
    assert mi.type_info(structtype.Raw) == mi.RawType()


def test_msgpack_ext():
    pass

def test_newtype():
    UserId = NewType("UserId", str)
    assert mi.type_info(UserId) == mi.StrType()
    assert mi.type_info(Annotated[UserId, StrValidator(max_length=10)]) == mi.StrType(
        max_length=10
    )

    # Annotated in NewType
    UserId = NewType("UserId", Annotated[str, StrValidator(max_length=10)])
    assert mi.type_info(UserId) == mi.StrType(max_length=10)

    # NewType in NewType (no extra annotation to avoid double-Validator)
    UserId2 = NewType("UserId2", UserId)
    assert mi.type_info(UserId2) == mi.StrType(max_length=10)


@py312_plus
@pytest.mark.parametrize(
    "src, typ",
    [
        ("type Ex = str | None", str | None),
        ("type Ex[T] = tuple[T, int]", tuple[Any, int]),
        ("type Temp[T] = tuple[T, int]; Ex = Temp[str]", tuple[str, int]),
    ],
)
def test_typealias(src, typ):
    with temp_module(src) as mod:
        assert mi.type_info(mod.Ex) == mi.type_info(typ)


def test_final():
    cases = [
        (int, mi.IntType()),
        (Annotated[int, NumericValidator(ge=0)], mi.IntType(ge=0)),
        (NewType("UserId", Annotated[int, NumericValidator(ge=0)]), mi.IntType(ge=0)),
    ]
    for typ, sol in cases:

        class Ex(structtype.Struct):
            x: Final[typ]

        info = mi.type_info(Ex)
        assert info.fields[0].type == sol


def test_custom():
    assert mi.type_info(complex) == mi.CustomType(complex)


@pytest.mark.parametrize(
    "kw",
    [{}, dict(min_length=0), dict(max_length=3)],
)
@pytest.mark.parametrize(
    "typ, info_type",
    [
        (list, mi.ListType),
        (tuple, mi.VarTupleType),
        (set, mi.SetType),
        (frozenset, mi.FrozenSetType),
        (List, mi.ListType),
        (Tuple, mi.VarTupleType),
        (Set, mi.SetType),
        (FrozenSet, mi.FrozenSetType),
    ],
)
@pytest.mark.parametrize("has_item_type", [False, True])
def test_sequence(kw, typ, info_type, has_item_type):
    if has_item_type:
        item_type = mi.IntType()
        if info_type is mi.VarTupleType:
            typ = typ[int, ...]
        else:
            typ = typ[int]
    else:
        item_type = mi.AnyType()

    if kw:
        typ = Annotated[typ, CollectionValidator(**kw)]

    sol = info_type(item_type=item_type, **kw)
    assert mi.type_info(typ) == sol


@pytest.mark.parametrize("typ", [Tuple, tuple])
def test_tuple(typ):
    assert mi.type_info(typ[()]) == mi.TupleType(())
    assert mi.type_info(typ[int]) == mi.TupleType((mi.IntType(),))
    assert mi.type_info(typ[int, float]) == mi.TupleType((mi.IntType(), mi.FloatType()))


@pytest.mark.parametrize("typ", [Dict, dict])
@pytest.mark.parametrize("kw", [{}, dict(min_length=0), dict(max_length=3)])
@pytest.mark.parametrize("has_args", [False, True])
def test_dict(typ, kw, has_args):
    if has_args:
        typ = typ[int, float]
        key = mi.IntType()
        val = mi.FloatType()
    else:
        key = val = mi.AnyType()
    if kw:
        typ = Annotated[typ, CollectionValidator(**kw)]
    sol = mi.DictType(key_type=key, value_type=val, **kw)
    assert mi.type_info(typ) == sol


@py315_or_later_only
@pytest.mark.parametrize("kw", [{}, dict(min_length=0), dict(max_length=3)])
@pytest.mark.parametrize("has_args", [False, True])
def test_frozendict(kw, has_args):
    if has_args:
        typ = frozendict[int, float]
        key = mi.IntType()
        val = mi.FloatType()
    else:
        typ = frozendict
        key = val = mi.AnyType()
    if kw:
        typ = Annotated[typ, CollectionValidator(**kw)]
    sol = mi.FrozenDictType(key_type=key, value_type=val, **kw)
    assert mi.type_info(typ) == sol


@pytest.mark.parametrize(
    "typ",
    [
        typing.Collection,
        typing.MutableSequence,
        typing.Sequence,
        collections.abc.Collection,
        collections.abc.MutableSequence,
        collections.abc.Sequence,
        typing.MutableSet,
        typing.AbstractSet,
        collections.abc.MutableSet,
        collections.abc.Set,
    ],
)
def test_abstract_sequence(typ):
    if "Set" in str(typ):
        col_type = mi.SetType
    else:
        col_type = mi.ListType

    assert mi.type_info(typ) == col_type(mi.AnyType())
    assert mi.type_info(typ[int]) == col_type(mi.IntType())


@pytest.mark.parametrize(
    "typ",
    [
        typing.MutableMapping,
        typing.Mapping,
        collections.abc.MutableMapping,
        collections.abc.Mapping,
    ],
)
def test_abstract_mapping(typ):
    assert mi.type_info(typ) == mi.DictType(mi.AnyType(), mi.AnyType())
    assert mi.type_info(typ[str, int]) == mi.DictType(mi.StrType(), mi.IntType())


@pytest.mark.parametrize("use_union_operator", [False, True])
def test_union(use_union_operator):
    if use_union_operator:
        typ = int | str
    else:
        typ = Union[int, str]

    sol = mi.UnionType((mi.IntType(), mi.StrType()))
    assert mi.type_info(typ) == sol

    assert not sol.includes_none
    assert mi.type_info(Union[int, None]).includes_none
    assert mi.type_info(int | None).includes_none


def test_int_literal():
    assert mi.type_info(Literal[3, 1, 2]) == mi.LiteralType((1, 2, 3))


def test_str_literal():
    assert mi.type_info(Literal["c", "a", "b"]) == mi.LiteralType(("a", "b", "c"))


def test_bool_literal():
    assert mi.type_info(Literal[True]) == mi.LiteralType((True,))
    assert mi.type_info(Literal[True, False]) == mi.LiteralType((False, True))


def test_mixed_literal():
    # Literals may mix value types; `type_info` shouldn't crash trying to sort
    # values of incomparable types (gh#1018).
    assert mi.type_info(Literal[1, None]) == mi.LiteralType((None, 1))
    assert mi.type_info(Literal[True, "yes"]) == mi.LiteralType((True, "yes"))


def test_int_enum():
    class Example(enum.IntEnum):
        B = 3
        A = 2

    assert mi.type_info(Example) == mi.EnumType(Example)


def test_enum():
    class Example(enum.Enum):
        B = "z"
        A = "y"

    assert mi.type_info(Example) == mi.EnumType(Example)


@pytest.mark.parametrize(
    "kw",
    [
        {},
        {"array_like": True},
        {"forbid_unknown_fields": True},
        {"tag": "Example", "tag_field": "type"},
    ],
)
def test_struct(kw):
    def factory():
        return "foo"

    ns = {"structtype": structtype, "StructConfig": StructConfig, "factory": factory}
    kwargs_str = ", ".join(f"{k}={v!r}" for k, v in kw.items())
    exec(f"class Example(structtype.Struct):\n    struct_config = StructConfig({kwargs_str})\n    x: int\n    y: int = 0\n    z: int = structtype.Factory(factory)", ns)
    Example = ns["Example"]

    sol = mi.StructType(
        cls=Example,
        fields=(
            mi.FieldNode(name="x", alias="x", type=mi.IntType()),
            mi.FieldNode(
                name="y", alias="y", type=mi.IntType(), required=False, default=0
            ),
            mi.FieldNode(
                name="z",
                alias="z",
                type=mi.IntType(),
                required=False,
                default_factory=factory,
            ),
        ),
        **kw,
    )
    assert mi.type_info(Example) == sol


def test_struct_no_fields():
    class Example(structtype.Struct):
        pass

    sol = mi.StructType(Example, fields=())
    assert mi.type_info(Example) == sol


def test_struct_keyword_only():
    class Example(structtype.Struct):
        struct_config = StructConfig(kw_only=True)
        a: int
        b: int = 1
        c: int
        d: int = 2

    sol = mi.StructType(
        Example,
        fields=(
            mi.FieldNode("a", "a", mi.IntType()),
            mi.FieldNode("b", "b", mi.IntType(), required=False, default=1),
            mi.FieldNode("c", "c", mi.IntType()),
            mi.FieldNode("d", "d", mi.IntType(), required=False, default=2),
        ),
    )
    assert mi.type_info(Example) == sol


def test_struct_alias():
    class Example(structtype.Struct):
        struct_config = StructConfig(rename="camel")
        field_one: int
        field_two: int

    sol = mi.StructType(
        Example,
        fields=(
            mi.FieldNode("field_one", "fieldOne", mi.IntType()),
            mi.FieldNode("field_two", "fieldTwo", mi.IntType()),
        ),
    )
    assert mi.type_info(Example) == sol


def test_generic_struct():
    class Example(structtype.Struct, Generic[T]):
        a: T
        b: list[T]

    sol = mi.StructType(
        Example,
        fields=(
            mi.FieldNode("a", "a", mi.AnyType()),
            mi.FieldNode("b", "b", mi.ListType(mi.AnyType())),
        ),
    )
    assert mi.type_info(Example) == sol

    sol = mi.StructType(
        Example[int],
        fields=(
            mi.FieldNode("a", "a", mi.IntType()),
            mi.FieldNode("b", "b", mi.ListType(mi.IntType())),
        ),
    )
    assert mi.type_info(Example[int]) == sol


def test_typing_namedtuple():
    class Example(NamedTuple):
        a: str
        b: bool
        c: int = 0

    sol = mi.NamedTupleType(
        Example,
        fields=(
            mi.FieldNode("a", "a", mi.StrType()),
            mi.FieldNode("b", "b", mi.BoolType()),
            mi.FieldNode("c", "c", mi.IntType(), required=False, default=0),
        ),
    )
    assert mi.type_info(Example) == sol


def test_collections_namedtuple():
    Example = namedtuple("Example", ["a", "b", "c"], defaults=(0,))

    sol = mi.NamedTupleType(
        Example,
        fields=(
            mi.FieldNode("a", "a", mi.AnyType()),
            mi.FieldNode("b", "b", mi.AnyType()),
            mi.FieldNode("c", "c", mi.AnyType(), required=False, default=0),
        ),
    )
    assert mi.type_info(Example) == sol


def test_generic_namedtuple():
    NamedTuple = pytest.importorskip("typing_extensions").NamedTuple

    class Example(NamedTuple, Generic[T]):
        a: T
        b: list[T]

    sol = mi.NamedTupleType(
        Example,
        fields=(
            mi.FieldNode("a", "a", mi.AnyType()),
            mi.FieldNode("b", "b", mi.ListType(mi.AnyType())),
        ),
    )
    assert mi.type_info(Example) == sol

    sol = mi.NamedTupleType(
        Example[int],
        fields=(
            mi.FieldNode("a", "a", mi.IntType()),
            mi.FieldNode("b", "b", mi.ListType(mi.IntType())),
        ),
    )
    assert mi.type_info(Example[int]) == sol


@pytest.mark.parametrize("use_typing_extensions", [False, True])
def test_typeddict(use_typing_extensions):
    if use_typing_extensions:
        tex = pytest.importorskip("typing_extensions")
        cls = tex.TypedDict
    else:
        cls = TypedDict

    class Example(cls):
        a: str
        b: bool
        c: int

    sol = mi.TypedDictType(
        Example,
        fields=(
            mi.FieldNode("a", "a", mi.StrType()),
            mi.FieldNode("b", "b", mi.BoolType()),
            mi.FieldNode("c", "c", mi.IntType()),
        ),
    )
    assert mi.type_info(Example) == sol


@pytest.mark.parametrize("use_typing_extensions", [False, True])
def test_typeddict_optional(use_typing_extensions):
    if use_typing_extensions:
        tex = pytest.importorskip("typing_extensions")
        cls = tex.TypedDict
    else:
        cls = TypedDict

    class Base(cls):
        a: str
        b: bool

    class Example(Base, total=False):
        c: int

    sol = mi.TypedDictType(
        Example,
        fields=(
            mi.FieldNode("a", "a", mi.StrType()),
            mi.FieldNode("b", "b", mi.BoolType()),
            mi.FieldNode("c", "c", mi.IntType(), required=False),
        ),
    )
    assert mi.type_info(Example) == sol


def test_generic_typeddict():
    TypedDict = pytest.importorskip("typing_extensions").TypedDict

    class Example(TypedDict, Generic[T]):
        a: T
        b: list[T]

    sol = mi.TypedDictType(
        Example,
        fields=(
            mi.FieldNode("a", "a", mi.AnyType()),
            mi.FieldNode("b", "b", mi.ListType(mi.AnyType())),
        ),
    )
    assert mi.type_info(Example) == sol

    sol = mi.TypedDictType(
        Example[int],
        fields=(
            mi.FieldNode("a", "a", mi.IntType()),
            mi.FieldNode("b", "b", mi.ListType(mi.IntType())),
        ),
    )
    assert mi.type_info(Example[int]) == sol


def test_dataclass():
    @dataclass
    class Example:
        x: int
        y: int = 0
        z: str = field(default_factory=str)

    sol = mi.DataclassType(
        Example,
        fields=(
            mi.FieldNode("x", "x", mi.IntType()),
            mi.FieldNode("y", "y", mi.IntType(), required=False, default=0),
            mi.FieldNode("z", "z", mi.StrType(), required=False, default_factory=str),
        ),
    )
    assert mi.type_info(Example) == sol


def test_attrs():
    attrs = pytest.importorskip("attrs")

    @attrs.define
    class Example:
        x: int
        y: int = 0
        z: str = attrs.field(factory=str)

    sol = mi.DataclassType(
        Example,
        fields=(
            mi.FieldNode("x", "x", mi.IntType()),
            mi.FieldNode("y", "y", mi.IntType(), required=False, default=0),
            mi.FieldNode("z", "z", mi.StrType(), required=False, default_factory=str),
        ),
    )
    assert mi.type_info(Example) == sol


@pytest.mark.parametrize("module", ["dataclasses", "attrs"])
def test_generic_dataclass_or_attrs(module):
    m = pytest.importorskip(module)
    decorator = m.define if module == "attrs" else m.dataclass

    @decorator
    class Example(Generic[T]):
        a: T
        b: list[T]

    sol = mi.DataclassType(
        Example,
        fields=(
            mi.FieldNode("a", "a", mi.AnyType()),
            mi.FieldNode("b", "b", mi.ListType(mi.AnyType())),
        ),
    )
    assert mi.type_info(Example) == sol

    sol = mi.DataclassType(
        Example[int],
        fields=(
            mi.FieldNode("a", "a", mi.IntType()),
            mi.FieldNode("b", "b", mi.ListType(mi.IntType())),
        ),
    )
    assert mi.type_info(Example[int]) == sol


@pytest.mark.parametrize("kind", ["struct", "dataclass", "attrs"])
def test_unset_fields(kind):
    if kind == "struct":

        class Ex(structtype.Struct):
            x: int | structtype.UnsetType = structtype.UNSET

    elif kind == "dataclass":

        @dataclass
        class Ex:
            x: int | structtype.UnsetType = structtype.UNSET

    elif kind == "attrs":
        attrs = pytest.importorskip("attrs")

        @attrs.define
        class Ex:
            x: int | structtype.UnsetType = structtype.UNSET

    res = mi.type_info(Ex)
    assert res.fields == (mi.FieldNode("x", "x", mi.IntType(), required=False),)


@pytest.mark.parametrize("kind", ["struct", "namedtuple", "typeddict", "dataclass"])
def test_self_referential_objects(kind):
    if kind == "struct":
        code = """
        import structtype

        class Node(structtype.Struct):
            child: "Node"
        """
    elif kind == "namedtuple":
        code = """
        from typing import NamedTuple

        class Node(NamedTuple):
            child: "Node"
        """
    elif kind == "typeddict":
        code = """
        from typing import TypedDict

        class Node(TypedDict):
            child: "Node"
        """
    elif kind == "dataclass":
        code = """
        from dataclasses import dataclass

        @dataclass
        class Node:
            child: "Node"
        """

    with temp_module(code) as mod:
        res = mi.type_info(mod.Node)

    assert res.cls is mod.Node
    assert res.fields[0].name == "child"
    assert res.fields[0].type is res


def test_metadata():
    typ = Annotated[int, NumericValidator(gt=1), Field(title="a"), Field(description="b")]

    assert mi.type_info(typ) == mi.Metadata(
        mi.IntType(gt=1), {"title": "a", "description": "b"}
    )

    typ = Annotated[
        int,
        Field(json_schema_extra={"title": "a", "description": "b"}),
        Field(json_schema_extra={"title": "c", "examples": [1, 2]}),
    ]

    assert mi.type_info(typ) == mi.Metadata(
        mi.IntType(), {"title": "c", "description": "b", "examples": [1, 2]}
    )

    typ = Annotated[int, Field(deprecated=True)]

    assert mi.type_info(typ) == mi.Metadata(mi.IntType(), {"deprecated": True})

    typ = Annotated[int, Field(deprecated=False)]

    assert mi.type_info(typ) == mi.Metadata(mi.IntType(), {"deprecated": False})


def test_inspect_with_unhashable_metadata():
    typ = Annotated[int, {"unhashable"}]

    assert mi.type_info(typ) == mi.IntType()


def test_multi_type_info():
    class Example(structtype.Struct):
        x: int
        y: int

    ex_type = mi.StructType(
        Example,
        fields=(
            mi.FieldNode("x", "x", mi.IntType()),
            mi.FieldNode("y", "y", mi.IntType()),
        ),
    )

    assert mi.multi_type_info([]) == ()

    res = mi.multi_type_info([Example, list[Example]])
    assert res == (ex_type, mi.ListType(ex_type))
    assert res[0] is res[1].item_type


def test_type_info_custom_base_class():
    class CustomMeta(structtype.StructMeta):
        pass

    class Base(metaclass=CustomMeta):
        pass

    class Model(Base):
        foo: str

    assert mi.type_info(Model) == mi.StructType(
        cls=Model,
        fields=(
            mi.FieldNode(
                name="foo",
                alias="foo",
                type=mi.StrType(min_length=None, max_length=None, pattern=None),
                required=True,
                default=structtype.NODEFAULT,
                default_factory=structtype.NODEFAULT,
            ),
        ),
        tag_field=None,
        tag=None,
        array_like=False,
        forbid_unknown_fields=False,
    )


def test_is_struct_runtime():
    class Base(structtype.Struct):
        x: int

    class Derived(Base):
        pass

    class Generated(structtype.Struct):
        x: int

    class CustomMeta(structtype.StructMeta):
        pass

    class CustomBase(metaclass=CustomMeta):
        x: int

    class Custom(CustomBase):
        pass

    class NotStruct:
        pass

    assert isinstance(type(Base(1)), structtype.StructMeta)
    assert isinstance(type(Derived(1)), structtype.StructMeta)
    assert isinstance(type(Generated(1)), structtype.StructMeta)
    assert isinstance(type(Custom(1)), structtype.StructMeta)
    assert not isinstance(type(NotStruct()), structtype.StructMeta)
    assert not isinstance(type(object()), structtype.StructMeta)


def test_is_struct_type_runtime():
    class Base(structtype.Struct):
        x: int

    class Derived(Base):
        pass

    class Generated(structtype.Struct):
        x: int

    class CustomMeta(structtype.StructMeta):
        pass

    class CustomBase(metaclass=CustomMeta):
        pass

    class Custom(CustomBase):
        x: int

    class NotStruct:
        pass

    assert isinstance(Base, structtype.StructMeta)
    assert isinstance(Derived, structtype.StructMeta)
    assert isinstance(Generated, structtype.StructMeta)
    assert isinstance(Custom, structtype.StructMeta)
    assert not isinstance(NotStruct, structtype.StructMeta)
    assert not isinstance(object, structtype.StructMeta)


def test_pydantic():
    pydantic = pytest.importorskip("pydantic")

    class Example(pydantic.BaseModel):
        x: int
        y: int = 0
        z: str = pydantic.Field(default_factory=str)

    sol = mi.PydanticType(
        Example,
        fields=(
            mi.FieldNode("x", "x", mi.IntType()),
            mi.FieldNode("y", "y", mi.IntType(), required=False, default=0),
            mi.FieldNode("z", "z", mi.StrType(), required=False, default_factory=str),
        ),
    )
    assert mi.type_info(Example) == sol


# ------------------------------------------------------------------
# Coverage: FieldInfo.__repr__ branches (_inspect.py:643-656)
# ------------------------------------------------------------------


class _PRepr(Struct):
    a: int
    b: str = "x"


def test_fieldinfo_repr_required():
    fi = structtype.fields(_PRepr)[0]
    r = repr(fi)
    assert "required=True" in r
    assert "default=" not in r


def test_fieldinfo_repr_default():
    fi = structtype.fields(_PRepr)[1]
    r = repr(fi)
    assert "required=False" in r
    assert "default='x'" in r


class _PFactory(Struct):
    xs: list = structtype.Factory(list)


def test_fieldinfo_repr_factory():
    fi = structtype.fields(_PFactory)[0]
    r = repr(fi)
    assert "required=False" in r
    assert "default_factory=" in r


# ------------------------------------------------------------------
# Coverage: tuple[()] normalization (_inspect.py:1017)
# ------------------------------------------------------------------
def test_tuple_empty_tuple_args():
    ti = mi.type_info(Tuple[()])
    assert isinstance(ti, mi.TupleType)
    assert ti.item_types == ()
