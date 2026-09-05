import dataclasses
import datetime
import decimal
import enum
import sys
import uuid
from typing import Any, NamedTuple, TypedDict

import pytest

from structtype import ALL_BUILTIN_TYPES, Struct, StructConfig


class Color(enum.Enum):
    RED = "red"


class ColorInt(enum.IntEnum):
    RED = 1
    BLUE = 2


if sys.version_info >= (3, 11):

    class ColorStr(enum.StrEnum):
        RED = "red"


class Inner(Struct):
    x: int


@dataclasses.dataclass
class DC:
    a: int
    b: str


class TD(TypedDict):
    a: int


class NT(NamedTuple):
    a: int
    b: str


def _single_field_struct(label, annotation):
    return type(
        "R_" + label.replace("-", "_"),
        (Struct,),
        {"__annotations__": {"v": annotation}},
    )


ROUNDTRIP_CASES = [
    ("bool", bool, True),
    ("int", int, 3),
    ("float", float, 1.5),
    ("str", str, "x"),
    ("none", type(None), None),
    ("datetime", datetime.datetime, datetime.datetime(2024, 1, 15, 10, 30)),
    ("date", datetime.date, datetime.date(2024, 1, 15)),
    ("time", datetime.time, datetime.time(10, 30)),
    ("timedelta", datetime.timedelta, datetime.timedelta(days=1)),
    ("uuid", uuid.UUID, uuid.UUID(int=7)),
    ("decimal", decimal.Decimal, decimal.Decimal("3.14")),
    ("bytes", bytes, b"hello"),
    ("bytearray", bytearray, bytearray(b"hi")),
    ("memoryview", memoryview, memoryview(b"yo")),
    ("set", set[int], {1, 2}),
    ("frozenset", frozenset[int], frozenset([1, 2])),
    ("list", list[int], [1, 2]),
    ("tuple-fixed", tuple[int, str], (1, "a")),
    ("tuple-vartuple", tuple[int, ...], (1, 2)),
    ("dict", dict[str, int], {"k": 1}),
    ("enum", Color, Color.RED),
    ("intenum", ColorInt, ColorInt.RED),
    ("any", Any, {"k": 1}),
    ("nested-struct", Inner, Inner(1)),
    ("list-struct", list[Inner], [Inner(1), Inner(2)]),
    ("dict-struct", dict[str, Inner], {"k": Inner(3)}),
    ("dataclass", DC, DC(1, "x")),
    ("typeddict", TD, {"a": 2}),
    ("namedtuple", NT, NT(3, "y")),
]

if sys.version_info >= (3, 11):
    ROUNDTRIP_CASES.append(("strenum", ColorStr, ColorStr.RED))


@pytest.mark.parametrize(
    "label,annotation,value",
    ROUNDTRIP_CASES,
    ids=[c[0] for c in ROUNDTRIP_CASES],
)
def test_roundtrip_native_types(label, annotation, value):
    cls = _single_field_struct(label, annotation)
    obj = cls(value)
    back = cls.struct_validate(obj.struct_dump())
    assert back == obj
    assert back.v == value


@pytest.mark.parametrize(
    "label,annotation,value",
    ROUNDTRIP_CASES,
    ids=[c[0] for c in ROUNDTRIP_CASES],
)
def test_roundtrip_json_native_types(label, annotation, value):
    cls = _single_field_struct(label, annotation)
    obj = cls(value)
    msg = obj.struct_dump_json()
    back = cls.struct_validate_json(msg)
    assert back == obj
    assert back.v == value


class KitchenSink(Struct):
    b: bool
    i: int
    f: float
    s: str
    n: type(None)
    dt: datetime.datetime
    date: datetime.date
    time: datetime.time
    delta: datetime.timedelta
    u: uuid.UUID
    dec: decimal.Decimal
    by: bytes
    ba: bytearray
    mv: memoryview
    st: set[int]
    fs: frozenset[int]
    li: list[int]
    tu: tuple[int, str]
    tv: tuple[int, ...]
    di: dict[str, int]
    e: Color
    a: Any
    inner: Inner
    li_struct: list[Inner]
    di_struct: dict[str, Inner]
    dc: DC
    tdict: TD
    nt: NT


def _kitchen_sink():
    return KitchenSink(
        b=True,
        i=3,
        f=1.5,
        s="x",
        n=None,
        dt=datetime.datetime(2024, 1, 15, 10, 30),
        date=datetime.date(2024, 1, 15),
        time=datetime.time(10, 30),
        delta=datetime.timedelta(days=1),
        u=uuid.UUID(int=7),
        dec=decimal.Decimal("3.14"),
        by=b"hello",
        ba=bytearray(b"hi"),
        mv=memoryview(b"yo"),
        st={1, 2},
        fs=frozenset([1, 2]),
        li=[1, 2],
        tu=(1, "a"),
        tv=(1, 2),
        di={"k": 1},
        e=Color.RED,
        a={"k": 1},
        inner=Inner(1),
        li_struct=[Inner(1), Inner(2)],
        di_struct={"k": Inner(3)},
        dc=DC(1, "x"),
        tdict={"a": 2},
        nt=NT(3, "y"),
    )


def test_roundtrip_kitchen_sink():
    obj = _kitchen_sink()
    back = KitchenSink.struct_validate(obj.struct_dump())
    assert back == obj
    back = KitchenSink.struct_validate_json(obj.struct_dump_json())
    assert back == obj


def test_roundtrip_all_builtin_types():
    obj = _kitchen_sink()
    dumped = obj.struct_dump(builtin_types=ALL_BUILTIN_TYPES)
    assert type(dumped["dt"]) is datetime.datetime
    assert type(dumped["dec"]) is decimal.Decimal
    assert type(dumped["by"]) is bytes
    assert type(dumped["u"]) is uuid.UUID
    assert type(dumped["e"]) is Color
    assert type(dumped["st"]) is list
    assert type(dumped["fs"]) is list
    assert type(dumped["tu"]) is tuple
    assert KitchenSink.struct_validate(dumped) == obj


def test_roundtrip_tagged_union():
    class Cat(Struct):
        struct_config = StructConfig(tag="cat", tag_field="type")
        meow: str

    class Dog(Struct):
        struct_config = StructConfig(tag="dog", tag_field="type")
        bark: str

    class Zoo(Struct):
        animal: Cat | Dog

    for animal in (Cat("meow"), Dog("woof")):
        back = Zoo.struct_validate(Zoo(animal).struct_dump())
        assert back == Zoo(animal)
        assert type(back.animal) is type(animal)
