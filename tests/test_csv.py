import io
from datetime import date, datetime, timedelta
from decimal import Decimal
from typing import Annotated
from uuid import UUID

import pytest
from structtype._core import _csv_decode, _csv_fields

import structtype as st


class Row(st.Struct):
    id: int
    name: str
    score: float | None = None


def test_c_primitive_decodes_and_fills_defaults():
    assert _csv_decode(Row, ["1", "alice", "1.5"], frozenset({""})) == Row(
        1, "alice", 1.5
    )
    assert _csv_decode(Row, ["2", "bob", ""], frozenset({""})) == Row(2, "bob", None)


def test_c_primitive_ignores_extra_cells():
    assert _csv_decode(Row, ["1", "alice", "2.0", "junk"], frozenset()) == Row(
        1, "alice", 2.0
    )


def test_c_primitive_missing_required_errors():
    with pytest.raises(st.ValidationError):
        _csv_decode(Row, ["1"], frozenset())


def test_c_primitive_fields_normalized_in_declaration_order():
    from enum import Enum

    class Level(Enum):
        HIGH = "high"

    class T(st.Struct):
        n: int
        lvl: Level
        b: bytes
        s: str | None
        items: list[int]

    fields = _csv_fields(T(1, Level.HIGH, b"\x00\x01", None, [1, 2]))
    assert list(fields) == [1, "high", "AAE=", None, [1, 2]]


def _reader(text, **fmt):
    import csv

    return csv.reader(io.StringIO(text, newline=""), **fmt)


def _writer(stream):
    import csv

    return csv.writer(stream, lineterminator="\n")


def _one(cls, buf, **fmt):
    return next(cls.struct_validate_csv(_reader(buf, **fmt)))


class User(st.Struct):
    user_id: int
    name: str
    joined: date | None = None


def test_validate_csv_positional_types():
    assert _one(User, "1,alice,2020-01-02\n") == User(1, "alice", date(2020, 1, 2))


def test_validate_csv_empty_cell_null():
    assert _one(User, "2,bob,\n") == User(2, "bob", None)


def test_validate_csv_null_values_option():
    assert next(
        User.struct_validate_csv(_reader("3,carol,NA\n"), null_values=("NA",))
    ) == User(3, "carol", None)


def test_validate_csv_null_values_empty_disables_mapping():
    class T(st.Struct):
        name: str

    assert next(T.struct_validate_csv(_reader('""\n'), null_values=())) == T("")
    with pytest.raises(st.ValidationError):
        next(T.struct_validate_csv(_reader('""\n')))


def test_validate_csv_rejects_bare_string_null_values():
    with pytest.raises(TypeError, match="sequence of strings"):
        User.struct_validate_csv(_reader("3,carol,NA\n"), null_values="NA")


def test_validate_csv_explicit_empty_null_values_matches_default():
    assert next(
        User.struct_validate_csv(_reader("2,bob,\n"), null_values=("",))
    ) == User(2, "bob", None)


def test_validate_csv_iterates_rows():
    assert list(User.struct_validate_csv(_reader("1,alice,\n2,bob,\n"))) == [
        User(1, "alice"),
        User(2, "bob"),
    ]


def test_validate_csv_empty_reader_is_empty():
    assert list(User.struct_validate_csv(_reader(""))) == []


def test_validate_csv_propagates_reader_error():
    import csv

    class Boom:
        def __iter__(self):
            return self

        def __next__(self):
            raise csv.Error("boom")

    with pytest.raises(csv.Error, match="boom"):
        list(User.struct_validate_csv(Boom()))


def test_validate_csv_decode_stopiteration_is_not_exhaustion():
    class Boom(st.Struct):
        x: int

        def __post_init__(self):
            raise StopIteration

    with pytest.raises(RuntimeError, match="StopIteration"):
        list(Boom.struct_validate_csv(_reader("1\n")))


def test_validate_csv_custom_delimiter():
    assert _one(User, "1;alice;2020-01-02\n", delimiter=";") == User(
        1, "alice", date(2020, 1, 2)
    )


def test_validate_csv_reader_quotechar():
    class Q(st.Struct):
        a: str
        b: str

    assert _one(Q, "'a,b',x\n", quotechar="'") == Q("a,b", "x")


def test_validate_csv_caller_decodes_encoding():
    assert _one(User, "4,caf\xe9,\n") == User(4, "café", None)


def test_validate_csv_rejects_nested_field():
    class W(st.Struct):
        items: list[int]

    with pytest.raises(st.ValidationError):
        list(W.struct_validate_csv(_reader("1\n")))


class Rec(st.Struct):
    a: int
    b: str
    c: bool
    d: date | None = None


def test_dump_csv_one_row_and_roundtrip():
    rec = Rec(1, "alice", True, date(2020, 1, 2))
    stream = io.StringIO(newline="")
    rec.struct_dump_csv(_writer(stream))
    assert stream.getvalue() == "1,alice,true,2020-01-02\n"
    assert _one(Rec, stream.getvalue()) == rec


def test_dump_csv_quoting_and_none():
    stream = io.StringIO(newline="")
    Rec(2, 'x,y"z', False, None).struct_dump_csv(_writer(stream))
    assert stream.getvalue() == '2,"x,y""z",false,\n'


