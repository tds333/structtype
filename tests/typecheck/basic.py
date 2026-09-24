"""Type-check fixture: basic ``Struct`` usage.

Checked by ``make typecheck-tests`` and ``tests/test_typecheck.py`` against the
shipped ``structtype`` type stubs. This file must stay free of ``# type:
ignore`` — any error reported here is a stub bug.
"""

from typing import Any

from structtype import Struct, StructConfig


class Point(Struct):
    x: int
    y: str = "a"


p = Point(1)
p2 = Point(x=1, y="b")
px: int = p.x
py: str = p.y


class KwOnly(Struct):
    struct_config = StructConfig(kw_only=True)
    a: int
    b: str


k = KwOnly(a=1, b="x")


class Frozen(Struct):
    struct_config = StructConfig(frozen=True)
    v: int


f = Frozen(v=1)


class Arr(Struct):
    struct_config = StructConfig(array_like=True)
    a: int
    b: int


a = Arr(1, 2)


class Ordered(Struct):
    struct_config = StructConfig(order=True)
    v: int


o = Ordered(1)

# `order=True` generates the ordering dunders (docs/usage.rst)
lt: bool = o < Ordered(2)
le: bool = o <= Ordered(2)
gt: bool = o > Ordered(0)
ge: bool = o >= Ordered(0)

# Documented struct members
slots: tuple[str, ...] = Point.__slots__
inst_slots: tuple[str, ...] = p.__slots__
match_args: tuple[str, ...] = Point.__match_args__

# Struct methods
j: bytes = p.struct_dump_json()
d = p.struct_dump()
p.struct_check_types()
q: Point = Point.struct_validate_json(b'{"x":1}')
r: Point = Point.struct_validate({"x": 1})

# Class-level attributes
field_names: tuple[str, ...] = Point.__struct_fields__
defaults: tuple[Any, ...] = Point.__struct_defaults__
cfg: StructConfig = Point.__struct_config__
sc: StructConfig = Point.struct_config
sc2: StructConfig = Point.__struct_config__
frozen: bool = cfg["frozen"]

# struct_config is present even on classes that never declare it
sc_base: StructConfig = Struct.struct_config
sc_inst: StructConfig = p.struct_config
