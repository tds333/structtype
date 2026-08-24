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

"""Benchmark structtype's Serializer + Constraint annotations against pydantic.

Both libraries validate constrained fields (numeric/string/collection
constraints) and convert a custom type (``PostalCode``) via a serializer
codec. Operations:

- Load (dict -> object): ``struct_validate`` vs ``model_validate``
- Dump (object -> dict): ``struct_dump`` vs ``model_dump``
- Load JSON (bytes -> object): ``struct_validate_json`` vs ``model_validate_json``
- Dump JSON (object -> bytes): ``struct_dump_json`` vs ``model_dump_json``
- Init: constructor-only cost. structtype validates on init only when
  ``check_types_on_init=True``; pydantic always validates on construction.

pydantic is optional — benchmarks that use it are skipped when not installed.
"""

import random
import sys
import timeit
from typing import Annotated

try:
    import pydantic
    from pydantic import BaseModel, Field, field_serializer, field_validator

    HAS_PYDANTIC = True
except ImportError:
    HAS_PYDANTIC = False
    pydantic = None

import structtype
from structtype import (
    CollectionConstraint,
    NumericConstraint,
    Serializer,
    Struct,
    StructConfig,
    StrConstraint,
)


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


def dump_zip(p: PostalCode) -> str:
    return p.code


# ── Schemas ──


class Order_st(Struct):
    id: Annotated[int, NumericConstraint(gt=0, le=10**6)]
    customer: Annotated[
        str, StrConstraint(min_length=1, max_length=64, pattern="^[a-zA-Z ]+$")
    ]
    amount: Annotated[float, NumericConstraint(gt=0, le=10**5)]
    quantity: Annotated[int, NumericConstraint(ge=1, le=100)]
    sku: Annotated[str, StrConstraint(pattern="^[A-Z0-9-]+$")]
    tags: Annotated[list[str], CollectionConstraint(min_length=1, max_length=10)]
    zip: Annotated[PostalCode, Serializer(dump=dump_zip, load=PostalCode)]
    note: str | None = None


class Order_st_v(Order_st):
    struct_config = StructConfig(check_types_on_init=True)


if HAS_PYDANTIC:

    class Order_pd(BaseModel):
        model_config = {"arbitrary_types_allowed": True}
        id: Annotated[int, Field(gt=0, le=10**6)]
        customer: Annotated[
            str, Field(min_length=1, max_length=64, pattern="^[a-zA-Z ]+$")
        ]
        amount: Annotated[float, Field(gt=0, le=10**5)]
        quantity: Annotated[int, Field(ge=1, le=100)]
        sku: Annotated[str, Field(pattern="^[A-Z0-9-]+$")]
        tags: Annotated[list[str], Field(min_length=1, max_length=10)]
        zip: PostalCode
        note: str | None = None

        @field_serializer("zip")
        def _dump_zip(self, v: PostalCode) -> str:
            return v.code

        @field_validator("zip", mode="before")
        @classmethod
        def _load_zip(cls, v):
            if isinstance(v, PostalCode):
                return v
            return PostalCode(v)


# ── Test data generation ──


def make_raw_orders(n=500):
    names = ["Alice Smith", "Bob Johnson", "Carol Lee", "Dave Brown", "Eve Davis"]
    orders = []
    for oid in range(n):
        orders.append(
            {
                "id": oid + 1,
                "customer": random.choice(names),
                "amount": round(random.uniform(1.0, 500.0), 2),
                "quantity": random.randint(1, 100),
                "sku": f"{random.choice('ABCDE')}{random.randint(1000, 9999)}",
                "tags": random.sample(
                    ["red", "blue", "green", "sale", "new"], random.randint(1, 4)
                ),
                "zip": f"{random.randint(10000, 99999)}",
                "note": random.choice([None, "rush", "gift wrap"]),
            }
        )
    return orders


raw_orders = make_raw_orders(500)
order_st = [Order_st.struct_validate(r) for r in raw_orders]
order_st_v = [Order_st_v.struct_validate(r) for r in raw_orders]
if HAS_PYDANTIC:
    order_pd = [Order_pd.model_validate(r) for r in raw_orders]
else:
    order_pd = []

# Pre-encoded JSON for decode benchmarks
json_st = [o.struct_dump_json() for o in order_st]
if HAS_PYDANTIC:
    json_pd = [o.model_dump_json() for o in order_pd]
else:
    json_pd = []

# Constructor args with `zip` already a PostalCode instance (check_types_on_init
# is a pure type-check and does not apply Serializer.load).
init_args = [{**d, "zip": PostalCode(d["zip"])} for d in raw_orders]


# ── Benchmarking ──


print_header()


def bench(name, st_fn, ms_data, pd_fn=None, pd_data=None, *, n=200):
    t_st = min(timeit.repeat(lambda: st_fn(ms_data), number=n, repeat=3)) / n
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


# Load: dict -> object (validators on input)
bench(
    "Load (dict -> object)",
    lambda data: [Order_st.struct_validate(d) for d in data],
    raw_orders,
    pd_fn=(lambda data: [Order_pd.model_validate(d) for d in data])
    if HAS_PYDANTIC
    else None,
    pd_data=raw_orders if HAS_PYDANTIC else None,
)

# Dump: object -> dict (dump serializers)
bench(
    "Dump (object -> dict)",
    lambda data: [o.struct_dump() for o in data],
    order_st,
    pd_fn=(lambda data: [o.model_dump() for o in data]) if HAS_PYDANTIC else None,
    pd_data=order_pd if HAS_PYDANTIC else None,
)

# Load JSON: bytes -> object
bench(
    "Load JSON (bytes -> object)",
    lambda data: [Order_st.struct_validate_json(b) for b in data],
    json_st,
    pd_fn=(lambda data: [Order_pd.model_validate_json(b) for b in data])
    if HAS_PYDANTIC
    else None,
    pd_data=json_pd if HAS_PYDANTIC else None,
)

# Dump JSON: object -> bytes
bench(
    "Dump JSON (object -> bytes)",
    lambda data: [o.struct_dump_json() for o in data],
    order_st,
    pd_fn=(lambda data: [o.model_dump_json() for o in data]) if HAS_PYDANTIC else None,
    pd_data=order_pd if HAS_PYDANTIC else None,
)

# Init (constructor only). structtype validates on init only with
# check_types_on_init=True; pydantic always validates on construction.
bench(
    "Init (no validation)",
    lambda data: [Order_st(**d) for d in data],
    init_args,
    pd_fn=(lambda data: [Order_pd(**d) for d in data]) if HAS_PYDANTIC else None,
    pd_data=init_args if HAS_PYDANTIC else None,
)

# Init with validation on both sides
bench(
    "Init (with validation)",
    lambda data: [Order_st_v(**d) for d in data],
    init_args,
    pd_fn=(lambda data: [Order_pd(**d) for d in data]) if HAS_PYDANTIC else None,
    pd_data=init_args if HAS_PYDANTIC else None,
)
