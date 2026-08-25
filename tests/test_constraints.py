import datetime
import math
import re
from typing import Annotated

import pytest

import structtype
from structtype import (
    BytesConstraint,
    CollectionConstraint,
    Constraint,
    Field,
    NumericConstraint,
    Serializer,
    StrConstraint,
    TimezoneConstraint,
)
from structtype._core import JSONDecoder, JSONEncoder, _json_encode, _json_decode

class _JsonProto:
    Decoder = JSONDecoder
    Encoder = JSONEncoder
    encode = staticmethod(_json_encode)
    decode = staticmethod(_json_decode)

@pytest.fixture(params=["json"])
def proto(request):
    return _JsonProto


FIELDS = {
    "title": "example title",
    "description": "example description",
    "examples": ["example 1", "example 2"],
    "deprecated": True,
    "json_schema_extra": {"foo": "bar"},
}

NUMERIC_FIELDS = {
    "gt": 0,
    "ge": 0,
    "lt": 10,
    "le": 10,
    "multiple_of": 1,
}

STR_FIELDS = {
    "pattern": "^foo$",
    "min_length": 0,
    "max_length": 10,
}


def assert_eq(a, b):
    assert a == b
    assert not a != b


def assert_ne(a, b):
    assert a != b
    assert not a == b


class TestMetaObject:
    def test_init_nokwargs(self):
        c = Field()
        for f in FIELDS:
            assert getattr(c, f) is None

    @pytest.mark.parametrize("field", FIELDS)
    def test_init_explicit_none(self, field):
        c = Field(**{field: None})
        for f in FIELDS:
            assert getattr(c, f) is None

    @pytest.mark.parametrize("field", FIELDS)
    def test_init(self, field):
        c = Field(**{field: FIELDS[field]})
        for f in FIELDS:
            sol = FIELDS[field] if f == field else None
            assert getattr(c, f) == sol

    def test_repr_empty(self):
        assert repr(Field()) == "structtype.Field()"
        for field in FIELDS:
            c = Field(**{field: None})
            assert repr(c) == "structtype.Field()"

    def test_repr_error(self):
        class Oops:
            def __repr__(self):
                raise ValueError("Oh no!")

        m = Field(json_schema_extra={"oops": Oops()})
        with pytest.raises(ValueError, match="Oh no!"):
            repr(m)

    @pytest.mark.parametrize("field", FIELDS)
    def test_repr_one_field(self, field):
        c = Field(**{field: FIELDS[field]})
        assert repr(c) == f"structtype.Field({field}={FIELDS[field]!r})"

    def test_rich_repr_empty(self):
        assert Field().__rich_repr__() == []

    @pytest.mark.parametrize("field", FIELDS)
    def test_rich_repr_one_field(self, field):
        m = Field(**{field: FIELDS[field]})
        assert m.__rich_repr__() == [(field, FIELDS[field])]

    def test_equality(self):
        assert_eq(Field(), Field())
        assert_ne(Field(), None)

        with pytest.raises(TypeError):
            Field() > Field()
        with pytest.raises(TypeError):
            Field() > None

    @pytest.mark.parametrize("field", ["title", "description"])
    def test_string_fields(self, field):
        Field(**{field: "good"})
        with pytest.raises(TypeError, match=f"`{field}` must be a str, got bytes"):
            Field(**{field: b"bad"})

    @pytest.mark.parametrize("field", ["deprecated"])
    def test_bool_fields(self, field):
        Field(**{field: True})
        Field(**{field: False})
        with pytest.raises(TypeError, match=f"`{field}` must be a bool, got float"):
            Field(**{field: 1.5})

    @pytest.mark.parametrize("field", ["examples"])
    def test_list_fields(self, field):
        Field(**{field: ["good", "stuff"]})
        with pytest.raises(TypeError, match=f"`{field}` must be a list, got str"):
            Field(**{field: "bad"})

    @pytest.mark.parametrize("field", ["json_schema_extra"])
    def test_dict_fields(self, field):
        Field(**{field: {"good": "stuff"}})
        with pytest.raises(TypeError, match=f"`{field}` must be a dict, got str"):
            Field(**{field: "bad"})

    def test_rejects_constraint_kwargs(self):
        with pytest.raises(TypeError, match=r"(unexpected|invalid) keyword argument"):
            Field(ge=0)

    def test_rejects_dump_validate_kwargs(self):
        with pytest.raises(TypeError, match=r"(unexpected|invalid) keyword argument"):
            Field(dump=repr)
        with pytest.raises(TypeError, match=r"(unexpected|invalid) keyword argument"):
            Field(validate=repr)


