# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "msgspec",
#     "pydantic",
#     "structtype",
# ]
#
# [tool.uv.sources]
# structtype = { path = "..", editable = true }
# ///

"""Compare structtype, msgspec, and pydantic across many field types.

Three parts:

- Kitchen-sink struct: a single struct with one field for each supported
  field type, measured for construct, dict validate, dict dump, JSON encode,
  and JSON decode.

- Nested struct: a parent struct whose fields are nested structs containing
  basic types (str, int, float), to show the cost of nesting vs. flat fields.
  Measured for the same five operations.

- Per-type micro-benchmarks: each field type is measured through a struct
  with 10 identical fields of that type (one per library), so the per-type
  cost is amplified ~10x while the struct's fixed overhead stays roughly
  constant. The same five operations as the kitchen-sink struct are measured
  per type.

Notes on fairness:

- ``bytearray`` is excluded: pydantic v2 has no schema for ``bytearray``
  fields.
- ``dict dump`` uses ``model_dump(mode="json")`` for pydantic, since both
  ``struct_dump`` and ``msgspec.to_builtins`` produce JSON-compatible values
  (bytes -> base64 str, datetime -> iso str, set -> list, ...) while the
  default ``model_dump`` leaves such values unconverted.

pydantic is optional - benchmarks that use it are skipped when not installed.
"""

from __future__ import annotations

import datetime as dt
import decimal
import enum
import sys
import timeit
import uuid
from typing import Any, Literal

import msgspec as msgspec_lib

try:
    import pydantic

    HAS_PYDANTIC = True
except ImportError:
    HAS_PYDANTIC = False
    pydantic = None

import structtype


class Fruit(enum.Enum):
    APPLE = "apple"
    BANANA = "banana"


class Job(enum.IntEnum):
    CREATED = 0
    RUNNING = 1
    SUCCEEDED = 2


class Color(enum.StrEnum):
    RED = "red"
    GREEN = "green"


# (name, annotation, sample value)
FIELD_TYPES = [
    ("int", int, 42),
    ("float", float, 3.14),
    ("str", str, "hello"),
    ("bool", bool, True),
    ("bytes", bytes, b"abc"),
    (
        "datetime",
        dt.datetime,
        dt.datetime(2021, 4, 2, 18, 18, 10, 123, tzinfo=dt.timezone.utc),
    ),
    ("date", dt.date, dt.date(2021, 4, 2)),
    ("time", dt.time, dt.time(18, 18, 10, 123)),
    (
        "timedelta",
        dt.timedelta,
        dt.timedelta(days=1, seconds=30, microseconds=123),
    ),
    ("decimal", decimal.Decimal, decimal.Decimal("1.2345")),
    ("uuid", uuid.UUID, uuid.UUID("c4524ac0-e81e-4aa8-a595-0aec605a659a")),
    ("enum", Fruit, Fruit.APPLE),
    ("intenum", Job, Job.RUNNING),
    ("strenum", Color, Color.RED),
    ("literal", Literal["a", "b", "c"], "a"),
    ("list", list[str], ["a", "b", "c"]),
    ("tuple_fixed", tuple[int, str], (1, "x")),
    ("tuple_var", tuple[int, ...], (1, 2, 3)),
    ("set", set[int], {1, 2, 3}),
    ("frozenset", frozenset[int], frozenset({1, 2, 3})),
    ("mapping", dict[str, int], {"a": 1, "b": 2}),
    ("optional", int | None, 5),
    ("union", int | str, 5),
]

FIELD_NAMES = [name for name, _, _ in FIELD_TYPES]
MICRO_VALUES = {name: value for name, _, value in FIELD_TYPES}


# ── Nested type ──


class Nested_st(structtype.Struct):
    x: int
    y: int


class Nested_ms(msgspec_lib.Struct):
    x: int
    y: int


class Nested_pd(pydantic.BaseModel):
    x: int
    y: int


# ── Kitchen-sink structs ──


def sink_annos(nested_type):
    annos = {name: anno for name, anno, _ in FIELD_TYPES}
    annos["any"] = Any
    annos["nested"] = nested_type
    return annos


