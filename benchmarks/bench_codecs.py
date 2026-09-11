# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "pydantic",
#     "structtype",
# ]
#
# [tool.uv.sources]
# structtype = { path = "..", editable = true }
# ///

"""Benchmark structtype's Constraint + Serializer annotations against pydantic.

A single wide record schema carries a constraint and/or serializer on every
field, spanning numeric, string, bytes, collection, timezone and custom-type
codecs (UUID, Decimal, date, enum, a custom class, and ``set[PostalCode]``):

- numeric constraints (``NumericConstraint`` <-> ``Field(gt=, ge=, lt=, le=)``)
- string constraints (``StrConstraint`` <-> ``Field(min_length=, pattern=)``)
- bytes constraints (``BytesConstraint`` <-> ``Field(min_length=)``)
- collection constraints (``CollectionConstraint`` <-> ``Field(min_length=)``)
- timezone (``TimezoneConstraint`` <-> ``AwareDatetime``)
- per-field Serializers for UUID / Decimal / date / enum / a custom class
  (``Serializer(dump=, load=)`` <-> ``@field_serializer`` / ``@field_validator``)

Some fields combine a Serializer *and* a Constraint, and two fields are optional
using the supported ``Annotated[T, codec] | None`` form (the codec attaches to
the concrete member, never to the union itself).

Operations:

- Load (dict -> object): ``struct_validate`` vs ``model_validate``
- Dump (object -> dict): ``struct_dump`` vs ``model_dump``
- Load JSON (bytes -> object): ``struct_validate_json`` vs ``model_validate_json``
- Dump JSON (object -> bytes): ``struct_dump_json`` vs ``model_dump_json``
- Init (no validation): structtype validates on init only when
  ``check_types_on_init=True``; pydantic always validates on construction.
- Init (with validation): the same with ``check_types_on_init=True``.

pydantic is optional — benchmarks that use it are skipped when not installed.
"""

import datetime as dt
import decimal
import enum
import random
import string
import sys
import timeit
import uuid
from typing import Annotated

try:
    import pydantic
    from pydantic import (
        AwareDatetime,
        BaseModel,
        Field,
        field_serializer,
        field_validator,
    )

    HAS_PYDANTIC = True
except ImportError:
    HAS_PYDANTIC = False
    pydantic = None

import structtype
from structtype import (
    BytesConstraint,
    CollectionConstraint,
    NumericConstraint,
    Serializer,
    StrConstraint,
    Struct,
    StructConfig,
    TimezoneConstraint,
)

UTC = dt.timezone.utc


def print_header():
    print(f"Python {sys.version}")
    print(
        f"structtype {structtype.__version__}, "
        f"pydantic {pydantic.__version__ if HAS_PYDANTIC else '(not installed)'}"
    )
    print()


# ── Custom type (converted via a serializer codec) ──


class PostalCode:
    __slots__ = ("code",)

    def __init__(self, code: str):
        self.code = code

    def __repr__(self):
        return f"PostalCode({self.code!r})"

    def __eq__(self, other):
        return isinstance(other, PostalCode) and self.code == other.code

    def __hash__(self):
        return hash(self.code)


def dump_zip(p: PostalCode) -> str:
    return p.code


def load_zip(v) -> PostalCode:
    return v if isinstance(v, PostalCode) else PostalCode(v)


class Color(enum.Enum):
    RED = "red"
    GREEN = "green"
    BLUE = "blue"


# ── Schemas ──


class Point(Struct):
    x: int
    y: int