class TestNumericConstraintMetaObject:
    def test_repr_empty(self):
        assert repr(NumericConstraint()) == "structtype.NumericConstraint()"

    @pytest.mark.parametrize("field", NUMERIC_FIELDS)
    def test_repr_one_field(self, field):
        c = NumericConstraint(**{field: NUMERIC_FIELDS[field]})
        assert repr(c) == f"structtype.NumericConstraint({field}={NUMERIC_FIELDS[field]!r})"

    def test_repr_multiple_fields(self):
        c = NumericConstraint(gt=0, lt=1)
        assert repr(c) == "structtype.NumericConstraint(gt=0, lt=1)"

    def test_rich_repr_empty(self):
        assert NumericConstraint().__rich_repr__() == []

    @pytest.mark.parametrize("field", NUMERIC_FIELDS)
    def test_rich_repr_one_field(self, field):
        m = NumericConstraint(**{field: NUMERIC_FIELDS[field]})
        assert m.__rich_repr__() == [(field, NUMERIC_FIELDS[field])]

    def test_rich_repr_multiple_fields(self):
        m = NumericConstraint(gt=0, lt=1)
        assert m.__rich_repr__() == [("gt", 0), ("lt", 1)]

    def test_equality(self):
        assert_eq(NumericConstraint(), NumericConstraint())
        assert_ne(NumericConstraint(), None)

        with pytest.raises(TypeError):
            NumericConstraint() > NumericConstraint()
        with pytest.raises(TypeError):
            NumericConstraint() > None

    def test_hash(self):
        def samples():
            return [
                NumericConstraint(),
                NumericConstraint(ge=0),
                NumericConstraint(ge=1, le=2),
            ]

        lk = {k: k for k in samples()}

        for key in samples():
            assert lk[key] == key

    @pytest.mark.parametrize("field", ["gt", "ge", "lt", "le", "multiple_of"])
    def test_numeric_fields(self, field):
        NumericConstraint(**{field: 1})
        NumericConstraint(**{field: 2.5})
        with pytest.raises(
            TypeError, match=f"`{field}` must be an int or float, got str"
        ):
            NumericConstraint(**{field: "bad"})

        with pytest.raises(ValueError, match=f"`{field}` must be finite"):
            NumericConstraint(**{field: float("inf")})

    @pytest.mark.parametrize("val", [0, 0.0])
    def test_multiple_of_bounds(self, val):
        with pytest.raises(ValueError, match=r"`multiple_of` must be > 0"):
            NumericConstraint(multiple_of=val)

    def test_conflicting_bounds_errors(self):
        with pytest.raises(ValueError, match="both `gt` and `ge`"):
            NumericConstraint(gt=0, ge=1)

        with pytest.raises(ValueError, match="both `lt` and `le`"):
            NumericConstraint(lt=0, le=1)


class TestStrConstraintMetaObject:
    @pytest.mark.parametrize("field", ["pattern"])
    def test_string_fields(self, field):
        StrConstraint(**{field: "good"})
        with pytest.raises(TypeError, match=f"`{field}` must be a str, got bytes"):
            StrConstraint(**{field: b"bad"})

    @pytest.mark.parametrize("field", ["min_length", "max_length"])
    def test_nonnegative_integer_fields(self, field):
        StrConstraint(**{field: 0})
        StrConstraint(**{field: 10})
        with pytest.raises(TypeError, match=f"`{field}` must be an int, got float"):
            StrConstraint(**{field: 1.5})
        with pytest.raises(ValueError, match=f"{field}` must be >= 0, got -10"):
            StrConstraint(**{field: -10})

    def test_invalid_pattern_errors(self):
        with pytest.raises(re.error):
            StrConstraint(pattern="[abc")

    def test_rich_repr_empty(self):
        assert StrConstraint().__rich_repr__() == []

    def test_rich_repr_fields(self):
        m = StrConstraint(min_length=2, max_length=5)
        assert m.__rich_repr__() == [("min_length", 2), ("max_length", 5)]


