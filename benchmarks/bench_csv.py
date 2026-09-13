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

- Decode, mixed record: ``struct_validate_csv`` (text -> validated Structs) vs
  ``csv.reader`` + per-cell coercion and construction (the equivalent
  hand-written approach) vs plain ``csv.reader`` / ``csv.DictReader``.
- Decode, str-only record: a Struct of only ``str`` fields, compared directly
  against the ``csv.reader`` tokenizer to expose the pure validation +
  construction overhead with no coercion.
- Wide record: a 50-field Struct cycling int/str/float/bool, for scaling with
  column count (both decode and dump).
- Dump: ``struct_dump_csv`` (Structs -> text) vs ``csv.writer`` + per-cell
  ``str()`` rendering (equivalent) vs ``csv.DictWriter``.

All paths process the same N flat-scalar rows. Plain ``csv.reader`` and
``csv.DictReader`` only tokenize: every cell they yield is a ``str`` and no
type conversion happens, so they are reference points, not equivalents. The
equivalent baselines are the hand-written coercion (decode) and rendering
(dump). Each non-baseline row reports its speedup or slowdown relative to the
section's baseline.
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


class StrRecord(Struct):
    a: str
    b: str
    c: str
    d: str
    e: str


# A 50-field record, cycling int/str/float/bool, to show scaling with width.
_WIDE_TYPES = (int, str, float, bool)
_WIDE_COL_TYPES = [_WIDE_TYPES[i % 4] for i in range(50)]
WideRecord = type(
    "WideRecord",
    (Struct,),
    {"__annotations__": {f"f{i}": t for i, t in enumerate(_WIDE_COL_TYPES)}},
)
_WIDE_FIELDS = WideRecord.__struct_fields__


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
    _layout_writer.writerow(_rec.struct_dump_csv())
body = _layout.getvalue()
header = ",".join(aliases) + "\n"

# Parsed cell strings, reused for the DictWriter baseline.
cells = list(csv.reader(io.StringIO(body, newline="")))
dict_rows = [dict(zip(aliases, row)) for row in cells]

# A second, all-str record: isolates validation + construction from coercion,
# so it can be compared directly against the csv.reader tokenizer.
str_records = [StrRecord(f"a{i}", f"b{i}", f"c{i}", f"d{i}", f"e{i}") for i in range(N)]
_str_layout = io.StringIO(newline="")
_str_writer = csv.writer(_str_layout, lineterminator="\n")
for _rec in str_records:
    _str_writer.writerow(_rec.struct_dump_csv())
str_body = _str_layout.getvalue()


def _wide_value(i):
    t = _WIDE_COL_TYPES[i]
    if t is int:
        return i
    if t is str:
        return f"s{i}"
    if t is float:
        return i / 10
    return i % 2 == 0


wide_records = [WideRecord(*[_wide_value(i) for i in range(50)]) for _ in range(N)]
_wide_layout = io.StringIO(newline="")
_wide_writer = csv.writer(_wide_layout, lineterminator="\n")
for _rec in wide_records:
    _wide_writer.writerow(_rec.struct_dump_csv())
wide_body = _wide_layout.getvalue()


def decode_structs():
    reader = csv.reader(io.StringIO(body, newline=""))
    return list(Record.struct_validate_csv(reader))


def decode_rows():
    return list(csv.reader(io.StringIO(body, newline="")))


def decode_dicts():
    return list(csv.DictReader(io.StringIO(header + body, newline="")))


def decode_manual():
    out = []
    for r in csv.reader(io.StringIO(body, newline="")):
        out.append(
            Record(
                int(r[0]),
                r[1],
                float(r[2]),
                r[3] == "true",
                None if r[4] == "" else float(r[4]),
            )
        )
    return out


def decode_str_rows():
    return list(csv.reader(io.StringIO(str_body, newline="")))


def decode_str_manual():
    return [StrRecord(*r) for r in csv.reader(io.StringIO(str_body, newline=""))]


