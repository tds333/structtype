# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "structtype",
# ]
#
# [tool.uv.sources]
# structtype = { path = "..", editable = true }
# ///

"""Benchmark CSV decode on a real, million-row file on disk.

Generates a deterministic CSV file and then measures two passes over it:

- Validate: ``csv.reader`` -> ``Row.struct_validate_csv(cells)`` for every row,
  which tokenizes, coerces with the native codecs and constructs each Struct.
- Baseline: ``csv.reader`` plus hand-written per-cell coercion to the same
  Python values the Struct holds (``int``/``float``/``bool``, ``date``,
  ``UUID`` and ``Decimal``). It skips construction, so it is the equivalent-work
  floor, not a full replacement.

Decode timing interleaves the two impls and reports the minimum of
``--repeat`` runs, so heap warm-up and allocation order do not bias either.
Both timed decode passes keep the parsed rows in a list, so construction and
retention are measured like a real "load the file into memory" workload; the
list is released between passes. Writing streams row by row, regenerating each
row on demand (seeded by index) so it never holds the whole dataset.

The schema is 10 mixed-scalar fields using only native codecs (no
``Serializer``, no constraints), so structtype and the hand-written baseline
do the same coercion work.
"""

import argparse
import csv
import os
import resource
import sys
import tempfile
import time
import uuid
from datetime import date, timedelta
from decimal import Decimal

import structtype
from structtype import Struct


class Row(Struct):
    id: int
    name: str
    age: int
    score: float
    active: bool
    ratio: float
    created: date
    uid: uuid.UUID
    balance: Decimal
    note: str | None = None


_EPOCH = date(2000, 1, 1)


def make_row(i, seed):
    """Deterministic row ``i``; ``seed`` shifts every value."""
    k = i + seed
    return Row(
        id=k + 1,
        name=f"user-{k:08d}",
        age=k % 150,
        score=round((k % 1000) / 10, 1),
        active=(k % 2 == 0),
        ratio=(k % 1000) / 1000,
        created=_EPOCH + timedelta(days=k % 10000),
        uid=uuid.UUID(int=k + 1),
        balance=Decimal(k % 100000) / 100,
        note=None if k % 5 == 0 else f"note-{k}",
    )


def write_csv(path, n, seed):
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f, lineterminator="\n")
        for i in range(n):
            writer.writerow(make_row(i, seed).struct_dump_csv())


def validate_csv(path, n):
    """Timed pass: decode every row, keeping the results like a real loader."""
    rows = []
    append = rows.append
    with open(path, newline="", encoding="utf-8") as f:
        for cells in csv.reader(f):
            append(Row.struct_validate_csv(cells))
    if len(rows) != n:
        raise AssertionError(f"validated {len(rows)} rows, expected {n}")
    return rows


def manual_csv(path):
    """Equivalent-work baseline: tokenize plus hand-write the same coercions."""
    rows = []
    append = rows.append
    with open(path, newline="", encoding="utf-8") as f:
        for c in csv.reader(f):
            append(
                (
                    int(c[0]),
                    c[1],
                    int(c[2]),
                    float(c[3]),
                    c[4] == "true",
                    float(c[5]),
                    date.fromisoformat(c[6]),
                    uuid.UUID(c[7]),
                    Decimal(c[8]),
                    c[9] or None,
                )
            )
    return rows


def _rss_mb():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024


def main():
    parser = argparse.ArgumentParser(
        description="Benchmark struct_validate_csv on a large on-disk CSV"
    )
    parser.add_argument(
        "--rows", type=int, default=1_000_000, help="number of records to write"
    )
    parser.add_argument(
        "--path",
        default=None,
        help="CSV file path (default: a temp file, removed on exit)",
    )
    parser.add_argument("--seed", type=int, default=0, help="value seed")
    parser.add_argument(
        "--repeat",
        type=int,
        default=3,
        help="timed passes per impl, interleaved; reports the minimum",
    )
    parser.add_argument(
        "--keep", action="store_true", help="keep the generated CSV file"
    )
    args = parser.parse_args()

    path = args.path or os.path.join(
        tempfile.gettempdir(), "structtype_bench_csv_1m.csv"
    )

    print(f"Python {sys.version.split()[0]}")
    print(f"structtype {structtype.__version__}")
    print(f"rows {args.rows:,}  repeat {args.repeat}  path {path}")
    print("-" * 60)

    t0 = time.perf_counter()
    write_csv(path, args.rows, args.seed)
    t_write = time.perf_counter() - t0

    size = os.path.getsize(path)

    t_validate = float("inf")
    t_base = float("inf")
    for _ in range(args.repeat):
        t0 = time.perf_counter()
        rows = validate_csv(path, args.rows)
        t_validate = min(t_validate, time.perf_counter() - t0)
        del rows

        t0 = time.perf_counter()
        rows = manual_csv(path)
        t_base = min(t_base, time.perf_counter() - t0)
        del rows

    peak = _rss_mb()

    print(f"file size        {size / 1e6:9.1f} MB")
    print(f"write            {t_write:9.3f} s  {args.rows / t_write:12,.0f} rows/s")
    print(
        f"validate         {t_validate:9.3f} s  {args.rows / t_validate:12,.0f} rows/s"
    )
    print(
        f"reader + manual  {t_base:9.3f} s  {args.rows / t_base:12,.0f} rows/s"
        "   (same coercion)"
    )
    speedup = t_base / t_validate
    if speedup >= 1:
        verdict = f"{speedup:.2f}x faster than manual"
    else:
        verdict = f"{1 / speedup:.2f}x slower than manual"
    print(f"validate vs base {verdict}")
    print(f"peak RSS         {peak:9.1f} MB")

    if not args.keep:
        os.remove(path)
        print(f"removed {path} (use --keep to retain)")


if __name__ == "__main__":
    main()
