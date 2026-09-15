# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "msgspec",
#     "structtype",
# ]
#
# [tool.uv.sources]
# structtype = { path = "..", editable = true }
# ///

"""Micro-benchmarks for byte-level string handling.

Covers JSON dump/validate for ASCII, non-ASCII and escape-heavy strings of
various lengths, plus the alias-field lookup for wide structs (declaration
order vs. reverse order). Reports ns/op and ns/UTF-8-byte.
"""

from __future__ import annotations

import json
import sys
import timeit

import structtype
from structtype import Struct, StructAdapter, StructMeta

try:
    import msgspec
except ImportError:
    msgspec = None


def measure(fn, number, repeat=5):
    return min(timeit.repeat(fn, number=number, repeat=repeat)) / number


def report(label, ns, nbytes=None):
    if nbytes:
        print(f"{label:44s} {ns * 1e9:10.1f} ns/op {ns / nbytes * 1e9:8.3f} ns/byte")
    else:
        print(f"{label:44s} {ns * 1e9:10.1f} ns/op")


def scaled_number(nbytes):
    return max(2000, 200_000 // max(1, nbytes))


def bench_str(adapter, msgspec_codec):
    print("\n== dump_json str ==")
    cases = [
        ("ascii 10", "x" * 10),
        ("ascii 100", "x" * 100),
        ("ascii 1000", "x" * 1000),
        ("ascii 10000", "x" * 10000),
        ("escape 100", ('a"b\\c\td\x01' * 20)),
        ("nonascii 100", "é" * 100),
        ("nonascii 1000", "é" * 1000),
        ("emoji 100", "𝄞" * 100),
    ]
    for name, s in cases:
        nbytes = len(s.encode("utf-8"))
        n = scaled_number(nbytes)
        ns = measure(lambda s=s: adapter.struct_dump_json(s), n)
        report(f"dump {name}", ns, nbytes)
        if msgspec is not None:
            ns = measure(lambda s=s: msgspec.json.encode(s), n)
            report(f"  msgspec {name}", ns, nbytes)

    print("\n== validate_json str ==")
    for name, s in cases:
        raw = s.encode("utf-8")
        j = json.dumps(s, ensure_ascii=False).encode("utf-8")
        n = scaled_number(len(raw))
        ns = measure(lambda j=j: adapter.struct_validate_json(j), n)
        report(f"validate {name}", ns, len(raw))
        if msgspec is not None:
            ns = measure(lambda j=j: msgspec.json.decode(j, type=str), n)
            report(f"  msgspec {name}", ns, len(raw))


def make_struct(nfields):
    annotations = {f"f{i}": int for i in range(nfields)}
    return StructMeta("S", (Struct,), {"__annotations__": annotations})


def bench_wide_struct():
    print("\n== wide struct validate_json ==")
    for nfields in (5, 20, 50, 100):
        cls = make_struct(nfields)
        adapter = StructAdapter(cls)
        keys = [f"f{i}" for i in range(nfields)]
        inorder = ("{" + ",".join(f'"{k}":1' for k in keys) + "}").encode()
        reverse = ("{" + ",".join(f'"{k}":1' for k in reversed(keys)) + "}").encode()
        n = max(2000, 200_000 // nfields)
        report(
            f"{nfields} fields inorder",
            measure(
                lambda adapter=adapter, inorder=inorder: adapter.struct_validate_json(
                    inorder
                ),
                n,
            ),
        )
        report(
            f"{nfields} fields reverse",
            measure(
                lambda adapter=adapter, reverse=reverse: adapter.struct_validate_json(
                    reverse
                ),
                n,
            ),
        )


def main():
    print(f"Python {sys.version.split()[0]}, structtype {structtype.__version__}")
    bench_str(StructAdapter(str), msgspec)
    bench_wide_struct()


if __name__ == "__main__":
    main()
