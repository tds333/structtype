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

"""Compare structtype, msgspec, and pydantic across multiple operations.

Benchmarks: dump/load dict round-trip, JSON round-trip (e-commerce data),
and JSON round-trip with tagged unions (filesystem data).

pydantic is optional — benchmarks that use it are skipped when not installed.
"""

from __future__ import annotations

import sys
import timeit

import msgspec as msgspec_lib

try:
    import pydantic
    HAS_PYDANTIC = True
except ImportError:
    HAS_PYDANTIC = False
    pydantic = None

import structtype


def print_header():
    print(f"Python {sys.version}")
    print(
        f"structtype {structtype.__version__}, "
        f"msgspec {msgspec_lib.__version__}, "
        f"pydantic {pydantic.__version__ if HAS_PYDANTIC else '(not installed)'}"
    )
    print()

# ── Schemas ──


class Item_st(structtype.Struct):
    name: str
    price: float
    tags: list[str] = []
    metadata: dict[str, str] | None = None


class Order_st(structtype.Struct, kw_only=True):
    id: int
    customer: str
    items: list[Item_st]
    created_at: str
    status: str = "pending"


class Item_ms(msgspec_lib.Struct):
    name: str
    price: float
    tags: list[str] = []
    metadata: dict[str, str] | None = None


class Order_ms(msgspec_lib.Struct, kw_only=True):
    id: int
    customer: str
    items: list[Item_ms]
    created_at: str
    status: str = "pending"


if HAS_PYDANTIC:

    class Item_pd(pydantic.BaseModel):
        name: str
        price: float
        tags: list[str] = []
        metadata: dict[str, str] | None = None

    class Order_pd(pydantic.BaseModel):
        id: int
        customer: str
        items: list[Item_pd]
        status: str = "pending"
        created_at: str


# ── Tagged union schemas ──

from typing import Annotated, Literal


class File_st(structtype.Struct, tag="file", kw_only=True):
    name: str
    size: int


class Dir_st(structtype.Struct, tag="dir", kw_only=True):
    name: str
    contents: list[File_st | Dir_st]


class File_ms(msgspec_lib.Struct, tag="file", kw_only=True):
    name: str
    size: int


class Dir_ms(msgspec_lib.Struct, tag="dir", kw_only=True):
    name: str
    contents: list[File_ms | Dir_ms]


if HAS_PYDANTIC:

    class File_pd(pydantic.BaseModel):
        type: Literal["file"] = "file"
        name: str
        size: int

    class Dir_pd(pydantic.BaseModel):
        type: Literal["dir"] = "dir"
        name: str
        contents: list[Annotated[File_pd | Dir_pd, pydantic.Field(discriminator="type")]]


# ── Test data generation ──

import random


def make_raw_orders(n=500):
    orders = []
    for oid in range(n):
        n_items = random.randint(5, 20)
        items = []
        for _ in range(n_items):
            item = {
                "name": f"item-{random.randint(1, 100)}",
                "price": round(random.uniform(1.0, 100.0), 2),
                "tags": random.sample(["a", "b", "c", "d", "e"], random.randint(1, 5)),
            }
            if random.random() < 0.3:
                item["metadata"] = {"key": "val"}
            items.append(item)
        orders.append(
            {
                "id": oid,
                "customer": f"cust-{random.randint(1, 50)}",
                "items": items,
                "status": random.choice(["pending", "shipped", "delivered"]),
                "created_at": "2024-01-15",
            }
        )
    return orders


raw_orders = make_raw_orders(500)
order_st = [Order_st.struct_validate(r) for r in raw_orders]
order_ms = [msgspec_lib.convert(r, Order_ms) for r in raw_orders]
if HAS_PYDANTIC:
    order_pd = [Order_pd.model_validate(r) for r in raw_orders]
else:
    order_pd = []

# Pre-encoded JSON for decode benchmarks
json_st = [o.struct_dump_json() for o in order_st]
json_ms = [msgspec_lib.json.encode(o) for o in order_ms]
if HAS_PYDANTIC:
    json_pd = [o.model_dump_json().encode() for o in order_pd]
else:
    json_pd = []

def make_tagged_data(depth=4, width=5):
    if depth == 0:
        return {"type": "file", "name": f"f-{depth}", "size": 42}
    return {
        "type": "dir",
        "name": f"d-{depth}",
        "contents": [make_tagged_data(depth - 1, i) for i in range(width)],
    }