SINK_ST = type(
    "AllTypes", (structtype.Struct,), {"__annotations__": sink_annos(Nested_st)}
)
SINK_MS = type(
    "AllTypes", (msgspec_lib.Struct,), {"__annotations__": sink_annos(Nested_ms)}
)
SINK_PD = type(
    "AllTypes", (pydantic.BaseModel,), {"__annotations__": sink_annos(Nested_pd)}
)

SINK_DICT = dict(MICRO_VALUES)
SINK_DICT["any"] = {"anything": 123}
SINK_DICT["nested"] = {"x": 1, "y": 2}

sink_st = SINK_ST.struct_validate(SINK_DICT)
sink_ms = msgspec_lib.convert(SINK_DICT, SINK_MS)
sink_pd = SINK_PD.model_validate(SINK_DICT)

sink_json_st = sink_st.struct_dump_json()
sink_json_ms = msgspec_lib.json.encode(sink_ms)
sink_json_pd = sink_pd.model_dump_json().encode()


# ── Per-type micro benchmarks ──

# Each field type gets a struct with 10 identical fields, so the per-type cost
# is amplified ~10x while the struct's fixed overhead stays roughly constant.
# A single shared sample value is reused for all 10 fields.

MICRO_ST = {}
MICRO_MS = {}
MICRO_PD = {}
MICRO_DICT = {}
micro_objs_st = {}
micro_objs_ms = {}
micro_objs_pd = {}
micro_json_st = {}
micro_json_ms = {}
micro_json_pd = {}
for name, anno, value in FIELD_TYPES:
    annos = {f"f{i}": anno for i in range(10)}
    cls_st = type(f"Micro_{name}", (structtype.Struct,), {"__annotations__": annos})
    cls_ms = type(f"Micro_{name}", (msgspec_lib.Struct,), {"__annotations__": annos})
    cls_pd = type(f"Micro_{name}", (pydantic.BaseModel,), {"__annotations__": annos})
    MICRO_ST[name] = cls_st
    MICRO_MS[name] = cls_ms
    MICRO_PD[name] = cls_pd

    d = {f"f{i}": value for i in range(10)}
    MICRO_DICT[name] = d
    micro_objs_st[name] = cls_st.struct_validate(d)
    micro_objs_ms[name] = msgspec_lib.convert(d, cls_ms)
    micro_objs_pd[name] = cls_pd.model_validate(d)

    # Each lib decodes its own JSON output
    micro_json_st[name] = micro_objs_st[name].struct_dump_json()
    micro_json_ms[name] = msgspec_lib.json.encode(micro_objs_ms[name])
    micro_json_pd[name] = micro_objs_pd[name].model_dump_json().encode()


# ── Nested struct (basic types) ──

# A nested struct containing mostly basic types (str, int, float), used as a
# field type in a parent struct with 10 identical nested fields so the nesting
# cost is amplified like the per-type micro-benchmarks.

BASIC_FIELDS = [
    ("id", int),
    ("name", str),
    ("street", str),
    ("city", str),
    ("region", str),
    ("country", str),
    ("zip", int),
    ("lat", float),
    ("lng", float),
    ("elevation", float),
    ("population", int),
    ("timezone", str),
    ("founded", int),
    ("area", float),
    ("active", bool),
]

Basic_st = type("Basic", (structtype.Struct,), {"__annotations__": dict(BASIC_FIELDS)})
Basic_ms = type("Basic", (msgspec_lib.Struct,), {"__annotations__": dict(BASIC_FIELDS)})
Basic_pd = type("Basic", (pydantic.BaseModel,), {"__annotations__": dict(BASIC_FIELDS)})

BASIC_DICT = {
    "id": 1,
    "name": "Alice Annabel Montgomery",
    "street": "1234 Main Street, Apartment 56, Charlottenburg",
    "city": "Berlin",
    "region": "BE",
    "country": "Germany",
    "zip": 10115,
    "lat": 52.52,
    "lng": 13.40,
    "elevation": 34.0,
    "population": 3600000,
    "timezone": "Europe/Berlin",
    "founded": 1237,
    "area": 891.7,
    "active": True,
}