class TestBytesConstraintMetaObject:
    def test_repr_empty(self):
        assert repr(BytesConstraint()) == "structtype.BytesConstraint()"

    def test_repr_multiple_fields(self):
        c = BytesConstraint(min_length=1, max_length=5)
        assert repr(c) == "structtype.BytesConstraint(min_length=1, max_length=5)"

    def test_rich_repr_empty(self):
        assert BytesConstraint().__rich_repr__() == []

    def test_rich_repr_multiple_fields(self):
        m = BytesConstraint(min_length=1, max_length=5)
        assert m.__rich_repr__() == [("min_length", 1), ("max_length", 5)]

    def test_equality(self):
        assert_eq(BytesConstraint(max_length=5), BytesConstraint(max_length=5))
        assert_ne(BytesConstraint(max_length=5), BytesConstraint(max_length=6))
        assert_ne(BytesConstraint(), None)


class TestCollectionConstraintMetaObject:
    def test_repr_empty(self):
        assert repr(CollectionConstraint()) == "structtype.CollectionConstraint()"

    def test_repr_multiple_fields(self):
        c = CollectionConstraint(min_length=1, max_length=5)
        assert (
            repr(c) == "structtype.CollectionConstraint(min_length=1, max_length=5)"
        )

    def test_rich_repr_empty(self):
        assert CollectionConstraint().__rich_repr__() == []

    def test_rich_repr_multiple_fields(self):
        m = CollectionConstraint(min_length=1, max_length=5)
        assert m.__rich_repr__() == [("min_length", 1), ("max_length", 5)]

    def test_equality(self):
        assert_eq(
            CollectionConstraint(min_length=1), CollectionConstraint(min_length=1)
        )
        assert_ne(CollectionConstraint(min_length=1), "not a constraint")


class TestTimezoneConstraintMetaObject:
    def test_bool_field(self):
        TimezoneConstraint(tz=True)
        TimezoneConstraint(tz=False)
        with pytest.raises(TypeError, match="`tz` must be a bool, got float"):
            TimezoneConstraint(tz=1.5)

    def test_rich_repr_field(self):
        m = TimezoneConstraint(tz=False)
        assert m.__rich_repr__() == [("tz", False)]


class TestConstraintBaseMetaObject:
    def test_repr_empty(self):
        assert repr(Constraint()) == "structtype.Constraint()"

    def test_rich_repr_empty(self):
        assert Constraint().__rich_repr__() == []

    def test_default_fn_is_none(self):
        assert Constraint().fn is None

    def test_equality(self):
        assert_eq(Constraint(), Constraint())
        assert_ne(Constraint(), None)


class TestSerializerMetaObject:
    def test_init_dump_validate(self):
        s = Serializer(dump=lambda c: (c.real, c.imag), load=lambda o: complex(*o))
        assert callable(s.dump)
        assert callable(s.load)

    def test_only_dump(self):
        s = Serializer(dump=repr)
        assert callable(s.dump)
        assert s.load is None

    def test_only_load(self):
        s = Serializer(load=complex)
        assert s.dump is None
        assert callable(s.load)

    def test_non_callable_dump(self):
        with pytest.raises(TypeError):
            Serializer(dump=42)

    def test_non_callable_load(self):
        with pytest.raises(TypeError):
            Serializer(load="nope")

    def test_repr_roundtrip(self):
        def dump(c):
            return (c.real, c.imag)

        def load(o):
            return complex(*o)

        s = Serializer(dump=dump, load=load)
        r = repr(s)
        assert "dump=" in r and "load=" in r

    def test_repr_only_dump(self):
        assert repr(Serializer(dump=str)) == "structtype.Serializer(dump=<class 'str'>)"

    def test_repr_only_load(self):
        assert (
            repr(Serializer(load=int)) == "structtype.Serializer(load=<class 'int'>)"
        )

    def test_rich_repr_only_dump(self):
        s = Serializer(dump=str)
        assert s.__rich_repr__() == [("dump", str)]

    def test_equality(self):
        a = Serializer(dump=repr)
        b = Serializer(dump=repr)
        assert a == b
        assert hash(a) == hash(b)

    def test_inequality(self):
        assert_ne(Serializer(dump=str), Serializer(load=int))
        assert_ne(Serializer(dump=str), None)


class TestJSONDecoderMetaObject:
    def test_repr(self):
        assert repr(JSONDecoder()) == "structtype.json.Decoder(typing.Any)"