def test_dump_csv_enum_and_bytes_roundtrip():
    from enum import Enum

    class Color(str, Enum):
        RED = "red"

    class T(st.Struct):
        c: Color
        b: bytes

    t = T(Color.RED, b"\x00\x01")
    stream = io.StringIO(newline="")
    t.struct_dump_csv(_writer(stream))
    assert stream.getvalue() == "red,AAE=\n"
    assert _one(T, stream.getvalue()) == t


def test_dump_csv_int_and_plain_enum_roundtrip():
    from enum import Enum, IntEnum

    class Level(IntEnum):
        LOW = 1
        HIGH = 2

    class Flavor(Enum):
        SWEET = "sweet"

    class E(st.Struct):
        level: Level
        flavor: Flavor

    e = E(Level.HIGH, Flavor.SWEET)
    stream = io.StringIO(newline="")
    e.struct_dump_csv(_writer(stream))
    assert stream.getvalue() == "2,sweet\n"
    assert _one(E, stream.getvalue()) == e


def test_dump_csv_rejects_nested():
    class W(st.Struct):
        items: list[int]

    with pytest.raises(TypeError):
        W([1, 2]).struct_dump_csv(_writer(io.StringIO(newline="")))


def test_dump_csv_omit_defaults_emits_all_columns():
    class D(st.Struct):
        struct_config = st.StructConfig(omit_defaults=True)

        a: int
        b: int = 7

    d = D(1)
    stream = io.StringIO(newline="")
    d.struct_dump_csv(_writer(stream))
    assert stream.getvalue() == "1,7\n"
    assert _one(D, stream.getvalue()) == d


def test_dump_csv_array_like_emits_single_row():
    class A(st.Struct):
        struct_config = st.StructConfig(array_like=True)

        a: int
        b: str

    x = A(1, "x")
    stream = io.StringIO(newline="")
    x.struct_dump_csv(_writer(stream))
    assert stream.getvalue() == "1,x\n"
    assert _one(A, stream.getvalue()) == x


def test_validate_csv_ignores_extra_cells():
    assert _one(Row, "1,alice,2.0,junk\n") == Row(1, "alice", 2.0)


def test_validate_csv_required_empty_cell_errors():
    with pytest.raises(st.ValidationError):
        next(Row.struct_validate_csv(_reader("1,,\n")))


def test_validate_csv_error_path_includes_field_name():
    with pytest.raises(st.ValidationError, match=r"\$\.name"):
        next(Row.struct_validate_csv(_reader("1,,\n")))


def test_validate_csv_uuid_decimal_datetime_timedelta_roundtrip():
    class T(st.Struct):
        uid: UUID
        dec: Decimal
        dt: datetime
        td: timedelta

    t = T(
        UUID("12345678-1234-5678-1234-567812345678"),
        Decimal("1.50"),
        datetime(2020, 1, 2, 3, 4, 5),
        timedelta(seconds=90),
    )
    stream = io.StringIO(newline="")
    t.struct_dump_csv(_writer(stream))
    assert _one(T, stream.getvalue()) == t


def test_validate_csv_rejects_struct_typed_field():
    class Inner(st.Struct):
        x: int

    class Outer(st.Struct):
        inner: Inner
        y: int

    with pytest.raises(st.ValidationError):
        list(Outer.struct_validate_csv(_reader("1,2\n")))
    with pytest.raises(TypeError):
        Outer(Inner(1), 2).struct_dump_csv(_writer(io.StringIO(newline="")))


def test_csv_ignores_aliases_uses_declaration_order():
    class Aliased(st.Struct):
        real: Annotated[int, st.Field(alias="other")]
        name: str

    assert _one(Aliased, "1,x\n") == Aliased(1, "x")
    stream = io.StringIO(newline="")
    Aliased(1, "x").struct_dump_csv(_writer(stream))
    assert stream.getvalue() == "1,x\n"
    assert _one(Aliased, stream.getvalue()) == Aliased(1, "x")


def test_csv_honors_serializer_load_and_dump():
    class S(st.Struct):
        d: Annotated[
            date,
            st.Serializer(
                load=lambda s: datetime.strptime(s, "%Y/%m/%d").date(),
                dump=lambda d: d.strftime("%Y/%m/%d"),
            ),
        ]
        n: int

    s = _one(S, "2020/01/02,7\n")
    assert s == S(date(2020, 1, 2), 7)

    stream = io.StringIO(newline="")
    s.struct_dump_csv(_writer(stream))
    assert stream.getvalue() == "2020/01/02,7\n"
    assert _one(S, stream.getvalue()) == s


def test_validate_csv_enforces_constraints():
    class C(st.Struct):
        n: Annotated[int, st.NumericConstraint(ge=0)]
        code: Annotated[str, st.StrConstraint(min_length=3)]

    assert _one(C, "5,abc\n") == C(5, "abc")

    with pytest.raises(st.ValidationError, match=r"\$\.n"):
        _one(C, "-1,abc\n")
    with pytest.raises(st.ValidationError, match=r"\$\.code"):
        _one(C, "5,ab\n")