class Record_st(Struct):
    # numeric constraints
    id: Annotated[int, NumericConstraint(ge=1, le=10**6)]
    priority: Annotated[int, NumericConstraint(multiple_of=5)]
    amount: Annotated[float, NumericConstraint(gt=0, le=10**5)]
    ratio: Annotated[float, NumericConstraint(ge=0, lt=1)]

    # string constraints
    name: Annotated[
        str, StrConstraint(min_length=1, max_length=64, pattern="^[a-zA-Z ]+$")
    ]
    sku: Annotated[str, StrConstraint(pattern="^[A-Z0-9-]+$")]

    # bytes + collection constraints
    digest: Annotated[bytes, BytesConstraint(min_length=4, max_length=32)]
    tags: Annotated[list[str], CollectionConstraint(min_length=1, max_length=10)]
    scores: Annotated[set[int], CollectionConstraint(min_length=1, max_length=100)]
    attrs: Annotated[dict[str, int], CollectionConstraint(min_length=1, max_length=10)]

    # timezone constraint
    created: Annotated[dt.datetime, TimezoneConstraint(tz=True)]

    # constraint + serializer on the same field
    stamp: Annotated[
        dt.datetime,
        TimezoneConstraint(tz=True),
        Serializer(
            dump=lambda d: int(d.timestamp()),
            load=lambda v: dt.datetime.fromtimestamp(v, UTC),
        ),
    ]
    blob: Annotated[
        bytes,
        BytesConstraint(min_length=2),
        Serializer(dump=lambda b: b.hex(), load=bytes.fromhex),
    ]
    zips: Annotated[
        set[PostalCode],
        CollectionConstraint(min_length=1, max_length=5),
        Serializer(
            dump=lambda s: sorted(p.code for p in s),
            load=lambda v: {load_zip(x) for x in v},
        ),
    ]

    # serializers on native / custom types
    uid: Annotated[uuid.UUID, Serializer(dump=str, load=uuid.UUID)]
    price: Annotated[decimal.Decimal, Serializer(dump=str, load=decimal.Decimal)]
    when: Annotated[
        dt.date, Serializer(dump=lambda d: d.isoformat(), load=dt.date.fromisoformat)
    ]
    color: Annotated[Color, Serializer(dump=lambda c: c.value, load=Color)]

    # nested struct
    nested: Point

    # optional fields: the codec attaches to the concrete member, then `| None`
    note: Annotated[str, StrConstraint(max_length=128)] | None = None
    label: Annotated[PostalCode, Serializer(dump=dump_zip, load=load_zip)] | None = None


class Record_st_v(Record_st):
    struct_config = StructConfig(check_types_on_init=True)


if HAS_PYDANTIC:

    class PointPD(BaseModel):
        x: int
        y: int

    class Record_pd(BaseModel):
        model_config = {"arbitrary_types_allowed": True}

        id: Annotated[int, Field(ge=1, le=10**6)]
        priority: Annotated[int, Field(multiple_of=5)]
        amount: Annotated[float, Field(gt=0, le=10**5)]
        ratio: Annotated[float, Field(ge=0, lt=1)]

        name: Annotated[str, Field(min_length=1, max_length=64, pattern="^[a-zA-Z ]+$")]
        sku: Annotated[str, Field(pattern="^[A-Z0-9-]+$")]

        digest: Annotated[bytes, Field(min_length=4, max_length=32)]
        tags: Annotated[list[str], Field(min_length=1, max_length=10)]
        scores: Annotated[set[int], Field(min_length=1, max_length=100)]
        attrs: Annotated[dict[str, int], Field(min_length=1, max_length=10)]

        created: AwareDatetime
        stamp: AwareDatetime
        blob: bytes
        zips: Annotated[set[PostalCode], Field(min_length=1, max_length=5)]

        uid: uuid.UUID
        price: decimal.Decimal
        when: dt.date
        color: Color

        nested: PointPD

        note: Annotated[str, Field(max_length=128)] | None = None
        label: PostalCode | None = None

        @field_serializer("stamp")
        def _dump_stamp(self, v):
            return int(v.timestamp())

        @field_serializer("blob")
        def _dump_blob(self, v):
            return v.hex()

        @field_serializer("zips")
        def _dump_zips(self, v):
            return sorted(p.code for p in v)

        @field_serializer("uid")
        def _dump_uid(self, v):
            return str(v)

        @field_serializer("price")
        def _dump_price(self, v):
            return str(v)

        @field_serializer("when")
        def _dump_when(self, v):
            return v.isoformat()

        @field_serializer("color")
        def _dump_color(self, v):
            return v.value

        @field_serializer("label")
        def _dump_label(self, v):
            return None if v is None else v.code

        @field_validator("stamp", mode="before")
        @classmethod
        def _load_stamp(cls, v):
            return dt.datetime.fromtimestamp(v, UTC) if isinstance(v, int) else v

        @field_validator("blob", mode="before")
        @classmethod
        def _load_blob(cls, v):
            return bytes.fromhex(v) if isinstance(v, str) else v

        @field_validator("zips", mode="before")
        @classmethod
        def _load_zips(cls, v):
            return {load_zip(x) for x in v}

        @field_validator("uid", mode="before")
        @classmethod
        def _load_uid(cls, v):
            return uuid.UUID(v) if isinstance(v, str) else v

        @field_validator("price", mode="before")
        @classmethod
        def _load_price(cls, v):
            return decimal.Decimal(v) if isinstance(v, str) else v

        @field_validator("when", mode="before")
        @classmethod
        def _load_when(cls, v):
            return dt.date.fromisoformat(v) if isinstance(v, str) else v

        @field_validator("color", mode="before")
        @classmethod
        def _load_color(cls, v):
            return Color(v) if isinstance(v, str) else v

        @field_validator("label", mode="before")
        @classmethod
        def _load_label(cls, v):
            return None if v is None else load_zip(v)