class TestInvalidConstraintAnnotations:
    """Constraint validity is applied in two places:

    - Type checks on constraint values in the Constraint constructors
    - Type checks on type & constraint annotations in Decoder constructors

    The tests here check the latter.
    """

    @pytest.mark.parametrize("name", ["ge", "gt", "le", "lt", "multiple_of"])
    def test_invalid_numeric_constraints(self, name):
        with pytest.raises(TypeError, match=f"Can only set `{name}` on a numeric type"):
            JSONDecoder(Annotated[str, NumericConstraint(**{name: 1})])

    def test_invalid_pattern_constraint(self):
        with pytest.raises(TypeError, match="Can only set `pattern` on a str type"):
            JSONDecoder(Annotated[int, StrConstraint(pattern="ok")])

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    def test_invalid_length_constraint(self, name):
        with pytest.raises(
            TypeError,
            match=f"Can only set `{name}` on a str type",
        ):
            JSONDecoder(Annotated[int, StrConstraint(**{name: 1})])

    def test_invalid_tz_constraint(self):
        with pytest.raises(
            TypeError,
            match="Can only set `tz` on a datetime or time type",
        ):
            JSONDecoder(Annotated[int, TimezoneConstraint(tz=True)])

    @pytest.mark.parametrize(
        "name, val",
        [("ge", 2**63), ("gt", 2**63 - 1), ("le", 2**63), ("lt", -(2**63))],
    )
    def test_invalid_integer_bounds(self, name, val):
        with pytest.raises(ValueError) as rec:
            JSONDecoder(Annotated[int, NumericConstraint(**{name: val})])
        assert name in str(rec.value)
        assert "not supported" in str(rec.value)

    def test_invalid_multiple_validator_annotations_conflict(self):
        with pytest.raises(TypeError, match="Multiple `Constraint` annotations"):
            JSONDecoder(Annotated[int, NumericConstraint(ge=1), NumericConstraint(ge=2)])

    def test_invalid_gt_and_ge_conflict(self):
        with pytest.raises(ValueError, match="Cannot specify both `gt` and `ge`"):
            NumericConstraint(gt=1, ge=2)

    def test_invalid_lt_and_le_conflict(self):
        with pytest.raises(ValueError, match="Cannot specify both `lt` and `le`"):
            NumericConstraint(lt=2, le=1)


class TestIntConstraints:
    @pytest.mark.parametrize(
        "name, bound, good, bad",
        [
            ("ge", -1, [-1, 2**63, 2**65], [-(2**64), -2]),
            ("gt", -1, [0, 2**63, 2**65], [-(2**64), -1]),
            ("le", -1, [-(2**64), -1], [0, 2**63, 2**65]),
            ("lt", -1, [-(2**64), -2], [-1, 2**63, 2**65]),
        ],
    )
    def test_bounds(self, proto, name, bound, good, bad):
        class Ex(structtype.Struct):
            x: Annotated[int, NumericConstraint(**{name: bound})]

        dec = proto.Decoder(Ex)

        for x in good:
            assert dec.decode(proto.encode(Ex(x))).x == x

        op = ">=" if name.startswith("g") else "<="
        offset = {"lt": -1, "gt": 1}.get(name, 0)
        err_msg = rf"Expected `int` {op} {bound + offset} - at `\$.x`"
        for x in bad:
            with pytest.raises(structtype.ValidationError, match=err_msg):
                dec.decode(proto.encode(Ex(x)))

    def test_multiple_of(self, proto):
        good = [-(2**64), -2, 0, 2, 40, 2**63 + 2, 2**65]
        bad = [1, -1, 2**63 + 1, 2**65 + 1]

        class Ex(structtype.Struct):
            x: Annotated[int, NumericConstraint(multiple_of=2)]

        dec = proto.Decoder(Ex)

        for x in good:
            assert dec.decode(proto.encode(Ex(x))).x == x

        err_msg = r"Expected `int` that's a multiple of 2 - at `\$.x`"
        for x in bad:
            with pytest.raises(structtype.ValidationError, match=err_msg):
                dec.decode(proto.encode(Ex(x)))

    @pytest.mark.parametrize(
        "meta, good, bad",
        [
            (NumericConstraint(ge=0, le=10, multiple_of=2), [0, 2, 10], [-1, 1, 11]),
            (NumericConstraint(ge=0, multiple_of=2), [0, 2**63 + 2], [-2, 2**63 + 1]),
            (NumericConstraint(le=0, multiple_of=2), [0, -(2**63)], [-1, 2, 2**63]),
            (NumericConstraint(ge=0, le=10), [0, 10], [-1, 11]),
            (NumericConstraint(gt=0, lt=10), [1, 2, 9], [-1, 0, 10]),
        ],
    )
    def test_combinations(self, proto, meta, good, bad):
        class Ex(structtype.Struct):
            x: Annotated[int, meta]

        dec = proto.Decoder(Ex)

        for x in good:
            assert dec.decode(proto.encode(Ex(x))).x == x

        for x in bad:
            with pytest.raises(structtype.ValidationError):
                dec.decode(proto.encode(Ex(x)))


