"""Type-check fixture: field type coverage.

Checked by ``make typecheck-tests`` and ``tests/test_typecheck.py`` against the
shipped ``structtype`` type stubs. Must stay free of ``# type: ignore``.
"""

from __future__ import annotations

import datetime as dt
import decimal
import enum
import uuid
from typing import Any, Literal

from structtype import Raw, Struct, StructConfig


class Color(enum.Enum):
    RED = "red"


class State(enum.IntEnum):
    ON = 1


class Meta(enum.Enum):
    A = "a"


class Collections(Struct):
    ints: list[int]
    mapping: dict[str, int]
    s: set[str]
    fs: frozenset[int]
    tup: tuple[int, ...]
    fixed: tuple[int, str]


class Scalar(Struct):
    d: dt.datetime
    date: dt.date
    time: dt.time
    delta: dt.timedelta
    dec: decimal.Decimal
    u: uuid.UUID
    b: bytes


class Variants(Struct):
    opt: int | None
    union: int | str
    lit: Literal["a", "b"]
    any_: Any
    color: Color
    state: State
    meta: Meta


class Nested(Struct):
    p: Collections
    scalar: Scalar


class File(Struct):
    struct_config = StructConfig(tag="file")
    name: str
    size: int


class Dir(Struct):
    struct_config = StructConfig(tag="dir")
    contents: list[File | Dir]


class WithRaw(Struct):
    data: Raw


c = Collections(
    ints=[1], mapping={"a": 1}, s={"x"}, fs=frozenset({1}), tup=(1,), fixed=(1, "a")
)
s = Scalar(
    d=dt.datetime(2020, 1, 1, tzinfo=dt.timezone.utc),
    date=dt.date(2020, 1, 1),
    time=dt.time(1, 2, 3),
    delta=dt.timedelta(1),
    dec=decimal.Decimal("1.0"),
    u=uuid.UUID(int=0),
    b=b"",
)
v = Variants(
    opt=None,
    union=1,
    lit="a",
    any_=1,
    color=Color.RED,
    state=State.ON,
    meta=Meta.A,
)
nested = Nested(p=c, scalar=s)
tree = Dir(contents=[File(name="a", size=1), Dir(contents=[])])
raw = WithRaw(data=Raw(b"{}"))