# ── Test data generation ──


def make_raw_records(n=500):
    names = ["Alice Smith", "Bob Johnson", "Carol Lee", "Dave Brown", "Eve Davis"]
    tags_pool = ["red", "blue", "green", "sale", "new"]

    records = []
    for i in range(n):
        length = random.randint(4, 16)
        records.append(
            {
                "id": i + 1,
                "priority": random.randint(0, 200) * 5,
                "amount": round(random.uniform(1.0, 500.0), 2),
                "ratio": round(random.uniform(0.0, 0.999), 3),
                "name": random.choice(names),
                "sku": f"{random.choice('ABCDE')}{random.randint(1000, 9999)}",
                "digest": "".join(
                    random.choices(string.ascii_letters, k=length)
                ).encode(),
                "tags": random.sample(tags_pool, random.randint(1, 4)),
                "scores": {
                    random.randint(0, 1000) for _ in range(random.randint(1, 8))
                },
                "attrs": {f"k{j}": j for j in range(random.randint(1, 5))},
                "created": dt.datetime(2020, 1, 1, tzinfo=UTC)
                + dt.timedelta(seconds=i),
                "stamp": dt.datetime(2020, 1, 1, tzinfo=UTC) + dt.timedelta(seconds=i),
                "blob": bytes(
                    random.getrandbits(8) for _ in range(random.randint(2, 8))
                ),
                "zips": [
                    f"{random.randint(10000, 99999)}"
                    for _ in range(random.randint(1, 5))
                ],
                "uid": str(uuid.UUID(int=random.getrandbits(128))),
                "price": str(decimal.Decimal(random.randrange(1, 100_000)) / 100),
                "when": dt.date(2020, 1, 1).isoformat(),
                "color": random.choice([c.value for c in Color]),
                "nested": {"x": i, "y": i * 2},
                "note": random.choice([None, "rush", "gift wrap"]),
                "label": random.choice([None, f"{random.randint(10000, 99999)}"]),
            }
        )
    return records


random.seed(1234)
raw_records = make_raw_records(500)

record_st = [Record_st.struct_validate(r) for r in raw_records]
# `check_types_on_init` is a pure type-check and never applies Serializer.load,
# so the constructor args must already hold the converted instances.
init_st = [dict(o) for o in record_st]