class TestFloatConstraints:
    @pytest.mark.parametrize("name", ["ge", "gt", "le", "lt"])
    def test_bound_constraint_uint64_valid_for_floats(self, name):
        typ = Annotated[float, NumericConstraint(**{name: 2**63})]
        JSONDecoder(typ)

    def get_bounds_cases(self, name, bound):
        def ceilp1(x):
            return int(math.ceil(x + 1))

        def floorm1(x):
            return int(math.floor(x - 1))

        if name.startswith("g"):
            good_dir = math.inf
            good_round = ceilp1
            bad_round = floorm1
        else:
            good_dir = -math.inf
            good_round = floorm1
            bad_round = ceilp1

        if name.endswith("e"):
            good = bound
            bad = math.nextafter(bound, -good_dir)
        else:
            good = math.nextafter(bound, good_dir)
            bad = bound
        good_cases = [good, good_round(good), float(good_round(good))]
        bad_cases = [bad, bad_round(bad), float(bad_round(bad))]

        op = ">" if name.startswith("g") else "<"
        if name.endswith("e"):
            op += "="

        return good_cases, bad_cases, op

    @pytest.mark.parametrize("name", ["ge", "gt", "le", "lt"])
    @pytest.mark.parametrize("bound", [1.5, -1.5, 10.0])
    def test_bounds(self, proto, name, bound):
        class Ex(structtype.Struct):
            x: Annotated[float, NumericConstraint(**{name: bound})]

        dec = proto.Decoder(Ex)

        good, bad, op = self.get_bounds_cases(name, bound)

        for x in good:
            assert dec.decode(proto.encode(Ex(x))).x == x

        err_msg = rf"Expected `float` {op} {bound} - at `\$.x`"
        for x in bad:
            with pytest.raises(structtype.ValidationError, match=err_msg):
                dec.decode(proto.encode(Ex(x)))

    def test_multiple_of(self, proto):
        """multipleOf for floats will always have precisions issues. This check
        just ensures that _some_ cases work. See
        https://github.com/json-schema-org/json-schema-spec/issues/312 for more
        info."""

        class Ex(structtype.Struct):
            x: Annotated[float, NumericConstraint(multiple_of=0.1)]

        dec = proto.Decoder(Ex)

        for x in [0, 0.0, 0.1, -0.1, 0.2, -0.2]:
            assert dec.decode(proto.encode(Ex(x))).x == x

        err_msg = r"Expected `float` that's a multiple of 0.1 - at `\$.x`"
        for x in [0.01, -0.15]:
            with pytest.raises(structtype.ValidationError, match=err_msg):
                dec.decode(proto.encode(Ex(x)))

    @pytest.mark.parametrize(
        "meta, good, bad",
        [
            (NumericConstraint(ge=0.0, le=10.0, multiple_of=2.0), [0, 2.0, 10], [-2, 11, 3]),
            (NumericConstraint(ge=0.0, multiple_of=2.0), [0, 2, 10.0], [-2, 3]),
            (NumericConstraint(le=10.0, multiple_of=2.0), [-2.0, 10.0], [11.0, 3.0]),
            (NumericConstraint(ge=0.0, le=10.0), [0.0, 2.0, 10.0], [-1.0, 11.5, 11]),
        ],
    )
    def test_combinations(self, proto, meta, good, bad):
        class Ex(structtype.Struct):
            x: Annotated[float, meta]

        dec = proto.Decoder(Ex)

        for x in good:
            assert dec.decode(proto.encode(Ex(x))).x == x

        for x in bad:
            with pytest.raises(structtype.ValidationError):
                assert dec.decode(proto.encode(Ex(x)))