# Dict versions for load benchmarks
dicts_raw = raw_orders[:]


# ── Benchmarking ──


print_header()


def bench(name, st_fn, ms_fn, st_data, ms_data, pd_fn=None, pd_data=None):
    n = 200
    t_st = min(timeit.repeat(lambda: st_fn(st_data), number=n, repeat=3)) / n
    t_ms = min(timeit.repeat(lambda: ms_fn(ms_data), number=n, repeat=3)) / n
    if pd_fn is not None:
        t_pd = min(timeit.repeat(lambda: pd_fn(pd_data), number=n, repeat=3)) / n
    else:
        t_pd = None

    best = min(t for t in [t_st, t_ms, t_pd] if t is not None)
    rows = [
        ("structtype", t_st),
        ("msgspec", t_ms),
        ("pydantic", t_pd),
    ]
    print(f"\n{name}")
    print("-" * 55)
    for label, t in rows:
        if t is None:
            print(f"  {label:<18} {'(not installed)':>20}")
        else:
            print(f"  {label:<18} {t * 1e6:8.1f} μs   ({t / best:.2f}x)")


# Dump: struct → dict
bench(
    "Dump (struct → dict)",
    lambda data: [o.struct_dump() for o in data],
    lambda data: [msgspec_lib.to_builtins(o) for o in data],
    order_st,
    order_ms,
    pd_fn=(lambda data: [o.model_dump() for o in data]) if HAS_PYDANTIC else None,
    pd_data=order_pd if HAS_PYDANTIC else None,
)

# Load: dict → struct
bench(
    "Load (dict → struct)",
    lambda data: [Order_st.struct_validate(d) for d in data],
    lambda data: [msgspec_lib.convert(d, Order_ms) for d in data],
    dicts_raw,
    dicts_raw,
    pd_fn=(lambda data: [Order_pd.model_validate(d) for d in data]) if HAS_PYDANTIC else None,
    pd_data=dicts_raw if HAS_PYDANTIC else None,
)

# Dump JSON: struct → bytes
bench(
    "Dump JSON (struct → bytes)",
    lambda data: [o.struct_dump_json() for o in data],
    lambda data: [msgspec_lib.json.encode(o) for o in data],
    order_st,
    order_ms,
    pd_fn=(lambda data: [o.model_dump_json() for o in data]) if HAS_PYDANTIC else None,
    pd_data=order_pd if HAS_PYDANTIC else None,
)

# Load JSON: bytes → struct
bench(
    "Load JSON (bytes → struct)",
    lambda data: [Order_st.struct_validate_json(b) for b in data],
    lambda data: [msgspec_lib.json.decode(b, type=Order_ms) for b in data],
    json_st,
    json_ms,
    pd_fn=(lambda data: [Order_pd.model_validate_json(b) for b in data]) if HAS_PYDANTIC else None,
    pd_data=json_pd if HAS_PYDANTIC else None,
)

# ── Tagged union data ──

tagged_raw = make_tagged_data()
tagged_st = Dir_st.struct_validate(tagged_raw)
tagged_ms = msgspec_lib.convert(tagged_raw, Dir_ms)
if HAS_PYDANTIC:
    tagged_pd = Dir_pd.model_validate(tagged_raw)
else:
    tagged_pd = None

tagged_json_st = tagged_st.struct_dump_json()
tagged_json_ms = msgspec_lib.json.encode(tagged_ms)
if HAS_PYDANTIC:
    tagged_json_pd = tagged_pd.model_dump_json().encode()
else:
    tagged_json_pd = None

# Tagged union: encode JSON
bench(
    "Dump JSON (tagged union)",
    lambda d: d.struct_dump_json(),
    lambda d: msgspec_lib.json.encode(d),
    tagged_st,
    tagged_ms,
    pd_fn=(lambda d: d.model_dump_json()) if HAS_PYDANTIC else None,
    pd_data=tagged_pd if HAS_PYDANTIC else None,
)

# Tagged union: decode JSON
bench(
    "Load JSON (tagged union)",
    lambda b: Dir_st.struct_validate_json(b),
    lambda b: msgspec_lib.json.decode(b, type=Dir_ms),
    tagged_json_st,
    tagged_json_ms,
    pd_fn=(lambda b: Dir_pd.model_validate_json(b)) if HAS_PYDANTIC else None,
    pd_data=tagged_json_pd if HAS_PYDANTIC else None,
)