def decode_str_structs():
    reader = csv.reader(io.StringIO(str_body, newline=""))
    return list(StrRecord.struct_validate_csv(reader))


def _coerce_wide(cell, t):
    if t is int:
        return int(cell)
    if t is float:
        return float(cell)
    if t is bool:
        return cell == "true"
    return cell


def decode_wide_manual():
    out = []
    for r in csv.reader(io.StringIO(wide_body, newline="")):
        out.append(
            WideRecord(*[_coerce_wide(c, t) for c, t in zip(r, _WIDE_COL_TYPES)])
        )
    return out


def decode_wide_structs():
    reader = csv.reader(io.StringIO(wide_body, newline=""))
    return list(WideRecord.struct_validate_csv(reader))


def dump_structs():
    stream = io.StringIO(newline="")
    writer = csv.writer(stream, lineterminator="\n")
    for rec in records:
        writer.writerow(rec.struct_dump_csv())
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


def dump_wide_structs():
    stream = io.StringIO(newline="")
    writer = csv.writer(stream, lineterminator="\n")
    for rec in wide_records:
        writer.writerow(rec.struct_dump_csv())
    return stream.getvalue()


def dump_wide_manual():
    stream = io.StringIO(newline="")
    writer = csv.writer(stream, lineterminator="\n")
    for rec in wide_records:
        writer.writerow([_render(getattr(rec, f)) for f in _WIDE_FIELDS])
    return stream.getvalue()


# Self-validating: every path must agree on the same data.
assert decode_structs() == records
assert decode_manual() == records
assert decode_rows() == cells
assert decode_dicts() == dict_rows
assert decode_str_structs() == str_records
assert decode_str_manual() == str_records
assert decode_str_rows() == [[r.a, r.b, r.c, r.d, r.e] for r in str_records]
assert decode_wide_structs() == wide_records
assert decode_wide_manual() == wide_records
assert dump_wide_structs() == wide_body
assert dump_wide_manual() == wide_body
assert dump_structs() == body
assert dump_rows() == body
assert dump_dicts() == body


print(f"Python {sys.version}")
print(f"structtype {structtype.__version__}")
print()
print(f"CSV vs stdlib csv, {N} records/op")
print("-" * 56)


def bench(label, fn, baseline=None, ref="", *, number=50):
    t = min(timeit.repeat(fn, number=number, repeat=3)) / number
    note = ""
    if baseline is not None:
        factor = baseline / t
        if 0.9 <= factor <= 1.1:
            note = f"   (~same as {ref})"
        elif factor > 1:
            note = f"   ({factor:.2f}x faster than {ref})"
        else:
            note = f"   ({1 / factor:.2f}x slower than {ref})"
    print(f"  {label:<26} {t * 1e6:9.1f} μs{note}")
    return t


print("Decode, mixed record (int/str/float/bool/Optional)")
bench("csv.reader (tokenize)", decode_rows)
bench("csv.DictReader (tokenize)", decode_dicts)
base = bench("reader + manual coercion", decode_manual)
bench("structtype decode", decode_structs, baseline=base, ref="manual coercion")
print()
print("Decode, str-only record (5 str fields)")
bench("csv.reader (tokenize)", decode_str_rows)
base = bench("reader + StrRecord(*row)", decode_str_manual)
bench("structtype decode", decode_str_structs, baseline=base, ref="StrRecord(*row)")
print()
print("Dump, mixed record")
base = bench("csv.writer (+ str/cell)", dump_rows)
bench("csv.DictWriter", dump_dicts, baseline=base, ref="manual render")
bench("structtype dump", dump_structs, baseline=base, ref="manual render")
print()
print("Decode, wide record (50 fields)")
base = bench("reader + manual coercion", decode_wide_manual)
bench("structtype decode", decode_wide_structs, baseline=base, ref="manual coercion")
print()
print("Dump, wide record (50 fields)")
base = bench("csv.writer (+ str/cell)", dump_wide_manual)
bench("structtype dump", dump_wide_structs, baseline=base, ref="manual render")