class TestStrConstraints:
    def test_min_length(self, proto):
        class Ex(structtype.Struct):
            x: Annotated[str, StrConstraint(min_length=2)]

        dec = proto.Decoder(Ex)

        for x in ["xx", "xxx", "𝄞x"]:
            assert dec.decode(proto.encode(Ex(x))).x == x

        err_msg = r"Expected `str` of length >= 2 - at `\$.x`"
        for x in ["x", "𝄞", ""]:
            with pytest.raises(structtype.ValidationError, match=err_msg):
                dec.decode(proto.encode(Ex(x)))

    def test_max_length(self, proto):
        class Ex(structtype.Struct):
            x: Annotated[str, StrConstraint(max_length=2)]

        dec = proto.Decoder(Ex)

        for x in ["", "xx", "𝄞x"]:
            assert dec.decode(proto.encode(Ex(x))).x == x

        err_msg = r"Expected `str` of length <= 2 - at `\$.x`"
        for x in ["xxx", "𝄞xx"]:
            with pytest.raises(structtype.ValidationError, match=err_msg):
                dec.decode(proto.encode(Ex(x)))

    @pytest.mark.parametrize(
        "pattern, good, bad",
        [
            ("", ["", "test"], []),
            ("as", ["as", "ease", "ast", "pass"], ["", "nope"]),
            ("^pre[123]*$", ["pre1", "pre123"], ["apre1", "pre1two"]),
        ],
    )
    def test_pattern(self, proto, pattern, good, bad):
        class Ex(structtype.Struct):
            x: Annotated[str, StrConstraint(pattern=pattern)]

        dec = proto.Decoder(Ex)

        for x in good:
            assert dec.decode(proto.encode(Ex(x))).x == x

        err_msg = f"Expected `str` matching regex {pattern!r} - at `$.x`"
        for x in bad:
            with pytest.raises(structtype.ValidationError) as rec:
                dec.decode(proto.encode(Ex(x)))
            assert str(rec.value) == err_msg

    @pytest.mark.parametrize(
        "meta, good, bad",
        [
            (
                StrConstraint(min_length=2, max_length=3, pattern="x"),
                ["xy", "xyz"],
                ["x", "yy", "wxyz"],
            ),
            (StrConstraint(min_length=2, max_length=4), ["xx", "xxxx"], ["x", "xxxxx"]),
            (StrConstraint(min_length=2, pattern="x"), ["xy", "wxyz"], ["x", "bad"]),
            (StrConstraint(max_length=3, pattern="x"), ["xy", "xyz"], ["y", "wxyz"]),
        ],
    )
    def test_combinations(self, proto, meta, good, bad):
        class Ex(structtype.Struct):
            x: Annotated[str, meta]

        dec = proto.Decoder(Ex)

        for x in good:
            assert dec.decode(proto.encode(Ex(x))).x == x

        for x in bad:
            with pytest.raises(structtype.ValidationError):
                dec.decode(proto.encode(Ex(x)))

    @pytest.mark.parametrize(
        "meta, good, bad",
        [
            (StrConstraint(min_length=2), ["xy", "𝄞xy"], ["", "𝄞"]),
            (StrConstraint(pattern="as"), ["as", "pass", "𝄞as"], ["", "nope", "𝄞"]),
        ],
    )
    def test_str_constraints_on_dict_keys(self, proto, meta, good, bad):
        dec = proto.Decoder(dict[Annotated[str, meta], int])

        for x in good:
            assert dec.decode(proto.encode({x: 1})) == {x: 1}

        for x in bad:
            with pytest.raises(structtype.ValidationError):
                dec.decode(proto.encode({x: 1}))


class TestDateTimeConstraints:
    @staticmethod
    def roundtrip(proto, cls, aware, as_str):
        dt = datetime.datetime.now(datetime.timezone.utc if aware else None)

        if as_str:
            s = proto.encode(cls(dt.isoformat()))
        else:
            s = proto.encode(cls(dt))

        res = proto.decode(s, type=cls)
        assert res.x == dt

    @pytest.mark.parametrize("as_str", [True, False])
    def test_tz_none(self, proto, as_str):
        class Ex(structtype.Struct):
            x: datetime.datetime

        self.roundtrip(proto, Ex, True, as_str)
        self.roundtrip(proto, Ex, False, as_str)

    @pytest.mark.parametrize("as_str", [True, False])
    def test_tz_false(self, proto, as_str):
        class Ex(structtype.Struct):
            x: Annotated[datetime.datetime, TimezoneConstraint(tz=False)]

        self.roundtrip(proto, Ex, False, as_str)

        err_msg = r"Expected `datetime` with no timezone component - at `\$.x`"

        with pytest.raises(structtype.ValidationError, match=err_msg):
            self.roundtrip(proto, Ex, True, as_str)

    @pytest.mark.parametrize("as_str", [True, False])
    def test_tz_true(self, proto, as_str):
        class Ex(structtype.Struct):
            x: Annotated[datetime.datetime, TimezoneConstraint(tz=True)]

        self.roundtrip(proto, Ex, True, as_str)

        err_msg = r"Expected `datetime` with a timezone component - at `\$.x`"

        with pytest.raises(structtype.ValidationError, match=err_msg):
            self.roundtrip(proto, Ex, False, as_str)