if HAS_PYDANTIC:
    record_pd = [Record_pd.model_validate(r) for r in raw_records]
    init_pd = [dict(o) for o in record_pd]
    json_pd = [o.model_dump_json() for o in record_pd]
else:
    record_pd = []
    init_pd = []
    json_pd = []

json_st = [o.struct_dump_json() for o in record_st]


# ── Sanity check: roundtrip both libraries before measuring ──

for raw, obj, js in zip(raw_records, record_st, json_st):
    assert Record_st.struct_validate_json(js) == obj, raw["id"]
if HAS_PYDANTIC:
    for raw, obj, js in zip(raw_records, record_pd, json_pd):
        assert Record_pd.model_validate_json(js) == obj, raw["id"]

# Both optional-member fields must exercise the None and non-None paths.
assert any(o.note is None for o in record_st)
assert any(o.note is not None for o in record_st)
assert any(o.label is None for o in record_st)
assert any(o.label is not None for o in record_st)


# ── Benchmarking ──


print_header()


def bench(name, st_fn, st_data, pd_fn=None, pd_data=None, *, n=200):
    t_st = min(timeit.repeat(lambda: st_fn(st_data), number=n, repeat=3)) / n
    if pd_fn is not None:
        t_pd = min(timeit.repeat(lambda: pd_fn(pd_data), number=n, repeat=3)) / n
    else:
        t_pd = None

    best = min(t for t in [t_st, t_pd] if t is not None)
    rows = [("structtype", t_st), ("pydantic", t_pd)]
    print(f"\n{name}")
    print("-" * 55)
    for label, t in rows:
        if t is None:
            print(f"  {label:<18} {'(not installed)':>20}")
        else:
            print(f"  {label:<18} {t * 1e6:8.1f} μs   ({t / best:.2f}x)")


# Load: dict -> object (constraints + serializers on input)
bench(
    "Load (dict -> object)",
    lambda data: [Record_st.struct_validate(r) for r in data],
    raw_records,
    pd_fn=(lambda data: [Record_pd.model_validate(r) for r in data])
    if HAS_PYDANTIC
    else None,
    pd_data=raw_records if HAS_PYDANTIC else None,
)

# Dump: object -> dict (constraints + dump serializers)
bench(
    "Dump (object -> dict)",
    lambda data: [o.struct_dump() for o in data],
    record_st,
    pd_fn=(lambda data: [o.model_dump() for o in data]) if HAS_PYDANTIC else None,
    pd_data=record_pd if HAS_PYDANTIC else None,
)

# Load JSON: bytes -> object
bench(
    "Load JSON (bytes -> object)",
    lambda data: [Record_st.struct_validate_json(b) for b in data],
    json_st,
    pd_fn=(lambda data: [Record_pd.model_validate_json(b) for b in data])
    if HAS_PYDANTIC
    else None,
    pd_data=json_pd if HAS_PYDANTIC else None,
)

# Dump JSON: object -> bytes
bench(
    "Dump JSON (object -> bytes)",
    lambda data: [o.struct_dump_json() for o in data],
    record_st,
    pd_fn=(lambda data: [o.model_dump_json() for o in data]) if HAS_PYDANTIC else None,
    pd_data=record_pd if HAS_PYDANTIC else None,
)

# Init (constructor only). structtype validates on init only with
# check_types_on_init=True; pydantic always validates on construction.
bench(
    "Init (no validation)",
    lambda data: [Record_st(**d) for d in data],
    init_st,
    pd_fn=(lambda data: [Record_pd(**d) for d in data]) if HAS_PYDANTIC else None,
    pd_data=init_pd if HAS_PYDANTIC else None,
)

# Init with validation on both sides
bench(
    "Init (with validation)",
    lambda data: [Record_st_v(**d) for d in data],
    init_st,
    pd_fn=(lambda data: [Record_pd(**d) for d in data]) if HAS_PYDANTIC else None,
    pd_data=init_pd if HAS_PYDANTIC else None,
)