def holder_annos(nested_type):
    return {f"a{i}": nested_type for i in range(10)}


HOLDER_ST = type(
    "Holder", (structtype.Struct,), {"__annotations__": holder_annos(Basic_st)}
)
HOLDER_MS = type(
    "Holder", (msgspec_lib.Struct,), {"__annotations__": holder_annos(Basic_ms)}
)
HOLDER_PD = type(
    "Holder", (pydantic.BaseModel,), {"__annotations__": holder_annos(Basic_pd)}
)

HOLDER_DICT = {f"a{i}": BASIC_DICT for i in range(10)}

holder_st = HOLDER_ST.struct_validate(HOLDER_DICT)
holder_ms = msgspec_lib.convert(HOLDER_DICT, HOLDER_MS)
holder_pd = HOLDER_PD.model_validate(HOLDER_DICT)

holder_json_st = holder_st.struct_dump_json()
holder_json_ms = msgspec_lib.json.encode(holder_ms)
holder_json_pd = holder_pd.model_dump_json().encode()


# ── Benchmarking ──


def measure(fn, number, repeat=3):
    """Return the best per-iteration time (seconds) for calling ``fn()``."""
    best = min(timeit.repeat(fn, number=number, repeat=repeat))
    return best / number


def print_header():
    print(f"Python {sys.version}")
    print(
        f"structtype {structtype.__version__}, "
        f"msgspec {msgspec_lib.__version__}, "
        f"pydantic {pydantic.__version__ if HAS_PYDANTIC else '(not installed)'}"
    )
    print()


def bench_op(title, st_fn, ms_fn, pd_fn, number):
    """Benchmark a single operation on the kitchen-sink struct."""
    t_st = measure(st_fn, number)
    t_ms = measure(ms_fn, number)
    t_pd = measure(pd_fn, number) if HAS_PYDANTIC else None
    best = min(t for t in (t_st, t_ms, t_pd) if t is not None)

    print(f"\n{title}")
    print("-" * 55)
    for label, t in [("structtype", t_st), ("msgspec", t_ms), ("pydantic", t_pd)]:
        if t is None:
            print(f"  {label:<18} {'(not installed)':>20}")
        else:
            print(f"  {label:<18} {t * 1e6:8.1f} μs   ({t / best:.2f}x)")


def bench_micro_table(title, st_fn, ms_fn, pd_fn, number):
    """Benchmark a single operation on each per-type micro struct."""
    print(f"\n{title}")
    print("-" * 74)
    print(f"  {'field type':<15}{'structtype':>19}{'msgspec':>19}{'pydantic':>19}")
    for name in FIELD_NAMES:
        t_st = measure(st_fn(name), number)
        t_ms = measure(ms_fn(name), number)
        t_pd = measure(pd_fn(name), number) if HAS_PYDANTIC else None
        best = min(t for t in (t_st, t_ms, t_pd) if t is not None)
        cells = [
            f"{t_st * 1e6:8.1f} μs ({t_st / best:.2f}x)",
            f"{t_ms * 1e6:8.1f} μs ({t_ms / best:.2f}x)",
            "          (n/a)"
            if t_pd is None
            else f"{t_pd * 1e6:8.1f} μs ({t_pd / best:.2f}x)",
        ]
        print(f"  {name:<15}" + "".join(f"{c:>19}" for c in cells))


# ── Main ──


print_header()