class TestTimeConstraints:
    @staticmethod
    def roundtrip(proto, cls, aware, as_str):
        dt = datetime.datetime.now(datetime.timezone.utc if aware else None).timetz()

        if as_str:
            s = proto.encode(cls(dt.isoformat()))
        else:
            s = proto.encode(cls(dt))

        res = proto.decode(s, type=cls)
        assert res.x == dt

    @pytest.mark.parametrize("as_str", [True, False])
    def test_tz_none(self, proto, as_str):
        class Ex(structtype.Struct):
            x: datetime.time

        self.roundtrip(proto, Ex, True, as_str)
        self.roundtrip(proto, Ex, False, as_str)

    @pytest.mark.parametrize("as_str", [True, False])
    def test_tz_false(self, proto, as_str):
        class Ex(structtype.Struct):
            x: Annotated[datetime.time, TimezoneConstraint(tz=False)]

        self.roundtrip(proto, Ex, False, as_str)

        err_msg = r"Expected `time` with no timezone component - at `\$.x`"

        with pytest.raises(structtype.ValidationError, match=err_msg):
            self.roundtrip(proto, Ex, True, as_str)

    @pytest.mark.parametrize("as_str", [True, False])
    def test_tz_true(self, proto, as_str):
        class Ex(structtype.Struct):
            x: Annotated[datetime.time, TimezoneConstraint(tz=True)]

        self.roundtrip(proto, Ex, True, as_str)

        err_msg = r"Expected `time` with a timezone component - at `\$.x`"

        with pytest.raises(structtype.ValidationError, match=err_msg):
            self.roundtrip(proto, Ex, False, as_str)


class TestBytesConstraints:
    @pytest.mark.parametrize("typ", [bytes, bytearray, memoryview])
    def test_min_length(self, proto, typ):
        class Ex(structtype.Struct):
            x: Annotated[typ, BytesConstraint(min_length=2)]

        dec = proto.Decoder(Ex)

        for x in [b"xx", b"xxx"]:
            assert bytes(dec.decode(proto.encode(Ex(x))).x) == x

        err_msg = r"Expected `bytes` of length >= 2 - at `\$.x`"
        for x in [b"", b"x"]:
            with pytest.raises(structtype.ValidationError, match=err_msg):
                dec.decode(proto.encode(Ex(x)))

    @pytest.mark.parametrize("typ", [bytes, bytearray, memoryview])
    def test_max_length(self, proto, typ):
        class Ex(structtype.Struct):
            x: Annotated[typ, BytesConstraint(max_length=2)]

        dec = proto.Decoder(Ex)

        for x in [b"", b"xx"]:
            assert bytes(dec.decode(proto.encode(Ex(x))).x) == x

        err_msg = r"Expected `bytes` of length <= 2 - at `\$.x`"
        with pytest.raises(structtype.ValidationError, match=err_msg):
            dec.decode(proto.encode(Ex(b"xxx")))

    @pytest.mark.parametrize("typ", [bytes, bytearray, memoryview])
    def test_combinations(self, proto, typ):
        class Ex(structtype.Struct):
            x: Annotated[typ, BytesConstraint(min_length=2, max_length=4)]

        dec = proto.Decoder(Ex)

        for x in [b"xx", b"xxx", b"xxxx"]:
            assert bytes(dec.decode(proto.encode(Ex(x))).x) == x

        for x in [b"x", b"xxxxx"]:
            with pytest.raises(structtype.ValidationError):
                dec.decode(proto.encode(Ex(x)))


