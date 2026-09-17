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

"""Benchmark JSON round-trip for a struct with a long ``str`` field, compared
against msgspec and pydantic. Strings are 100000 characters (ASCII, non-ASCII
and escape-heavy). Reports ns/UTF-8-byte and the structtype speedup vs
pydantic.
"""

from __future__ import annotations

import sys
import timeit

import structtype
from structtype import Struct

try:
    import msgspec
except ImportError:
    msgspec = None

try:
    import pydantic
except ImportError:
    pydantic = None

SIZE = 100000


def _text_with_escapes(size=SIZE):
    chars = ["x"] * size
    for i in range(79, size, 80):
        chars[i] = "\n"
    for i in range(199, size, 200):
        chars[i] = '"'
    return "".join(chars)


CASES = [
    ("ascii", "x" * SIZE),
    ("nonascii", "é" * SIZE),
    ("escape", _text_with_escapes()),
]


def measure(fn, number, repeat=5):
    return min(timeit.repeat(fn, number=number, repeat=repeat)) / number


def table(title, columns, rows, decimals=3):
    width = max(len(r[0]) for r in rows)
    print(f"\n{title}")
    print(f"  {'case':<{width}}" + "".join(f"{c:>13}" for c in columns))
    for row in rows:
        line = f"  {row[0]:<{width}}"
        for value in row[1:]:
            if value is None:
                line += f"{'n/a':>13}"
            elif isinstance(value, str):
                line += f"{value:>13}"
            else:
                line += f"{value:>13.{decimals}f}"
        print(line)


def speedup(base, other):
    return None if other is None else f"{other / base:.2f}x"


def main():
    class Msg(Struct):
        text: str

    codecs = {}
    if msgspec is not None:

        class MsgMS(msgspec.Struct):
            text: str

        codecs["msgspec"] = MsgMS
    if pydantic is not None:

        class MsgPD(pydantic.BaseModel):
            text: str

        codecs["pydantic"] = MsgPD

    parts = [f"Python {sys.version.split()[0]}", f"structtype {structtype.__version__}"]
    if msgspec is not None:
        parts.append(f"msgspec {msgspec.__version__}")
    if pydantic is not None:
        parts.append(f"pydantic {pydantic.__version__}")
    print(", ".join(parts))

    dump_rows, validate_rows = [], []
    for name, s in CASES:
        nbytes = len(s.encode("utf-8"))
        obj = Msg(s)
        buf = obj.struct_dump_json()

        st_dump = measure(lambda o=obj: o.struct_dump_json(), 200) / nbytes * 1e9
        st_validate = (
            measure(lambda b=buf: Msg.struct_validate_json(b), 200) / nbytes * 1e9
        )
        ms_dump = ms_validate = pd_dump = pd_validate = None
        if msgspec is not None:
            cls = codecs["msgspec"]
            mobj = cls(s)
            ms_dump = measure(lambda o=mobj: msgspec.json.encode(o), 200) / nbytes * 1e9
            ms_validate = (
                measure(lambda b=buf, c=cls: msgspec.json.decode(b, type=c), 200)
                / nbytes
                * 1e9
            )
        if pydantic is not None:
            pobj = codecs["pydantic"](text=s)
            pd_dump = (
                measure(lambda o=pobj: o.model_dump_json().encode(), 200) / nbytes * 1e9
            )
            pd_validate = (
                measure(
                    lambda b=buf, c=codecs["pydantic"]: c.model_validate_json(b), 200
                )
                / nbytes
                * 1e9
            )

        dump_rows.append((name, st_dump, ms_dump, pd_dump, speedup(st_dump, pd_dump)))
        validate_rows.append(
            (
                name,
                st_validate,
                ms_validate,
                pd_validate,
                speedup(st_validate, pd_validate),
            )
        )

    columns = ["structtype", "msgspec", "pydantic", "vs pydantic"]
    unit = f"{SIZE} chars"
    table(f"long str struct dump_json ({unit}, ns/byte)", columns, dump_rows)
    table(f"long str struct validate_json ({unit}, ns/byte)", columns, validate_rows)


if __name__ == "__main__":
    main()