print("Kitchen-sink struct (one field per type, incl. any + nested)")
print("=" * 55)
bench_op(
    "Construct (kwargs -> instance)",
    lambda: SINK_ST(**SINK_DICT),
    lambda: SINK_MS(**SINK_DICT),
    lambda: SINK_PD(**SINK_DICT),
    500,
)
bench_op(
    "Dict validate (dict -> struct)",
    lambda: SINK_ST.struct_validate(SINK_DICT),
    lambda: msgspec_lib.convert(SINK_DICT, SINK_MS),
    lambda: SINK_PD.model_validate(SINK_DICT),
    500,
)
bench_op(
    "Dict dump (struct -> dict)",
    lambda: sink_st.struct_dump(),
    lambda: msgspec_lib.to_builtins(sink_ms),
    lambda: sink_pd.model_dump(mode="json"),
    500,
)
bench_op(
    "JSON encode (struct -> bytes)",
    lambda: sink_st.struct_dump_json(),
    lambda: msgspec_lib.json.encode(sink_ms),
    lambda: sink_pd.model_dump_json(),
    500,
)
bench_op(
    "JSON decode (bytes -> struct)",
    lambda: SINK_ST.struct_validate_json(sink_json_st),
    lambda: msgspec_lib.json.decode(sink_json_ms, type=SINK_MS),
    lambda: SINK_PD.model_validate_json(sink_json_pd),
    500,
)

print()
print("Nested struct (10x basic-type struct)")
print("=" * 55)
bench_op(
    "Construct (kwargs -> instance)",
    lambda: HOLDER_ST(**HOLDER_DICT),
    lambda: HOLDER_MS(**HOLDER_DICT),
    lambda: HOLDER_PD(**HOLDER_DICT),
    5000,
)
bench_op(
    "Dict validate (dict -> struct)",
    lambda: HOLDER_ST.struct_validate(HOLDER_DICT),
    lambda: msgspec_lib.convert(HOLDER_DICT, HOLDER_MS),
    lambda: HOLDER_PD.model_validate(HOLDER_DICT),
    5000,
)
bench_op(
    "Dict dump (struct -> dict)",
    lambda: holder_st.struct_dump(),
    lambda: msgspec_lib.to_builtins(holder_ms),
    lambda: holder_pd.model_dump(mode="json"),
    5000,
)
bench_op(
    "JSON encode (struct -> bytes)",
    lambda: holder_st.struct_dump_json(),
    lambda: msgspec_lib.json.encode(holder_ms),
    lambda: holder_pd.model_dump_json(),
    5000,
)
bench_op(
    "JSON decode (bytes -> struct)",
    lambda: HOLDER_ST.struct_validate_json(holder_json_st),
    lambda: msgspec_lib.json.decode(holder_json_ms, type=HOLDER_MS),
    lambda: HOLDER_PD.model_validate_json(holder_json_pd),
    5000,
)

print()
print("Per-type micro-benchmarks")
print("=" * 74)
bench_micro_table(
    "Construct (10-field struct)",
    lambda name: lambda: MICRO_ST[name](**MICRO_DICT[name]),
    lambda name: lambda: MICRO_MS[name](**MICRO_DICT[name]),
    lambda name: lambda: MICRO_PD[name](**MICRO_DICT[name]),
    50000,
)
bench_micro_table(
    "Dict validate (dict -> struct)",
    lambda name: lambda: MICRO_ST[name].struct_validate(MICRO_DICT[name]),
    lambda name: lambda: msgspec_lib.convert(MICRO_DICT[name], MICRO_MS[name]),
    lambda name: lambda: MICRO_PD[name].model_validate(MICRO_DICT[name]),
    50000,
)
bench_micro_table(
    "Dict dump (struct -> dict)",
    lambda name: lambda: micro_objs_st[name].struct_dump(),
    lambda name: lambda: msgspec_lib.to_builtins(micro_objs_ms[name]),
    lambda name: lambda: micro_objs_pd[name].model_dump(mode="json"),
    50000,
)
bench_micro_table(
    "JSON encode (struct -> bytes)",
    lambda name: lambda: micro_objs_st[name].struct_dump_json(),
    lambda name: lambda: msgspec_lib.json.encode(micro_objs_ms[name]),
    lambda name: lambda: micro_objs_pd[name].model_dump_json(),
    50000,
)
bench_micro_table(
    "JSON decode (bytes -> struct)",
    lambda name: lambda: MICRO_ST[name].struct_validate_json(micro_json_st[name]),
    lambda name: (
        lambda: msgspec_lib.json.decode(micro_json_ms[name], type=MICRO_MS[name])
    ),
    lambda name: lambda: MICRO_PD[name].model_validate_json(micro_json_pd[name]),
    50000,
)