class TestArrayConstraints:
    @pytest.mark.parametrize("typ", [list, tuple, set, frozenset])
    def test_min_length(self, proto, typ):
        class Ex(structtype.Struct):
            x: Annotated[typ, CollectionConstraint(min_length=2)]

        dec = proto.Decoder(Ex)

        for n in [2, 3]:
            x = typ(range(n))
            assert dec.decode(proto.encode(Ex(x))).x == x

        err_msg = r"Expected `array` of length >= 2 - at `\$.x`"
        for n in [0, 1]:
            x = typ(range(n))
            with pytest.raises(structtype.ValidationError, match=err_msg):
                dec.decode(proto.encode(Ex(x)))

    @pytest.mark.parametrize("typ", [list, tuple, set, frozenset])
    def test_max_length(self, proto, typ):
        class Ex(structtype.Struct):
            x: Annotated[typ, CollectionConstraint(max_length=2)]

        dec = proto.Decoder(Ex)

        for n in [0, 2]:
            x = typ(range(n))
            assert dec.decode(proto.encode(Ex(x))).x == x

        err_msg = r"Expected `array` of length <= 2 - at `\$.x`"
        with pytest.raises(structtype.ValidationError, match=err_msg):
            dec.decode(proto.encode(Ex(typ(range(3)))))

    @pytest.mark.parametrize("typ", [list, tuple, set, frozenset])
    def test_combinations(self, proto, typ):
        class Ex(structtype.Struct):
            x: Annotated[typ, CollectionConstraint(min_length=2, max_length=4)]

        dec = proto.Decoder(Ex)

        for n in [2, 3, 4]:
            x = typ(range(n))
            assert dec.decode(proto.encode(Ex(x))).x == x

        for n in [1, 5]:
            x = typ(range(n))
            with pytest.raises(structtype.ValidationError):
                dec.decode(proto.encode(Ex(x)))


class TestMapConstraints:
    def test_min_length(self, proto):
        class Ex(structtype.Struct):
            x: Annotated[dict[str, int], CollectionConstraint(min_length=2)]

        dec = proto.Decoder(Ex)

        for n in [2, 3]:
            x = {str(i): i for i in range(n)}
            assert dec.decode(proto.encode(Ex(x))).x == x

        err_msg = r"Expected `object` of length >= 2 - at `\$.x`"
        for n in [0, 1]:
            x = {str(i): i for i in range(n)}
            with pytest.raises(structtype.ValidationError, match=err_msg):
                dec.decode(proto.encode(Ex(x)))

    def test_max_length(self, proto):
        class Ex(structtype.Struct):
            x: Annotated[dict[str, int], CollectionConstraint(max_length=2)]

        dec = proto.Decoder(Ex)

        for n in [0, 2]:
            x = {str(i): i for i in range(n)}
            assert dec.decode(proto.encode(Ex(x))).x == x

        err_msg = r"Expected `object` of length <= 2 - at `\$.x`"
        x = {"1": 1, "2": 2, "3": 3}
        with pytest.raises(structtype.ValidationError, match=err_msg):
            dec.decode(proto.encode(Ex(x)))

    def test_combinations(self, proto):
        class Ex(structtype.Struct):
            x: Annotated[dict[str, int], CollectionConstraint(min_length=2, max_length=4)]

        dec = proto.Decoder(Ex)

        for n in [2, 3, 4]:
            x = {str(i): i for i in range(n)}
            assert dec.decode(proto.encode(Ex(x))).x == x

        for n in [1, 5]:
            x = {str(i): i for i in range(n)}
            with pytest.raises(structtype.ValidationError):
                dec.decode(proto.encode(Ex(x)))


class TestUnionConstraints:
    def test_mix_float_and_int(self, proto):
        class Ex(structtype.Struct):
            x: (
                Annotated[int, NumericConstraint(ge=0, le=10)]
                | Annotated[float, NumericConstraint(ge=1000, le=2000)]
            )

        dec = proto.Decoder(Ex)

        for x in [0, 5, 10, 1000.0, 1234.5, 2000.0]:
            assert dec.decode(proto.encode(Ex(x))).x == x

        for x in [0.0, 10.0, 1000, 2000]:
            with pytest.raises(structtype.ValidationError):
                dec.decode(proto.encode(Ex(x)))

    def test_mix_length_constraints(self, proto):
        class Ex(structtype.Struct):
            x: (
                Annotated[dict[str, int], CollectionConstraint(min_length=1, max_length=2)]
                | Annotated[list[int], CollectionConstraint(min_length=3, max_length=4)]
                | Annotated[str, StrConstraint(min_length=5, max_length=6)]
            )

        dec = proto.Decoder(Ex)

        for x in [{"x": 1}, [1, 2, 3], "xxxxx"]:
            assert dec.decode(proto.encode(Ex(x))).x == x

        for x in [{}, [1], "x"]:
            with pytest.raises(structtype.ValidationError):
                dec.decode(proto.encode(Ex(x)))
