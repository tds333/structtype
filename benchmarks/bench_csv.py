# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "structtype",
# ]
#
# [tool.uv.sources]
# structtype = { path = "..", editable = true }
# ///

"""Benchmark structtype's CSV codec against the stdlib ``csv`` module.

The point is the overhead structtype adds on top of CSV tokenizing and
quoting, not another library:

- Decode: ``struct_validate_csv`` (text -> validated Structs) vs
  ``csv.reader`` (text -> row lists) vs ``csv.DictReader`` (text -> dicts).
- Dump: ``struct_dump_csv`` (Structs -> text) vs ``csv.writer`` row lists vs
  ``csv.DictWriter`` mappings.

All paths process the same N flat-scalar rows. The dump reference is
``csv.writer`` with per-cell ``str()`` rendering — the equivalent hand-written
approach, including the scalar-to-text work structtype also must do.
"""

import csv
import io
import sys
import timeit

import structtype
from structtype import Struct


class Record(Struct):
    id: int
    name: str
    ratio: float
    active: bool
    score: float | None


N = 1000
records = [
    Record(
        id=i + 1,
        name=f"user-{i}",
        ratio=(i % 100) / 100,
        active=(i % 2 == 0),
        score=None if i % 3 == 0 else (i % 1000) / 10,
    )
    for i in range(N)
]
aliases = Record.__struct_alias_fields__

# One CSV row per record, no header (what structtype consumes).
_layout = io.StringIO(newline="")
_layout_writer = csv.writer(_layout, lineterminator="\n")
for _rec in records:
    _rec.struct_dump_csv(_layout_writer)
body = _layout.getvalue()
header = ",".join(aliases) + "\n"

# Parsed cell strings, reused for the DictWriter baseline.
cells = list(csv.reader(io.StringIO(body, newline="")))
dict_rows = [dict(zip(aliases, row)) for row in cells]


def decode_structs():
    reader = csv.reader(io.StringIO(body, newline=""))
    return list(Record.struct_validate_csv(reader))


def decode_rows():
    return list(csv.reader(io.StringIO(body, newline="")))


def decode_dicts():
    return list(csv.DictReader(io.StringIO(header + body, newline="")))


def dump_structs():
    stream = io.StringIO(newline="")
    writer = csv.writer(stream, lineterminator="\n")
    for rec in records:
        rec.struct_dump_csv(writer)
    return stream.getvalue()


def _render(value):
    if value is None:
        return ""
    if value is True:
        return "true"
    if value is False:
        return "false"
    return str(value)


def dump_rows():
    stream = io.StringIO(newline="")
    writer = csv.writer(stream, lineterminator="\n")
    for r in records:
        writer.writerow(
            [
                _render(r.id),
                _render(r.name),
                _render(r.ratio),
                _render(r.active),
                _render(r.score),
            ]
        )
    return stream.getvalue()


def dump_dicts():
    stream = io.StringIO(newline="")
    writer = csv.DictWriter(stream, fieldnames=aliases, lineterminator="\n")
    for row in dict_rows:
        writer.writerow(row)
    return stream.getvalue()


# Self-validating: every path must agree on the same data.
assert decode_structs() == records
assert decode_rows() == cells
assert decode_dicts() == dict_rows
assert dump_structs() == body
assert dump_rows() == body
assert dump_dicts() == body


print(f"Python {sys.version}")
print(f"structtype {structtype.__version__}")
print()
print(f"CSV vs stdlib csv, {N} records/op")
print("-" * 47)


def bench(label, fn, baseline=None, *, number=50):
    t = min(timeit.repeat(fn, number=number, repeat=3)) / number
    ratio = "" if baseline is None else f"   ({t / baseline:5.2f}x)"
    print(f"  {label:<20} {t * 1e6:9.1f} μs{ratio}")
    return t


base = bench("csv.reader", decode_rows)
bench("csv.DictReader", decode_dicts, baseline=base)
bench("structtype decode", decode_structs, baseline=base)
print()
base = bench("csv.writer (+ str/cell)", dump_rows)
bench("csv.DictWriter", dump_dicts, baseline=base)
bench("structtype dump", dump_structs, baseline=base)
