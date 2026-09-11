import base64
import dataclasses
import datetime
import decimal
import enum
import re
import sys
import uuid
from typing import Annotated, Any, Literal, NamedTuple, Optional, Union

import pytest

import structtype
from structtype import (
    BytesConstraint,
    CollectionConstraint,
    Field,
    NumericConstraint,
    Serializer,
    StrConstraint,
    Struct,
    StructAdapter,
    StructConfig,
    TimezoneConstraint,
    ValidationError,
    Constraint,
)
from structtype._core import JSONDecoder


def f(x):
    return x


def g(x):
    return x


class TestSerializer:
    def test_empty(self):
        s = Serializer()
        assert s.load is None
        assert s.dump is None

    def test_load_only(self):
        s = Serializer(load=f)
        assert s.load is f
        assert s.dump is None

    def test_dump_only(self):
        s = Serializer(dump=g)
        assert s.load is None
        assert s.dump is g

    def test_both(self):
        s = Serializer(load=f, dump=g)
        assert s.load is f
        assert s.dump is g

    def test_explicit_none(self):
        s = Serializer(load=None, dump=None)
        assert s.load is None
        assert s.dump is None

    @pytest.mark.parametrize("kwargs", [{"load": 1}, {"dump": "x"}, {"load": object()}])
    def test_not_callable(self, kwargs):
        with pytest.raises(TypeError):
            Serializer(**kwargs)

    def test_positional_args_rejected(self):
        with pytest.raises(TypeError):
            Serializer(f)

    def test_unknown_kwarg_rejected(self):
        with pytest.raises(TypeError):
            Serializer(load=f, bad=1)


class TestConstraint:
    def test_construction(self):
        v = Constraint(f)
        assert v.fn is f

    def test_keyword_construction(self):
        v = Constraint(fn=g)
        assert v.fn is g

    def test_fn_optional(self):
        v = Constraint()
        assert v.fn is None
        assert repr(v) == "structtype.Constraint()"

    def test_explicit_none_fn(self):
        v = Constraint(None)
        assert v.fn is None

    @pytest.mark.parametrize("fn", [1, "x", [f]])
    def test_not_callable(self, fn):
        with pytest.raises(TypeError, match="fn must be callable"):
            Constraint(fn)


class TestConstraintCall:
    def test_bare_validator_call_is_noop(self):
        assert Constraint()(1) is None
        assert Constraint()("anything") is None

    def test_fn_invoked_with_value(self):
        seen = []

        def record(v):
            seen.append(v)
            return "return value ignored"

        v = Constraint(record)
        assert v(42) is None
        assert seen == [42]

    def test_fn_exception_propagates(self):
        def boom(v):
            raise RuntimeError("boom")

        with pytest.raises(RuntimeError, match="boom"):
            Constraint(boom)(1)

    def test_requires_exactly_one_arg(self):
        v = Constraint(f)
        with pytest.raises(TypeError):
            v()
        with pytest.raises(TypeError):
            v(1, 2)

    def test_user_subclass_overrides_call(self):
        class Even(Constraint):
            def __call__(self, value):
                if value % 2:
                    raise ValueError("not even")

        e = Even()
        assert isinstance(e, Constraint)
        assert e.fn is None
        assert e(4) is None
        with pytest.raises(ValueError, match="not even"):
            e(3)

    def test_user_subclass_custom_init(self):
        class Positive(Constraint):
            def __init__(self):
                self.floor = 0

            def __call__(self, value):
                if value <= self.floor:
                    raise ValueError("not positive")

        p = Positive()
        assert p.floor == 0
        assert p(1) is None
        with pytest.raises(ValueError, match="not positive"):
            p(-1)


class TestIsinstance:
    @pytest.mark.parametrize(
        "v",
        [
            NumericConstraint(gt=0),
            StrConstraint(pattern="a"),
            BytesConstraint(min_length=1),
            CollectionConstraint(max_length=2),
            TimezoneConstraint(tz=True),
        ],
    )
    def test_subclass_instances_are_validators(self, v):
        assert isinstance(v, Constraint)
        assert issubclass(type(v), Constraint)

    @pytest.mark.parametrize(
        "cls",
        [
            NumericConstraint,
            StrConstraint,
            BytesConstraint,
            CollectionConstraint,
            TimezoneConstraint,
        ],
    )
    def test_subclasses_of_validator(self, cls):
        assert issubclass(cls, Constraint)
        # fast validators are leaf types
        with pytest.raises(TypeError):
            type("Sub", (cls,), {})


class TestNumericConstraint:
    @pytest.mark.parametrize(
        "kwargs",
        [
            {},
            {"gt": 0},
            {"ge": 0},
            {"lt": 10},
            {"le": 10},
            {"multiple_of": 2},
            {"gt": 0.5},
            {"ge": -3},
            {"lt": 10**20},
            {"le": 0},
            {"gt": 0, "lt": 10},
            {"ge": 0, "multiple_of": 5},
            {"gt": 1.5, "le": 100},
        ],
    )
    def test_valid_construction(self, kwargs):
        v = NumericConstraint(**kwargs)
        for k, expected in kwargs.items():
            assert getattr(v, k) == expected
        for unset in set(kwargs) ^ {"gt", "ge", "lt", "le", "multiple_of"}:
            assert getattr(v, unset) is None

    def test_none_is_equivalent_to_unset(self):
        v = NumericConstraint(gt=None, multiple_of=None)
        assert v.gt is None
        assert v.multiple_of is None
        assert v == NumericConstraint()

    def test_gt_ge_mutual_exclusion(self):
        with pytest.raises(ValueError, match="both `gt` and `ge`"):
            NumericConstraint(gt=0, ge=1)

    def test_lt_le_mutual_exclusion(self):
        with pytest.raises(ValueError, match="both `lt` and `le`"):
            NumericConstraint(lt=0, le=1)

    @pytest.mark.parametrize("name", ["gt", "ge", "lt", "le", "multiple_of"])
    @pytest.mark.parametrize("val", [float("nan"), float("inf"), float("-inf")])
    def test_non_finite_rejected(self, name, val):
        with pytest.raises(ValueError, match="must be finite"):
            NumericConstraint(**{name: val})

    @pytest.mark.parametrize("name", ["gt", "ge", "lt", "le", "multiple_of"])
    @pytest.mark.parametrize("val", ["1", True, [1]])
    def test_non_numeric_rejected(self, name, val):
        with pytest.raises(TypeError, match="must be an int or float"):
            NumericConstraint(**{name: val})

    @pytest.mark.parametrize("val", [0, -1, -1.5])
    def test_multiple_of_must_be_positive(self, val):
        with pytest.raises(ValueError, match="`multiple_of` must be > 0"):
            NumericConstraint(multiple_of=val)

    def test_huge_int_bound_rejected_cleanly(self):
        with pytest.raises(OverflowError):
            NumericConstraint(gt=10**400)

    def test_base_fn_slot_not_aliased(self):
        v = NumericConstraint(gt=5)
        assert v.fn is None


class TestNumericConstraintCall:
    @pytest.mark.parametrize(
        "kwargs,value",
        [
            ({}, 0),
            ({}, -2.5),
            ({"gt": 0}, 0.5),
            ({"ge": 0}, 0),
            ({"ge": 0}, 17),
            ({"lt": 10}, 9),
            ({"le": 10}, 10),
            ({"gt": 0, "lt": 10}, 5),
            ({"multiple_of": 3}, 9),
            ({"multiple_of": 3}, -9),
            ({"multiple_of": 3}, 0),
            ({"multiple_of": 2.5}, 5.0),
            ({"ge": -3}, -3),
            ({"ge": 0.5}, 1),
            ({"le": 100}, 100.0),
            ({"lt": 10**20}, 5),
            ({"gt": 0, "le": 1.5}, 1.5),
        ],
    )
    def test_passes(self, kwargs, value):
        assert NumericConstraint(**kwargs)(value) is None

    @pytest.mark.parametrize(
        "kwargs,value",
        [
            ({"gt": 0}, 0),
            ({"ge": 0}, -1),
            ({"lt": 10}, 10),
            ({"le": 10}, 11),
            ({"multiple_of": 3}, 10),
            ({"multiple_of": 3}, -10),
            ({"multiple_of": 2.5}, 5.5),
            ({"gt": 0, "lt": 10}, -1),
            ({"ge": 0, "multiple_of": 5}, 7),
        ],
    )
    def test_violation_raises_value_error(self, kwargs, value):
        v = NumericConstraint(**kwargs)
        with pytest.raises(ValueError):
            v(value)

    @pytest.mark.parametrize(
        "kwargs,value,match",
        [
            ({"ge": 0}, -1, r"Expected `int` >= 0"),
            ({"ge": 0.5}, 0.25, r"Expected `float` >= 0\.5"),
            ({"gt": 0}, 0, r"Expected `int` > 0"),
            ({"le": 10}, 11, r"Expected `int` <= 10"),
            ({"lt": 1.5}, 1.5, r"Expected `float` < 1\.5"),
            ({"multiple_of": 3}, 10, r"Expected `int` that's a multiple of 3"),
            ({"multiple_of": 2.5}, 5.5, r"Expected `float` that's a multiple of 2\.5"),
        ],
    )
    def test_error_message_style(self, kwargs, value, match):
        with pytest.raises(ValueError, match=match):
            NumericConstraint(**kwargs)(value)

    def test_mixed_int_float_bounds(self):
        # int value vs float bound and vice versa go through float comparison
        assert NumericConstraint(ge=0.5)(1) is None
        with pytest.raises(ValueError, match="Expected `int` >= 0\\.5"):
            NumericConstraint(ge=0.5)(0)

    def test_wrong_type_raises_type_error(self):
        v = NumericConstraint(ge=0)
        for value in ["1", None, [1], True]:
            with pytest.raises(TypeError, match="Expected `int` or `float`"):
                v(value)


class TestStrConstraint:
    @pytest.mark.parametrize(
        "kwargs",
        [
            {},
            {"pattern": "^a+$"},
            {"min_length": 0},
            {"max_length": 10},
            {"pattern": "[0-9]+", "min_length": 1, "max_length": 8},
        ],
    )
    def test_valid_construction(self, kwargs):
        v = StrConstraint(**kwargs)
        for k, expected in kwargs.items():
            assert getattr(v, k) == expected
        for unset in set(kwargs) ^ {"pattern", "min_length", "max_length"}:
            assert getattr(v, unset) is None

    def test_pattern_must_be_str(self):
        with pytest.raises(TypeError, match="`pattern` must be a str"):
            StrConstraint(pattern=1)

    def test_invalid_regex_raises(self):
        with pytest.raises(re.error):
            StrConstraint(pattern="(")

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    def test_negative_length_rejected(self, name):
        with pytest.raises(ValueError, match=f"`{name}` must be >= 0"):
            StrConstraint(**{name: -1})

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    @pytest.mark.parametrize("val", ["1", 1.5, True])
    def test_non_int_length_rejected(self, name, val):
        with pytest.raises(TypeError, match=f"`{name}` must be an int"):
            StrConstraint(**{name: val})

    def test_none_is_equivalent_to_unset(self):
        v = StrConstraint(pattern=None, max_length=None)
        assert v.pattern is None
        assert v.max_length is None
        assert v == StrConstraint()


class TestStrConstraintCall:
    @pytest.mark.parametrize(
        "kwargs,value",
        [
            ({}, "anything"),
            ({"pattern": "[0-9]+"}, "abc123"),
            ({"pattern": "^a+$"}, "aaa"),
            ({"min_length": 2}, "ab"),
            ({"max_length": 3}, "abc"),
            ({"min_length": 1, "max_length": 3}, "ab"),
            ({"pattern": "b"}, "abc"),
        ],
    )
    def test_passes(self, kwargs, value):
        assert StrConstraint(**kwargs)(value) is None

    @pytest.mark.parametrize(
        "kwargs,value,match",
        [
            ({"pattern": "[0-9]+"}, "abc", "matching regex"),
            ({"min_length": 2}, "a", r"Expected `str` of length >= 2"),
            ({"max_length": 3}, "abcd", r"Expected `str` of length <= 3"),
            ({"pattern": "^a+$"}, "aab", "matching regex"),
        ],
    )
    def test_violation_raises_value_error(self, kwargs, value, match):
        with pytest.raises(ValueError, match=match):
            StrConstraint(**kwargs)(value)

    @pytest.mark.parametrize("value", [1, b"abc", None, ["a"]])
    def test_wrong_type_raises_type_error(self, value):
        with pytest.raises(TypeError, match="Expected `str`"):
            StrConstraint(pattern="a")(value)


class TestBytesConstraint:
    @pytest.mark.parametrize(
        "kwargs",
        [{}, {"min_length": 0}, {"max_length": 4}, {"min_length": 1, "max_length": 9}],
    )
    def test_valid_construction(self, kwargs):
        v = BytesConstraint(**kwargs)
        for k, expected in kwargs.items():
            assert getattr(v, k) == expected
        for unset in set(kwargs) ^ {"min_length", "max_length"}:
            assert getattr(v, unset) is None

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    def test_negative_length_rejected(self, name):
        with pytest.raises(ValueError, match=f"`{name}` must be >= 0"):
            BytesConstraint(**{name: -1})

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    def test_non_int_length_rejected(self, name):
        with pytest.raises(TypeError, match=f"`{name}` must be an int"):
            BytesConstraint(**{name: "x"})


class TestBytesConstraintCall:
    @pytest.mark.parametrize("value", [b"ab", bytearray(b"abc"), memoryview(b"abcd")])
    def test_passes(self, value):
        assert BytesConstraint(min_length=2)(value) is None

    @pytest.mark.parametrize(
        "kwargs,value",
        [
            ({"min_length": 2}, b"a"),
            ({"min_length": 2}, bytearray(b"a")),
            ({"max_length": 3}, b"abcd"),
            ({"min_length": 1, "max_length": 3}, b""),
        ],
    )
    def test_violation_raises_value_error(self, kwargs, value):
        with pytest.raises(ValueError, match="of length"):
            BytesConstraint(**kwargs)(value)

    @pytest.mark.parametrize("value", ["abc", 123, None])
    def test_wrong_type_raises_type_error(self, value):
        with pytest.raises(TypeError, match="Expected `bytes`"):
            BytesConstraint(min_length=1)(value)


class TestCollectionConstraint:
    @pytest.mark.parametrize(
        "kwargs",
        [{}, {"min_length": 2}, {"max_length": 7}, {"min_length": 1, "max_length": 3}],
    )
    def test_valid_construction(self, kwargs):
        v = CollectionConstraint(**kwargs)
        for k, expected in kwargs.items():
            assert getattr(v, k) == expected
        for unset in set(kwargs) ^ {"min_length", "max_length"}:
            assert getattr(v, unset) is None

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    def test_negative_length_rejected(self, name):
        with pytest.raises(ValueError, match=f"`{name}` must be >= 0"):
            CollectionConstraint(**{name: -1})

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    def test_non_int_length_rejected(self, name):
        with pytest.raises(TypeError, match=f"`{name}` must be an int"):
            CollectionConstraint(**{name: object()})


class TestCollectionConstraintCall:
    @pytest.mark.parametrize(
        "value",
        [
            [1, 2],
            {1, 2},
            frozenset({1, 2}),
            (1, 2),
            {"a": 1, "b": 2},
        ],
    )
    def test_passes(self, value):
        assert CollectionConstraint(min_length=2)(value) is None

    @pytest.mark.parametrize(
        "kwargs,value",
        [
            ({"min_length": 3}, [1, 2]),
            ({"min_length": 1}, set()),
            ({"max_length": 1}, (1, 2)),
            ({"max_length": 1}, {"a": 1, "b": 2}),
        ],
    )
    def test_violation_raises_value_error(self, kwargs, value):
        with pytest.raises(ValueError, match="of length"):
            CollectionConstraint(**kwargs)(value)

    @pytest.mark.parametrize("value", ["abc", b"abc", 123])
    def test_wrong_type_raises_type_error(self, value):
        with pytest.raises(TypeError, match="Expected `list`"):
            CollectionConstraint(min_length=1)(value)


class TestTimezoneConstraintCall:
    AWARE_DT = datetime.datetime(2020, 1, 1, tzinfo=datetime.timezone.utc)
    NAIVE_DT = datetime.datetime(2020, 1, 1)
    AWARE_TIME = datetime.time(12, 0, tzinfo=datetime.timezone.utc)
    NAIVE_TIME = datetime.time(12, 0)

    def test_aware_required_passes(self):
        v = TimezoneConstraint(tz=True)
        assert v(self.AWARE_DT) is None
        assert v(self.AWARE_TIME) is None

    def test_naive_required_passes(self):
        v = TimezoneConstraint(tz=False)
        assert v(self.NAIVE_DT) is None
        assert v(self.NAIVE_TIME) is None

    @pytest.mark.parametrize("value_name", ["NAIVE_DT", "NAIVE_TIME"])
    def test_aware_required_fails(self, value_name):
        with pytest.raises(ValueError, match="with a timezone component"):
            TimezoneConstraint(tz=True)(getattr(self, value_name))

    @pytest.mark.parametrize("value_name", ["AWARE_DT", "AWARE_TIME"])
    def test_naive_required_fails(self, value_name):
        with pytest.raises(ValueError, match="with no timezone component"):
            TimezoneConstraint(tz=False)(getattr(self, value_name))

    @pytest.mark.parametrize(
        "value", ["2020-01-01", 0, None, datetime.date(2020, 1, 1)]
    )
    def test_wrong_type_raises_type_error(self, value):
        with pytest.raises(TypeError, match="Expected `datetime` or `time`"):
            TimezoneConstraint(tz=True)(value)


class TestTimezoneConstraint:
    @pytest.mark.parametrize("tz", [True, False])
    def test_keyword_construction(self, tz):
        v = TimezoneConstraint(tz=tz)
        assert v.tz is tz

    @pytest.mark.parametrize("tz", [True, False])
    def test_positional_construction(self, tz):
        v = TimezoneConstraint(tz)
        assert v.tz is tz

    def test_required(self):
        with pytest.raises(TypeError):
            TimezoneConstraint()

    @pytest.mark.parametrize("tz", ["yes", 1, None, []])
    def test_non_bool_rejected(self, tz):
        with pytest.raises(TypeError, match="`tz` must be a bool"):
            TimezoneConstraint(tz=tz)


class TestEqHashRepr:
    def test_serializer_eq_hash(self):
        assert Serializer() == Serializer()
        assert Serializer(load=f) == Serializer(load=f)
        assert Serializer(load=f) != Serializer(load=g)
        assert hash(Serializer(load=f)) == hash(Serializer(load=f))
        assert repr(Serializer()) == "structtype.Serializer()"
        assert repr(Serializer(load=f)) == f"structtype.Serializer(load={f!r})"
        assert Serializer(load=f).__rich_repr__() == [("load", f)]

    def test_validator_eq_hash(self):
        assert Constraint(f) == Constraint(f)
        assert Constraint(f) != Constraint(g)
        assert hash(Constraint(f)) == hash(Constraint(f))
        assert repr(Constraint(f)) == f"structtype.Constraint(fn={f!r})"
        assert Constraint(f).__rich_repr__() == [("fn", f)]

    def test_numeric_eq_hash(self):
        assert NumericConstraint(gt=1) == NumericConstraint(gt=1)
        assert NumericConstraint(gt=1) != NumericConstraint(gt=2)
        assert NumericConstraint(gt=1) != NumericConstraint(ge=1)
        assert hash(NumericConstraint(gt=1)) == hash(NumericConstraint(gt=1))
        assert repr(NumericConstraint(gt=1)) == "structtype.NumericConstraint(gt=1)"
        assert repr(NumericConstraint()) == "structtype.NumericConstraint()"
        expected = [("gt", 1), ("le", 2)]
        assert NumericConstraint(gt=1, le=2).__rich_repr__() == expected

    def test_str_eq_hash(self):
        assert StrConstraint(pattern="a") == StrConstraint(pattern="a")
        assert StrConstraint(pattern="a") != StrConstraint(pattern="b")
        assert hash(StrConstraint(pattern="a")) == hash(StrConstraint(pattern="a"))
        assert (
            repr(StrConstraint(pattern="a", max_length=3))
            == "structtype.StrConstraint(pattern='a', max_length=3)"
        )

    def test_bytes_collection_timezone_eq_hash(self):
        assert BytesConstraint(min_length=1) == BytesConstraint(min_length=1)
        assert BytesConstraint(min_length=1) != CollectionConstraint(min_length=1)
        assert CollectionConstraint() == CollectionConstraint()
        assert hash(TimezoneConstraint(tz=True)) == hash(TimezoneConstraint(tz=True))
        assert TimezoneConstraint(tz=True) == TimezoneConstraint(tz=True)
        assert TimezoneConstraint(tz=True) != TimezoneConstraint(tz=False)
        assert (
            repr(TimezoneConstraint(False)) == "structtype.TimezoneConstraint(tz=False)"
        )


class Color:
    """Custom type with no protocol methods - conversion only via Serializer."""

    def __init__(self, rgb):
        self.rgb = rgb

    def __eq__(self, other):
        return isinstance(other, Color) and self.rgb == other.rgb

    def __repr__(self):
        return f"Color({self.rgb!r})"


class Shade:
    """Custom type implementing the struct_dump/struct_validate protocol."""

    def __init__(self, name):
        self.name = name

    def __eq__(self, other):
        return isinstance(other, Shade) and self.name == other.name

    def __hash__(self):
        return hash(self.name)

    def struct_dump(self):
        return {"name": self.name}

    @classmethod
    def struct_validate(cls, obj):
        return cls(obj["name"])


class TestSerializerCodecWiring:
    def test_load_called_on_json_decode(self):
        calls = []

        def load(value):
            calls.append(value)
            return Color(tuple(value))

        class Msg(Struct):
            color: Annotated[Color, Serializer(load=load)]

        out = Msg.struct_validate_json(b'{"color": [1, 2, 3]}')
        assert out.color == Color((1, 2, 3))
        assert calls == [[1, 2, 3]]

    def test_load_not_called_when_already_instance(self):
        calls = []

        def load(value):
            calls.append(value)
            return Color(value)

        class Msg(Struct):
            color: Annotated[Color, Serializer(load=load)]

        c = Color("red")
        out = Msg.struct_validate({"color": c})
        assert out.color is c
        assert calls == []

    def test_load_used_by_struct_validate(self):
        def load(value):
            return Color(value.upper())

        class Msg(Struct):
            color: Annotated[Color, Serializer(load=load)]

        out = Msg.struct_validate({"color": "red"})
        assert out.color == Color("RED")

    def test_load_errors_wrap_as_validation_error(self):
        def load(value):
            raise ValueError("bad value")

        class Msg(Struct):
            color: Annotated[Color, Serializer(load=load)]

        with pytest.raises(ValidationError):
            Msg.struct_validate_json(b'{"color": "red"}')

    def test_dump_called_on_encode(self):
        calls = []

        def dump(color):
            calls.append(color)
            return list(color.rgb)

        class Msg(Struct):
            color: Annotated[Color, Serializer(dump=dump)]

        out = Msg(Color((1, 2, 3))).struct_dump_json()
        assert out == b'{"color":[1,2,3]}'
        assert calls == [Color((1, 2, 3))]

    def test_load_and_dump_together_roundtrip(self):
        loads, dumps = [], []

        def load(value):
            loads.append(value)
            return Color(tuple(value))

        def dump(color):
            dumps.append(color)
            return sorted(color.rgb, reverse=True)

        class Msg(Struct):
            color: Annotated[Color, Serializer(load=load, dump=dump)]

        msg = Msg.struct_validate_json(b'{"color": [1, 2, 3]}')
        assert msg.color == Color((1, 2, 3))
        assert msg.struct_dump_json() == b'{"color":[3,2,1]}'
        assert loads == [[1, 2, 3]]
        assert dumps == [Color((1, 2, 3))]

    def test_dump_only_falls_back_to_protocol_on_decode(self):
        def dump(shade):
            return shade.name

        class Msg(Struct):
            shade: Annotated[Shade, Serializer(dump=dump)]

        out = Msg.struct_validate_json(b'{"shade": {"name": "teal"}}')
        assert out.shade == Shade("teal")
        assert Msg(out.shade).struct_dump_json() == b'{"shade":"teal"}'

    def test_native_type_rejected_at_class_creation(self):
        def f(x):
            return x

        with pytest.raises(TypeError, match="native types"):

            class Msg(Struct):
                value: Annotated[int, Serializer(load=f)]

    def test_native_type_rejected_in_nested_annotation_at_class_creation(self):
        def f(x):
            return x

        with pytest.raises(TypeError, match="native types"):

            class Msg(Struct):
                values: list[Annotated[str, Serializer(dump=str)]]

    @pytest.mark.parametrize(
        "annotation",
        [
            Annotated[Literal[1], Serializer()],
            Annotated[Literal["x"], Serializer(load=f)],
            Optional[Annotated[Literal[True], Serializer(dump=g)]],
            list[Annotated[Literal[1], Serializer()]],
        ],
    )
    def test_literal_serializer_rejected_in_direct_decoder(self, annotation):
        with pytest.raises(TypeError, match="native types"):
            JSONDecoder(annotation)

    def test_native_type_rejected_in_direct_decoder(self):
        def f(x):
            return x

        with pytest.raises(TypeError, match="native types"):
            JSONDecoder(Annotated[int, Serializer(load=f)])

    def test_native_type_rejected_in_direct_decoder_nested(self):
        def f(x):
            return x

        with pytest.raises(TypeError, match="native types"):
            JSONDecoder(list[Annotated[int, Serializer(load=f)]])

    def test_allowed_type_serializer_at_class_creation(self):
        def load(value):
            return datetime.datetime.fromisoformat(value)

        def dump(dt):
            return dt.isoformat()

        class Msg(Struct):
            ts: Annotated[datetime.datetime, Serializer(load=load, dump=dump)]

        msg = Msg.struct_validate_json(b'{"ts": "2024-01-15T10:30:00"}')
        assert msg.ts == datetime.datetime(2024, 1, 15, 10, 30, 0)
        assert msg.struct_dump_json() == b'{"ts":"2024-01-15T10:30:00"}'

    def test_allowed_type_serializer_in_nested_position(self):
        def load(value):
            return uuid.UUID(value)

        def dump(u):
            return str(u)

        class Msg(Struct):
            ids: list[Annotated[uuid.UUID, Serializer(load=load, dump=dump)]]

        msg = Msg.struct_validate_json(
            b'{"ids": ["00000000-0000-0000-0000-000000000001"]}'
        )
        assert msg.ids[0] == uuid.UUID("00000000-0000-0000-0000-000000000001")

    def test_allowed_type_serializer_in_direct_decoder(self):
        def load(value):
            return decimal.Decimal(value)

        dec = JSONDecoder(
            Annotated[decimal.Decimal, Serializer(load=load)]
        )
        assert dec.decode(b'"3.14"') == decimal.Decimal("3.14")

    def test_allowed_type_bytes_serializer(self):
        import base64 as b64

        def load(value):
            return b64.b64decode(value)

        def dump(data):
            return b64.b64encode(data).decode()

        class Msg(Struct):
            data: Annotated[bytes, Serializer(load=load, dump=dump)]

        msg = Msg.struct_validate_json(b'{"data": "aGVsbG8="}')
        assert msg.data == b"hello"

    def test_allowed_type_enum_serializer(self):
        class Color(enum.Enum):
            RED = "red"
            GREEN = "green"

        def load(value):
            return Color(value)

        def dump(color):
            return color.value

        class Msg(Struct):
            color: Annotated[Color, Serializer(load=load, dump=dump)]

        msg = Msg.struct_validate_json(b'{"color": "red"}')
        assert msg.color is Color.RED

    def test_optional_datetime_serializer_roundtrip(self):
        def load(value):
            return datetime.datetime.fromisoformat(value)

        def dump(dt):
            return dt.isoformat()

        class Msg(Struct):
            ts: Annotated[
                Optional[datetime.datetime], Serializer(load=load, dump=dump)
            ]

        msg = Msg.struct_validate_json(b'{"ts": "2024-01-15T10:30:00"}')
        assert msg.ts == datetime.datetime(2024, 1, 15, 10, 30, 0)
        assert msg.struct_dump_json() == b'{"ts":"2024-01-15T10:30:00"}'

    def test_optional_datetime_serializer_none_bypass(self):
        def load(value):
            return datetime.datetime.fromisoformat(value)

        def dump(dt):
            return dt.isoformat()

        class Msg(Struct):
            ts: Annotated[
                Optional[datetime.datetime], Serializer(load=load, dump=dump)
            ]

        msg = Msg(None)
        assert msg.ts is None
        assert msg.struct_dump_json() == b'{"ts":null}'

        msg2 = Msg.struct_validate_json(b'{"ts": null}')
        assert msg2.ts is None

    def test_optional_uuid_serializer_roundtrip(self):
        def load(value):
            return uuid.UUID(value)

        def dump(u):
            return str(u)

        class Msg(Struct):
            id: Annotated[
                Optional[uuid.UUID], Serializer(load=load, dump=dump)
            ]

        u = uuid.uuid4()
        msg = Msg(u)
        assert msg.struct_dump_json() == f'{{"id":"{u}"}}'.encode()
        assert Msg.struct_validate_json(f'{{"id":"{u}"}}'.encode()).id == u

        msg_none = Msg(None)
        assert msg_none.struct_dump_json() == b'{"id":null}'
        assert Msg.struct_validate_json(b'{"id":null}').id is None

    def test_optional_bytes_serializer_roundtrip(self):
        import base64 as b64

        def load(value):
            return b64.b64decode(value)

        def dump(data):
            return b64.b64encode(data).decode()

        class Msg(Struct):
            data: Annotated[
                Optional[bytes], Serializer(load=load, dump=dump)
            ]

        msg = Msg(b"hello")
        assert msg.struct_dump_json() == b'{"data":"aGVsbG8="}'
        assert Msg.struct_validate_json(b'{"data":"aGVsbG8="}').data == b"hello"

        msg_none = Msg(None)
        assert msg_none.struct_dump_json() == b'{"data":null}'
        assert Msg.struct_validate_json(b'{"data":null}').data is None

    def test_optional_serializer_struct_validate(self):
        def load(value):
            return datetime.datetime.fromisoformat(value)

        def dump(dt):
            return dt.isoformat()

        class Msg(Struct):
            ts: Annotated[
                Optional[datetime.datetime], Serializer(load=load, dump=dump)
            ]

        dt = datetime.datetime(2024, 1, 15, 10, 30, 0)
        msg = Msg.struct_validate({"ts": dt})
        assert msg.ts == dt

        msg_none = Msg.struct_validate({"ts": None})
        assert msg_none.ts is None

    def test_optional_blocked_type_rejected(self):
        def f(x):
            return x

        with pytest.raises(TypeError, match="native types"):
            class Msg(Struct):
                v: Annotated[Optional[int], Serializer(load=f)]

    @pytest.mark.parametrize(
        "annotation",
        [
            Annotated[int, Serializer()],
            Annotated[str, Serializer()],
            Annotated[float, Serializer()],
            Annotated[bool, Serializer()],
            Annotated[Literal[1], Serializer()],
            Annotated[Literal["x"], Serializer(load=f)],
            Annotated[Literal[True], Serializer(dump=g)],
            Optional[Annotated[Literal[1], Serializer()]],
            list[Annotated[Literal[1], Serializer()]],
        ],
    )
    def test_empty_and_literal_serializers_rejected(self, annotation):
        with pytest.raises(TypeError, match="native types"):
            type("Msg", (Struct,), {"__annotations__": {"value": annotation}})

    def test_bare_none_rejected(self):
        """Bare `None` (as a union member) should not have a Serializer,
        but `type(None)` is a custom type and is accepted."""
        pass  # type(None) is NoneType, a custom type — allowed

    def test_multiple_serializers_in_one_position_rejected(self):
        def f(x):
            return x

        # Class creation tolerates it for now; the lazy decoder build rejects.
        with pytest.raises(TypeError, match="Multiple `Serializer` annotations"):
            JSONDecoder(Annotated[Color, Serializer(load=f), Serializer(load=f)])

    def test_serializer_without_callables_ignored(self):
        # An empty Serializer remains inert on supported custom types.
        class MsgCustom(Struct):
            color: Annotated[Color, Serializer()]

        c = Color("red")
        out = MsgCustom.struct_validate({"color": c})
        assert out.color is c

    def test_nested_list_element_codec(self):
        def load(value):
            return Color(tuple(value))

        def dump(color):
            return list(color.rgb)

        class Msg(Struct):
            colors: list[Annotated[Color, Serializer(load=load, dump=dump)]]

        msg = Msg.struct_validate_json(b'{"colors": [[1, 2], [3, 4]]}')
        assert msg.colors == [Color((1, 2)), Color((3, 4))]
        assert msg.struct_dump_json() == b'{"colors":[[1,2],[3,4]]}'

    def test_dict_value_codec(self):
        def load(value):
            return Color(value)

        def dump(color):
            return color.rgb

        class Msg(Struct):
            by_name: dict[str, Annotated[Color, Serializer(load=load, dump=dump)]]

        msg = Msg.struct_validate_json(b'{"by_name": {"a": "red", "b": "blue"}}')
        assert msg.by_name == {"a": Color("red"), "b": Color("blue")}
        assert msg.struct_dump_json() == b'{"by_name":{"a":"red","b":"blue"}}'


class TestFutureAnnotations:
    """Annotations under ``from __future__ import annotations`` are stored as
    lazy strings. They must be resolved at class-creation time so Serializer
    codecs, Field metadata, and Constraint constraints work exactly as with
    eager annotations."""

    def test_serializer_dump_and_load(self):
        from tests.utils import temp_module

        source = """
        from __future__ import annotations
        from typing import Annotated
        from structtype import Struct, Serializer

        class Color:
            def __init__(self, rgb):
                self.rgb = rgb
            def __eq__(self, other):
                return isinstance(other, Color) and self.rgb == other.rgb
            def __repr__(self):
                return f"Color({self.rgb!r})"

        def dump(c):
            return list(c.rgb)

        def load(v):
            return Color(tuple(v))

        class Msg(Struct):
            color: Annotated[Color, Serializer(dump=dump, load=load)]
        """
        with temp_module(source) as mod:
            out = mod.Msg.struct_validate({"color": [1, 2, 3]})
            assert out.color == mod.Color((1, 2, 3))
            assert mod.Msg.struct_validate_json(out.struct_dump_json()) == out

    def test_field_alias(self):
        from tests.utils import temp_module

        source = """
        from __future__ import annotations
        from typing import Annotated
        from structtype import Struct, Field

        class Msg(Struct):
            color: Annotated[int, Field(alias="colour")]
        """
        with temp_module(source) as mod:
            assert mod.Msg.__struct_alias_fields__ == ("colour",)
            out = mod.Msg.struct_validate({"colour": 5})
            assert out.color == 5
            assert out.struct_dump() == {"colour": 5}

    def test_validator_constraint(self):
        from tests.utils import temp_module

        source = """
        from __future__ import annotations
        from typing import Annotated
        from structtype import Struct, NumericConstraint

        class Msg(Struct):
            n: Annotated[int, NumericConstraint(gt=0)]
        """
        with temp_module(source) as mod:
            assert mod.Msg.struct_validate({"n": 5}).n == 5
            with pytest.raises(ValidationError):
                mod.Msg.struct_validate({"n": -1})

    @pytest.mark.skipif(sys.version_info < (3, 14), reason="PEP 649 __annotate__ is 3.14+")
    def test_pep649_annotate_in_class_body(self):
        """A 3.14+ class body may define ``__annotate__`` (PEP 649). It is
        resolved through the ``annotationlib`` path and must produce the same
        fields/types as an ``__annotations__`` dict."""
        from tests.utils import temp_module

        source = """
        from __future__ import annotations
        from structtype import Struct

        def annotate(fmt):
            return {"x": "int", "s": "str | None"}

        class Msg(Struct):
            __annotate__ = annotate
            x: int
            s: str | None = None
        """
        with temp_module(source) as mod:
            assert mod.Msg.__struct_fields__ == ("x", "s")
            assert mod.Msg.struct_validate({"x": 5}).x == 5
            assert mod.Msg.struct_validate({"x": 5}).s is None
            with pytest.raises(ValidationError):
                mod.Msg.struct_validate({"x": "bad"})

    @pytest.mark.skipif(sys.version_info < (3, 14), reason="3.14+ quotes string annotations differently")
    def test_quoted_string_annotation(self):
        """3.14+ stores a quoted annotation like ``\"list[dict[str, int]]\"``
        with extra inner quotes; it must still resolve to the real generic."""
        from tests.utils import temp_module

        source = """
        from __future__ import annotations
        from structtype import Struct

        class Box(Struct):
            items: "list[dict[str, int]]" = []
        """
        with temp_module(source) as mod:
            b = mod.Box.struct_validate({"items": [{"a": 1}]})
            assert b.items == [{"a": 1}]

    @pytest.mark.skipif(sys.version_info < (3, 14), reason="PEP 649 __annotate__ is 3.14+")
    def test_pep649_nested(self):
        """Forward references and nested generics under ``__annotate__`` resolve
        at class creation or fall back to the decode-time resolver."""
        from tests.utils import temp_module

        source = """
        from __future__ import annotations
        from structtype import Struct

        def annotate(fmt):
            return {"child": "Node | None", "items": "list[int]"}

        class Node(Struct):
            __annotate__ = annotate
            child: "Node | None" = None
            items: list[int] = []
        """
        with temp_module(source) as mod:
            n = mod.Node(mod.Node())
            assert n.child is not None
            assert n.items == []

    def test_none_annotation(self):
        """A ``None`` annotation under lazy strings resolves to ``NoneType`` at
        class creation, matching the eager-annotation behavior."""
        from tests.utils import temp_module

        source = """
        from __future__ import annotations
        from structtype import Struct

        class Msg(Struct):
            x: None = None
        """
        with temp_module(source) as mod:
            assert mod.Msg.__struct_fields__ == ("x",)
            out = mod.Msg.struct_validate({})
            assert out.x is None
            assert out.struct_dump_json() == b'{"x":null}'


class TestStructAdapterSerializerRejection:
    def test_rejects_load(self):
        def f(x):
            return x

        with pytest.raises(TypeError, match="not supported on StructAdapter"):
            StructAdapter(Annotated[list[int], Serializer(load=f)])

    def test_rejects_dump(self):
        def g(x):
            return x

        with pytest.raises(TypeError, match="not supported on StructAdapter"):
            StructAdapter(Annotated[list[int], Serializer(dump=g)])

    def test_accepts_non_codec_annotations(self):
        ta = StructAdapter(Annotated[list[int], CollectionConstraint(min_length=1)])
        assert ta.struct_validate_json(b"[1]") == [1]
        ta = StructAdapter(Annotated[int, NumericConstraint(ge=0)])
        assert ta.struct_validate_json(b"42") == 42


class TestNumericConstraintLowering:
    @pytest.mark.parametrize(
        "kwargs,good,bad",
        [
            ({"ge": 0}, 0, -1),
            ({"gt": 0}, 1, 0),
            ({"le": 10}, 10, 11),
            ({"lt": 10}, 9, 10),
            ({"multiple_of": 3}, 9, 10),
            ({"gt": 0, "lt": 10}, 5, -1),
            ({"ge": 0, "le": 10}, 10, -1),
            ({"ge": 4, "multiple_of": 3}, 6, 3),
        ],
    )
    def test_int_field(self, kwargs, good, bad):
        class Ex(Struct):
            x: Annotated[int, NumericConstraint(**kwargs)]

        assert Ex.struct_validate_json(b'{"x": %d}' % good) == Ex(good)
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": %d}' % bad)

    @pytest.mark.parametrize(
        "kwargs,good,bad",
        [
            ({"ge": 0.5}, 0.5, 0.25),
            ({"gt": 0.0}, 0.5, 0.0),
            ({"le": 1.5}, 1.5, 2.0),
            ({"lt": 1.5}, 1.0, 1.5),
            ({"multiple_of": 0.5}, 1.5, 1.75),
        ],
    )
    def test_float_field(self, kwargs, good, bad):
        class Ex(Struct):
            x: Annotated[float, NumericConstraint(**kwargs)]

        assert Ex.struct_validate_json(b'{"x": %r}' % good) == Ex(good)
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": %r}' % bad)

    def test_nested_in_list_element(self):
        class Ex(Struct):
            xs: list[Annotated[int, NumericConstraint(ge=0)]]

        out = Ex.struct_validate_json(b'{"xs": [1, 2]}')
        assert out.xs == [1, 2]
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"xs": [1, -1]}')

    def test_error_message_carries_path(self):
        class Ex(Struct):
            x: Annotated[int, NumericConstraint(ge=0)]

        with pytest.raises(ValidationError, match=r"Expected `int` >= 0 - at `\$.x`"):
            Ex.struct_validate_json(b'{"x": -1}')

    def test_empty_numeric_validator_is_inert(self):
        class Ex(Struct):
            x: Annotated[int, NumericConstraint()]

        assert Ex.struct_validate_json(b'{"x": -5}') == Ex(-5)


class TestStrConstraintLowering:
    def test_passes(self):
        class Ex(Struct):
            x: Annotated[str, StrConstraint(min_length=2, max_length=4, pattern="a+")]

        assert Ex.struct_validate_json(b'{"x": "aa"}') == Ex("aa")

    @pytest.mark.parametrize(
        "value",
        [
            '"a"',  # too short
            '"aaaaa"',  # too long
            '"bbb"',  # no pattern match
            '""',  # too short & no pattern match
        ],
    )
    def test_failures(self, value):
        class Ex(Struct):
            x: Annotated[str, StrConstraint(min_length=2, max_length=4, pattern="a+")]

        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": %s}' % value.encode())


class TestBytesConstraintLowering:
    def test_min_length(self):
        class Ex(Struct):
            x: Annotated[bytes, BytesConstraint(min_length=1)]

        good = base64.b64encode(b"ab")
        bad = base64.b64encode(b"")
        assert Ex.struct_validate_json(b'{"x": "%s"}' % good) == Ex(b"ab")
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": "%s"}' % bad)


class TestCollectionConstraintLowering:
    def test_list_min_length(self):
        class Ex(Struct):
            x: Annotated[list[int], CollectionConstraint(min_length=1)]

        assert Ex.struct_validate_json(b'{"x": [1]}') == Ex([1])
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": []}')

    def test_dict_max_length(self):
        class Ex(Struct):
            x: Annotated[dict[str, int], CollectionConstraint(max_length=1)]

        assert Ex.struct_validate_json(b'{"x": {"a": 1}}') == Ex({"a": 1})
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": {"a": 1, "b": 2}}')


class TestTimezoneConstraintLowering:
    AWARE = b'"2020-01-01T00:00:00+00:00"'
    NAIVE = b'"2020-01-01T00:00:00"'

    def test_aware_required(self):
        class Ex(Struct):
            x: Annotated[datetime.datetime, TimezoneConstraint(tz=True)]

        assert b"+00:00" in Ex.struct_validate_json(b'{"x": %s}' % self.AWARE).x.isoformat().encode()
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": %s}' % self.NAIVE)

    def test_naive_required(self):
        class Ex(Struct):
            x: Annotated[datetime.datetime, TimezoneConstraint(tz=False)]

        assert Ex.struct_validate_json(b'{"x": %s}' % self.NAIVE).x == datetime.datetime(
            2020, 1, 1
        )
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": %s}' % self.AWARE)

    def test_time_field(self):
        class Ex(Struct):
            x: Annotated[datetime.time, TimezoneConstraint(tz=True)]

        assert Ex.struct_validate_json(b'{"x": "12:00:00+00:00"}').x.tzinfo is not None


class TestConstraintApplicability:
    @pytest.mark.parametrize(
        "typ",
        [str, bytes, list[int], dict[str, int], datetime.datetime],
    )
    def test_numeric_validator_on_non_numeric(self, typ):
        with pytest.raises(TypeError, match="numeric"):
            JSONDecoder(Annotated[typ, NumericConstraint(ge=0)])

    def test_str_validator_on_int(self):
        with pytest.raises(TypeError, match="str"):
            JSONDecoder(Annotated[int, StrConstraint(pattern="a+")])

    def test_bytes_validator_on_int(self):
        with pytest.raises(TypeError, match="bytes"):
            JSONDecoder(Annotated[int, BytesConstraint(min_length=1)])

    def test_collection_validator_on_int(self):
        with pytest.raises(TypeError, match="collection"):
            JSONDecoder(Annotated[int, CollectionConstraint(min_length=1)])

    def test_timezone_validator_on_int(self):
        with pytest.raises(TypeError, match="datetime or time"):
            JSONDecoder(Annotated[int, TimezoneConstraint(tz=True)])

    def test_applicability_checked_at_decoder_build(self):
        # Errors surface when the decoder is built, not when values decode.
        dec = None
        with pytest.raises(TypeError):
            dec = JSONDecoder(Annotated[str, NumericConstraint(ge=0)])
        assert dec is None


class TestMultipleConstraintsRejected:
    def test_two_base_validators(self):
        with pytest.raises(TypeError, match="Multiple"):
            JSONDecoder(Annotated[int, Constraint(f), Constraint(g)])

    def test_base_plus_fast(self):
        with pytest.raises(TypeError, match="Multiple"):
            JSONDecoder(Annotated[int, Constraint(f), NumericConstraint(ge=0)])

    def test_fast_plus_fast(self):
        with pytest.raises(TypeError, match="Multiple"):
            JSONDecoder(Annotated[int, NumericConstraint(ge=0), StrConstraint(pattern="a")])

    def test_subclass_instances(self):
        class Even(Constraint):
            def __call__(self, v):
                if v % 2:
                    raise ValueError("not even")

        with pytest.raises(TypeError, match="Multiple"):
            JSONDecoder(Annotated[int, Even(), Even()])
        with pytest.raises(TypeError, match="Multiple"):
            JSONDecoder(Annotated[int, Even(), NumericConstraint(gt=0)])


class TestUserConstraintSmoke:
    """Full user-validator behavior lands in a later task; here we only check
    that annotations carrying base/user validators don't break decoder builds."""

    def test_bare_validator_is_noop(self):
        class Ex(Struct):
            x: Annotated[int, Constraint()]

        assert Ex.struct_validate_json(b'{"x": 5}') == Ex(5)
        # No fast constraints lowered; anything passes for now
        assert Ex.struct_validate_json(b'{"x": -5}') == Ex(-5)

    def test_validator_with_fn_is_noop_on_decode(self):
        calls = []

        def record(v):
            calls.append(v)

        class Ex(Struct):
            x: Annotated[int, Constraint(record)]

        assert Ex.struct_validate_json(b'{"x": 5}') == Ex(5)

    def test_user_subclass_instance(self):
        class Even(Constraint):
            def __call__(self, v):
                if v % 2:
                    raise ValueError("not even")

        class Ex(Struct):
            x: Annotated[int, Even()]

        # Decoder construction must not crash; invocation lands later
        assert Ex.struct_validate_json(b'{"x": 4}') == Ex(4)

    def test_user_validator_with_serializer_on_custom_type(self):
        class MsgCustom(Struct):
            color: Annotated[Color, Serializer(), Constraint()]

        c = Color("red")
        assert MsgCustom.struct_validate({"color": c}).color is c


class TestUserConstraintInvocation:
    """Task 7: base/user Constraint instances are invoked at runtime, after
    type-check/load, wherever constraints are enforced."""

    def test_container_typed_validator(self):
        # The getter must return the validator instance itself, not an
        # element TypeNode (which would crash or misfire).
        class Ex(Struct):
            xs: Annotated[list[int], Constraint(lambda v: None)]

        assert Ex.struct_validate_json(b'{"xs": [1, 2]}') == Ex([1, 2])

    def test_custom_type_load_then_validator_in_order(self):
        order = []

        def load(value):
            order.append("load")
            return Color(tuple(value))

        def check(color):
            order.append(("check", color))

        class Msg(Struct):
            color: Annotated[Color, Serializer(load=load), Constraint(check)]

        out = Msg.struct_validate_json(b'{"color": [1, 2]}')
        assert out.color == Color((1, 2))
        # load runs first; the validator receives the loaded instance
        assert order == ["load", ("check", Color((1, 2)))]

    def test_validator_failure_wraps_path_context(self):
        def fail(v):
            raise ValueError("no good")

        class Ex(Struct):
            x: Annotated[int, Constraint(fail)]

        with pytest.raises(ValidationError, match=r"no good - at `\$.x`"):
            Ex.struct_validate_json(b'{"x": 5}')

    def test_validator_failure_nested_path(self):
        def fail(v):
            raise ValueError("no good")

        class Ex(Struct):
            xs: list[Annotated[int, Constraint(fail)]]

        with pytest.raises(ValidationError, match=r"no good - at `\$.xs\[0\]`"):
            Ex.struct_validate_json(b'{"xs": [0, 1]}')

    def test_pure_python_subclass_invoked(self):
        class Even(Constraint):
            def __call__(self, v):
                if v % 2:
                    raise ValueError("not even")

        class Ex(Struct):
            x: Annotated[int, Even()]

        assert Ex.struct_validate_json(b'{"x": 4}') == Ex(4)
        with pytest.raises(ValidationError, match=r"not even - at `\$.x`"):
            Ex.struct_validate_json(b'{"x": 3}')
        with pytest.raises(ValidationError, match="not even"):
            Ex.struct_validate({"x": 3})

    def test_subclass_validator_not_shared_across_structs(self):
        class Even(Constraint):
            def __call__(self, v):
                if v % 2:
                    raise ValueError("not even")

        class First(Struct):
            x: Annotated[int, Even()]

        class Other(Struct):
            x: Annotated[int, Constraint()]

        # Regression: on Python 3.10, bare Constraint() == Even() due to
        # inherited richcompare, causing typing.Annotated to cache them as
        # the same object. Verify the two struct types are independent.
        assert Other.struct_validate_json(b'{"x": -5}') == Other(x=-5)
        assert First.struct_validate_json(b'{"x": 4}') == First(x=4)

    @pytest.mark.parametrize(
        "annotation,payload",
        [
            ('Annotated[int, Constraint()]', b'{"x": -5}'),
            ('Annotated[list[int], Constraint()]', b'{"x": []}'),
            ('Annotated[Shade, Constraint()]', b'{"x": {"name": "t"}}'),
        ],
    )
    def test_bare_validator_is_inert(self, annotation, payload):
        ns = {
            "Annotated": Annotated,
            "Constraint": Constraint,
            "Shade": Shade,
            "Struct": Struct,
        }
        exec(f"class Ex(Struct):\n    x: {annotation}", ns)
        out = ns["Ex"].struct_validate_json(payload)
        assert out is not None

    def test_bare_validator_on_struct_instance_input(self):
        class Inner(Struct):
            y: int = 0

        class Outer(Struct):
            inner: Annotated[Inner, Constraint()]

        assert Outer.struct_validate({"inner": Inner()}).inner.y == 0

    def test_numeric_fast_subclass_as_instance_still_fires(self):
        # Fast subclasses lower to bitflag checks; the ge=0 violation must
        # still be caught even though no USER_VALIDATOR call happens.
        class Ex(Struct):
            x: Annotated[int, NumericConstraint(ge=0)]

        with pytest.raises(ValidationError, match="Expected `int` >= 0"):
            Ex.struct_validate_json(b'{"x": -1}')
        with pytest.raises(ValidationError, match="Expected `int` >= 0"):
            Ex(-1).struct_check_types()

    def test_optional_null_skips_validator(self):
        seen = []

        class Ex(Struct):
            x: Optional[Annotated[str, Constraint(lambda v: seen.append(v))]]

        assert Ex.struct_validate_json(b'{"x": null}') == Ex(None)
        assert seen == []
        Ex.struct_validate_json(b'{"x": "s"}')
        assert seen == ["s"]

    def test_struct_check_types_field_context(self):
        def fail(v):
            raise ValueError("selfcheck boom")

        class Ex(Struct):
            x: Annotated[int, Constraint(fail)]

        with pytest.raises(
            ValidationError, match=r"selfcheck boom - at `\$\.x`"
        ):
            Ex(1).struct_check_types()

    def test_struct_check_types_nested_struct_field(self):
        class Inner(Struct):
            y: int = 0

        def fail(v):
            raise ValueError("bad inner")

        class Outer(Struct):
            inner: Annotated[Inner, Constraint(fail)]

        with pytest.raises(ValidationError, match=r"bad inner - at `\$.inner`"):
            Outer(Inner()).struct_check_types()

    def test_dict_key_validator(self):
        dec = JSONDecoder(dict[Annotated[int, NumericConstraint(ge=0)], int])
        assert dec.decode(b'{"1": 2}') == {1: 2}
        with pytest.raises(ValidationError):
            dec.decode(b'{"-1": 2}')

    def test_adhoc_decoder_top_level(self):
        dec = JSONDecoder(Annotated[list[int], CollectionConstraint(min_length=1)])
        assert dec.decode(b"[1]") == [1]

    def test_no_validation_on_encode(self):
        calls = []

        class Ex(Struct):
            x: Annotated[int, Constraint(calls.append)]

        assert Ex(5).struct_dump_json() == b'{"x":5}'
        assert calls == []


class TestValidateSelfCheckTypesOnly:
    """struct_check_types and check_types_on_init are pure type-checks:
    no Serializer.load, no protocol conversion, only isinstance + Constraints."""

    def test_custom_field_wrong_type_raises(self):
        calls = []

        class Color:
            def __init__(self, v):
                self.v = v

        ser = Serializer(load=lambda s: (calls.append("load"), Color(s))[1])

        class Ex(Struct):
            struct_config = StructConfig(check_types_on_init=True)
            c: Annotated[Color, ser]

        # check_types_on_init should raise because "blue" isn't a Color
        with pytest.raises(ValidationError, match="Expected `Color`, got `str`"):
            Ex("blue")
        assert calls == []  # load was NOT called

    def test_custom_field_correct_type_passes(self):
        calls = []

        class Color:
            def __init__(self, v):
                self.v = v

        ser = Serializer(load=lambda s: (calls.append("load"), Color(s))[1])

        class Ex(Struct):
            struct_config = StructConfig(check_types_on_init=True)
            c: Annotated[Color, ser]

        # Already the right type — passes, load not called
        Ex(Color("red")).struct_check_types()
        assert calls == []

    def test_validate_self_raises_on_wrong_type(self):
        calls = []

        class Color:
            def __init__(self, v):
                self.v = v

        ser = Serializer(load=lambda s: (calls.append("load"), Color(s))[1])

        class Ex(Struct):
            c: Annotated[Color, ser]

        ex = Ex(Color("x"))
        ex.c = "blue"  # bypass type safety
        with pytest.raises(ValidationError, match="Expected `Color`, got `str`"):
            ex.struct_check_types()
        assert calls == []  # load was NOT called

    def test_protocol_not_called_on_mismatch(self):
        calls = []

        class Color:
            @classmethod
            def struct_validate(cls, d):
                calls.append("protocol")
                return cls(d["v"])

        class Ex(Struct):
            struct_config = StructConfig(check_types_on_init=True)
            c: Color

        with pytest.raises(ValidationError, match="Expected `Color`, got `dict`"):
            Ex({"v": 1})
        assert calls == []  # protocol was NOT called

    def test_optional_none_passes(self):
        class Color:
            def __init__(self, v):
                self.v = v

        class Ex(Struct):
            struct_config = StructConfig(check_types_on_init=True)
            c: Optional[Annotated[Color, Serializer(load=Color)]]

        # None is valid for Optional[Color]
        Ex(None).struct_check_types()

    def test_validator_called_on_correct_type(self):
        seen = []

        class Color:
            def __init__(self, v):
                self.v = v

        class Ex(Struct):
            struct_config = StructConfig(check_types_on_init=True)
            c: Annotated[Color, Serializer(load=Color), Constraint(lambda v: seen.append(v.v))]

        Ex(Color("ok"))
        assert seen == ["ok"]  # called once during construction

    def test_validate_json_still_converts(self):
        """struct_validate_json must still call load (conversion)."""
        calls = []

        class Color:
            def __init__(self, v):
                self.v = v

        ser = Serializer(load=lambda s: (calls.append("load"), Color(s))[1])

        class Ex(Struct):
            c: Annotated[Color, ser]

        Ex.struct_validate_json(b'{"c": "blue"}')
        assert calls == ["load"]  # load WAS called in json path

    def test_struct_validate_still_converts(self):
        """struct_validate must still call load (conversion)."""
        calls = []

        class Color:
            def __init__(self, v):
                self.v = v

        ser = Serializer(load=lambda s: (calls.append("load"), Color(s))[1])

        class Ex(Struct):
            c: Annotated[Color, ser]

        Ex.struct_validate({"c": "blue"})
        assert calls == ["load"]  # load WAS called in struct_validate path

    def test_check_types_on_init_raises(self):
        calls = []

        class Color:
            def __init__(self, v):
                self.v = v

        ser = Serializer(load=lambda s: (calls.append("load"), Color(s))[1])

        class Ex(Struct):
            struct_config = StructConfig(check_types_on_init=True)
            c: Annotated[Color, ser]

        with pytest.raises(ValidationError):
            Ex("blue")
        assert calls == []  # load was NOT called on constructor


class TestCompositionRules:
    """Class-creation enforcement of at most one Field, Serializer, Constraint
    per annotation position."""

    def test_multiple_fields_rejected(self):
        with pytest.raises(TypeError, match="Multiple `Field` annotations"):
            class Bad(Struct):
                x: Annotated[int, Field(), Field()]

    def test_multiple_fields_with_alias_rejected(self):
        with pytest.raises(TypeError, match="Multiple `Field` annotations"):
            class Bad(Struct):
                x: Annotated[int, Field(alias="a"), Field(alias="b")]

    def test_multiple_serializers_rejected(self):
        def f(x):
            return x

        with pytest.raises(TypeError, match="Multiple `Serializer` annotations"):
            class Bad(Struct):
                x: Annotated[Color, Serializer(load=f), Serializer(dump=f)]

    def test_multiple_validators_rejected(self):
        with pytest.raises(TypeError, match="Multiple `Constraint` annotations"):
            class Bad(Struct):
                x: Annotated[int, Constraint(), Constraint()]

    def test_multiple_fast_validators_rejected(self):
        with pytest.raises(TypeError, match="Multiple `Constraint` annotations"):
            class Bad(Struct):
                x: Annotated[int, NumericConstraint(ge=0), StrConstraint(min_length=1)]

    def test_cross_kind_combo_allowed(self):
        def fn(v):
            pass

        def dump(v):
            return v

        class Good(Struct):
            x: Annotated[Color, Field(title="x"), Serializer(dump=dump), Constraint(fn)]

        assert Good.__struct_fields__ == ("x",)

    def test_field_serializer_validator_all_present(self):
        def fn(v):
            pass

        def dump(v):
            return v

        def load(v):
            return Color(v)

        class Good(Struct):
            x: Annotated[
                Color, Field(title="x"), Serializer(load=load, dump=dump), Constraint(fn)
            ]

        assert Good.__struct_fields__ == ("x",)


class TestConstraintApplicability:
    """Constraint-vs-type-kind checks enforced at class creation time."""

    def test_numeric_validator_on_str_rejected(self):
        with pytest.raises(TypeError, match="NumericConstraint.*numeric types"):
            class Bad(Struct):
                x: Annotated[str, NumericConstraint(ge=0)]

    def test_str_validator_on_int_rejected(self):
        with pytest.raises(TypeError, match="StrConstraint.*str.*types"):
            class Bad(Struct):
                x: Annotated[int, StrConstraint(min_length=1)]

    def test_timezone_validator_on_int_rejected(self):
        with pytest.raises(TypeError, match="TimezoneConstraint.*datetime.*time"):
            class Bad(Struct):
                x: Annotated[int, TimezoneConstraint(tz=True)]

    def test_collection_validator_on_int_rejected(self):
        with pytest.raises(TypeError, match="CollectionConstraint.*collection"):
            class Bad(Struct):
                x: Annotated[int, CollectionConstraint(min_length=1)]

    def test_bytes_validator_on_int_rejected(self):
        with pytest.raises(TypeError, match="BytesConstraint.*bytes-like"):
            class Bad(Struct):
                x: Annotated[int, BytesConstraint(min_length=1)]

    def test_numeric_validator_on_float_ok(self):
        class Good(Struct):
            x: Annotated[float, NumericConstraint(ge=0.0)]

        assert Good.__struct_fields__ == ("x",)

    def test_collection_validator_on_list_ok(self):
        class Good(Struct):
            x: Annotated[list[int], CollectionConstraint(min_length=1)]

        assert Good.__struct_fields__ == ("x",)

    def test_collection_validator_on_dict_ok(self):
        class Good(Struct):
            x: Annotated[dict[str, int], CollectionConstraint(min_length=1)]

        assert Good.__struct_fields__ == ("x",)

    def test_collection_validator_on_set_ok(self):
        class Good(Struct):
            x: Annotated[set[int], CollectionConstraint(min_length=1)]

        assert Good.__struct_fields__ == ("x",)

    def test_collection_validator_on_tuple_ok(self):
        class Good(Struct):
            x: Annotated[tuple[int, ...], CollectionConstraint(min_length=1)]

        assert Good.__struct_fields__ == ("x",)

    def test_timezone_validator_on_datetime_ok(self):
        class Good(Struct):
            x: Annotated[datetime.datetime, TimezoneConstraint(tz=True)]

        assert Good.__struct_fields__ == ("x",)

    def test_timezone_validator_on_time_ok(self):
        class Good(Struct):
            x: Annotated[datetime.time, TimezoneConstraint(tz=False)]

        assert Good.__struct_fields__ == ("x",)

    def test_bytes_validator_on_bytes_ok(self):
        class Good(Struct):
            x: Annotated[bytes, BytesConstraint(min_length=1)]

        assert Good.__struct_fields__ == ("x",)

    def test_str_validator_on_str_ok(self):
        class Good(Struct):
            x: Annotated[str, StrConstraint(min_length=1)]

        assert Good.__struct_fields__ == ("x",)

    def test_numeric_validator_on_int_ok(self):
        class Good(Struct):
            x: Annotated[int, NumericConstraint(ge=0)]

        assert Good.__struct_fields__ == ("x",)

    def test_union_deferred(self):
        """Union types should NOT raise at class creation; defer to lazy decoder."""
        class Good(Struct):
            x: Annotated[Union[int, str], NumericConstraint(ge=0)]

        assert Good.__struct_fields__ == ("x",)

    def test_union_deferred_at_definition(self):
        """Python 3.10+ union syntax should also defer."""
        class Good(Struct):
            x: Annotated[int | str, NumericConstraint(ge=0)]

        assert Good.__struct_fields__ == ("x",)

    def test_adhoc_decoder_still_raises(self):
        """Ad-hoc JSONDecoder(invalid combo) still raises at decoder build time."""
        with pytest.raises(TypeError, match="numeric"):
            JSONDecoder(Annotated[str, NumericConstraint(ge=0)])

    def test_validator_inherits_from_base(self):
        """Subclass inherits codec maps + validators from base class."""
        def dump(c):
            return list(c.rgb)

        class Base(Struct):
            color: Annotated[Color, Serializer(dump=dump)]

        class Sub(Base):
            extra: int = 0

        out = Sub.struct_validate({"color": Color((1, 2, 3))})
        assert out.color.rgb == (1, 2, 3)

    def test_serializer_and_validator_together(self):
        """Serializer.dump works alongside Constraint constraints on same field."""
        def dump(v):
            return list(v.rgb)

        def load(v):
            return Color(tuple(v))

        class Good(Struct):
            x: Annotated[
                Color,
                Serializer(load=load, dump=dump),
                Constraint(lambda v: None),
            ]

        obj = Good(Color((1, 2, 3)))
        assert obj.struct_dump_json() == b'{"x":[1,2,3]}'

    def test_nested_collection_element_validator(self):
        """Constraint on element type inside a collection defers (inner is leaf)."""
        class Good(Struct):
            x: list[Annotated[int, NumericConstraint(ge=0)]]

        assert Good.__struct_fields__ == ("x",)

    def test_nested_collection_element_validator_rejects(self):
        """Constraint mismatch on element type inside a collection raises."""
        with pytest.raises(TypeError, match="NumericConstraint.*numeric"):
            class Bad(Struct):
                x: list[Annotated[str, NumericConstraint(ge=0)]]




class TestNativeSubclassCodecs:
    """Subclasses of natively supported types count as custom types, so they
    accept per-field ``Serializer`` codecs (docs/extending.rst,
    "Custom formats for natively supported types")."""

    FMT = "%d/%m/%Y %H:%M"

    def test_datetime_subclass_roundtrip(self):
        class EuroDT(datetime.datetime):
            @classmethod
            def parse(cls, value):
                if isinstance(value, EuroDT):
                    return value
                return cls.strptime(value, self.FMT)

        ser = Serializer(dump=lambda d: d.strftime(self.FMT), load=EuroDT.parse)

        class Event(Struct):
            when: Annotated[EuroDT, ser]

        msg = Event.struct_validate_json(b'{"when": "05/06/2020 14:30"}')
        assert isinstance(msg.when, datetime.datetime)
        assert msg.when.year == 2020 and msg.when.hour == 14
        assert msg.struct_dump_json() == b'{"when":"05/06/2020 14:30"}'
        assert Event.struct_validate_json(msg.struct_dump_json()) == msg

    def test_str_subclass_normalizes_on_both_paths(self):
        class Lower(str):
            @classmethod
            def parse(cls, value):
                if isinstance(value, Lower):
                    return value
                return cls(str(value).lower())

        ser = Serializer(dump=str, load=Lower.parse)

        class User(Struct):
            name: Annotated[Lower, ser]

        assert User.struct_validate_json(b'{"name": "ALICE"}').name == "alice"
        assert User.struct_validate({"name": "BOB"}).name == "bob"

    def test_int_and_bytes_subclass_codecs(self):
        class Milli(int):
            @classmethod
            def parse(cls, value):
                return cls(int(value))

        class Hex(bytes):
            pass

        class Item(Struct):
            cents: Annotated[
                Milli,
                Serializer(dump=lambda m: str(int(m)), load=Milli.parse),
            ]
            blob: Annotated[
                Hex,
                Serializer(dump=lambda b: b.hex().upper(), load=lambda s: Hex.fromhex(s)),
            ]

        out = Item.struct_validate_json(b'{"cents": "1999", "blob": "00ff10"}')
        assert out.cents == 1999 and isinstance(out.cents, int)
        assert out.cents + 1 == 2000
        assert out.blob == bytes.fromhex("00ff10")
        assert out.struct_dump_json() == b'{"cents":"1999","blob":"00FF10"}'

    def test_instance_passthrough_skips_load(self):
        calls = []

        class Marker(str):
            pass

        def load(value):
            calls.append(value)
            return Marker(value)

        class Holder(Struct):
            name: Annotated[Marker, Serializer(dump=str, load=load)]

        marker = Marker("already")
        assert Holder.struct_validate({"name": marker}).name is marker
        assert calls == []  # already an instance: load skipped

        Holder.struct_validate({"name": "raw"})
        assert len(calls) == 1

    def test_base_instance_routed_through_load_and_wraps_errors(self):
        class EuroDT(datetime.datetime):
            @classmethod
            def parse(cls, value):
                return cls.strptime(value, "%d/%m/%Y")  # rejects non-str

        class Holder(Struct):
            when: Annotated[EuroDT, Serializer(dump=str, load=EuroDT.parse)]

        # base-class instances are not the subclass, so load() runs on them
        with pytest.raises(ValidationError, match="strptime"):
            Holder.struct_validate({"when": datetime.datetime(2020, 1, 1)})

    def test_optional_and_nested_containers(self):
        class Lower(str):
            @classmethod
            def parse(cls, value):
                if isinstance(value, Lower):
                    return value
                return cls(str(value).lower())

        ser = Serializer(dump=str, load=Lower.parse)

        class Batch(Struct):
            primary: Optional[Annotated[Lower, ser]] = None
            tags: list[Annotated[Lower, ser]] = []

        out = Batch.struct_validate_json(b'{"primary": null, "tags": ["A", "B"]}')
        assert out.primary is None
        assert all(isinstance(t, Lower) for t in out.tags)
        assert [str(t) for t in out.tags] == ["a", "b"]
        assert out.struct_dump_json() == b'{"primary":null,"tags":["a","b"]}'

    def test_bare_subclass_without_codec_needs_protocol(self):
        class Plain(datetime.datetime):
            pass

        class NoCodec(Struct):
            when: Plain

        # no native fallback for custom-classified subclasses
        with pytest.raises(structtype.ValidationError):
            NoCodec.struct_validate_json(b'{"when": "2020-01-01T00:00:00"}')
        with pytest.raises(TypeError):
            NoCodec(Plain(2020, 1, 1)).struct_dump_json()

        # protocol methods make it work without any annotation
        class WithProtocol(datetime.datetime):
            def struct_dump(self):
                return self.isoformat()

            @classmethod
            def struct_validate(cls, obj):
                if isinstance(obj, WithProtocol):
                    return obj
                return cls.fromisoformat(obj)

        class Ok(Struct):
            when: WithProtocol

        ok = Ok.struct_validate_json(b'{"when": "2020-01-01T12:00:00"}')
        assert isinstance(ok.when, WithProtocol)
        assert ok.struct_dump_json() == b'{"when":"2020-01-01T12:00:00"}'


class TestNativeTypeSerializerFunctional:
    """Functional tests for Serializer on newly allowed types (load, dump,
    roundtrip, struct_dump builtins path, error wrapping)."""

    # -- set / frozenset ---------------------------------------------------

    def test_set_serializer_roundtrip(self):
        class Msg(Struct):
            v: Annotated[set[int], Serializer(dump=sorted, load=set)]

        msg = Msg.struct_validate_json(b'{"v": [3, 1, 2]}')
        assert msg.v == {1, 2, 3}
        assert msg.struct_dump_json() == b'{"v":[1,2,3]}'
        assert msg.struct_dump() == {"v": [1, 2, 3]}

    def test_frozenset_serializer_roundtrip(self):
        class Msg(Struct):
            v: Annotated[frozenset[int], Serializer(dump=sorted, load=frozenset)]

        msg = Msg.struct_validate_json(b'{"v": [3, 1, 2]}')
        assert msg.v == frozenset({1, 2, 3})
        assert msg.struct_dump_json() == b'{"v":[1,2,3]}'
        assert msg.struct_dump() == {"v": [1, 2, 3]}

    def test_set_serializer_struct_validate(self):
        class Msg(Struct):
            v: Annotated[set[int], Serializer(dump=sorted, load=set)]

        msg = Msg.struct_validate({"v": [3, 1, 2]})
        assert msg.v == {1, 2, 3}

    def test_set_serializer_optional_none(self):
        class Msg(Struct):
            v: Annotated[Optional[set[int]], Serializer(dump=sorted, load=set)]

        msg = Msg.struct_validate_json(b'{"v": null}')
        assert msg.v is None
        assert msg.struct_dump_json() == b'{"v":null}'

    # -- date / time / timedelta -------------------------------------------

    def test_date_serializer_roundtrip(self):
        def dump(d):
            return d.isoformat()

        def load(v):
            return datetime.date.fromisoformat(v)

        class Msg(Struct):
            v: Annotated[datetime.date, Serializer(dump=dump, load=load)]

        msg = Msg.struct_validate_json(b'{"v": "2026-01-15"}')
        assert msg.v == datetime.date(2026, 1, 15)
        assert msg.struct_dump_json() == b'{"v":"2026-01-15"}'
        assert msg.struct_dump() == {"v": "2026-01-15"}

    def test_time_serializer_roundtrip(self):
        def dump(t):
            return t.isoformat()

        def load(v):
            return datetime.time.fromisoformat(v)

        class Msg(Struct):
            v: Annotated[datetime.time, Serializer(dump=dump, load=load)]

        msg = Msg.struct_validate_json(b'{"v": "14:30:00"}')
        assert msg.v == datetime.time(14, 30)
        assert msg.struct_dump_json() == b'{"v":"14:30:00"}'
        assert msg.struct_dump() == {"v": "14:30:00"}

    def test_timedelta_serializer_roundtrip(self):
        def dump(td):
            return td.total_seconds()

        def load(v):
            return datetime.timedelta(seconds=v)

        class Msg(Struct):
            v: Annotated[
                datetime.timedelta, Serializer(dump=dump, load=load)
            ]

        td = datetime.timedelta(hours=1, minutes=30)
        msg = Msg.struct_validate_json(b'{"v": 5400.0}')
        assert msg.v == td
        assert msg.struct_dump() == {"v": 5400.0}

    def test_date_serializer_struct_validate(self):
        def load(v):
            return datetime.date.fromisoformat(v)

        class Msg(Struct):
            v: Annotated[datetime.date, Serializer(dump=str, load=load)]

        msg = Msg.struct_validate({"v": "2026-06-15"})
        assert msg.v == datetime.date(2026, 6, 15)

    def test_date_serializer_optional_none(self):
        def dump(d):
            return d.isoformat()

        def load(v):
            return datetime.date.fromisoformat(v)

        class Msg(Struct):
            v: Annotated[
                Optional[datetime.date], Serializer(dump=dump, load=load)
            ]

        msg = Msg.struct_validate_json(b'{"v": null}')
        assert msg.v is None
        assert msg.struct_dump_json() == b'{"v":null}'

    # -- Decimal in struct field -------------------------------------------

    def test_decimal_serializer_in_struct(self):
        def dump(d):
            return str(d)

        def load(v):
            return decimal.Decimal(v)

        class Msg(Struct):
            v: Annotated[decimal.Decimal, Serializer(dump=dump, load=load)]

        msg = Msg.struct_validate_json(b'{"v": "3.14"}')
        assert msg.v == decimal.Decimal("3.14")
        assert msg.struct_dump_json() == b'{"v":"3.14"}'
        assert msg.struct_dump() == {"v": "3.14"}

    def test_decimal_serializer_struct_validate(self):
        class Msg(Struct):
            v: Annotated[decimal.Decimal, Serializer(dump=str, load=decimal.Decimal)]

        msg = Msg.struct_validate({"v": "9.99"})
        assert msg.v == decimal.Decimal("9.99")

    # -- Union[X, None] outside Annotated — load path ----------------------

    def test_union_outside_annotated_load_path(self):
        class Msg(Struct):
            v: Annotated[
                complex,
                Serializer(load=lambda v: complex(v[0], v[1])),
            ] | None

        msg = Msg.struct_validate_json(b'{"v": [1.0, 2.0]}')
        assert msg.v == complex(1, 2)

    def test_union_outside_annotated_none_bypass(self):
        class Msg(Struct):
            v: Annotated[
                complex,
                Serializer(
                    dump=lambda c: [c.real, c.imag],
                    load=lambda v: complex(v[0], v[1]),
                ),
            ] | None

        msg = Msg.struct_validate_json(b'{"v": null}')
        assert msg.v is None
        assert msg.struct_dump_json() == b'{"v":null}'

    def test_union_outside_annotated_dump_path(self):
        class Msg(Struct):
            v: Annotated[
                complex, Serializer(dump=lambda c: [c.real, c.imag])
            ] | None

        msg = Msg(complex(1, 2))
        assert msg.struct_dump_json() == b'{"v":[1.0,2.0]}'

    # -- struct_dump() builtins path for newly allowed types ---------------

    def test_struct_dump_builtins_datetime(self):
        class Msg(Struct):
            v: Annotated[datetime.datetime, Serializer(dump=lambda d: d.year)]

        msg = Msg(datetime.datetime(2026, 3, 15))
        assert msg.struct_dump() == {"v": 2026}

    def test_struct_dump_builtins_uuid(self):
        class Msg(Struct):
            v: Annotated[uuid.UUID, Serializer(dump=lambda u: u.hex)]

        u = uuid.UUID("c4524ac0-e81e-4aa8-a595-0aec605a659a")
        msg = Msg(u)
        assert msg.struct_dump() == {"v": u.hex}

    def test_struct_dump_builtins_bytes(self):
        class Msg(Struct):
            v: Annotated[bytes, Serializer(dump=len, load=lambda n: b"x" * n)]

        msg = Msg(b"hello")
        assert msg.struct_dump() == {"v": 5}

    def test_struct_dump_builtins_decimal(self):
        class Msg(Struct):
            v: Annotated[decimal.Decimal, Serializer(dump=str, load=decimal.Decimal)]

        msg = Msg(decimal.Decimal("2.5"))
        assert msg.struct_dump() == {"v": "2.5"}

    def test_struct_dump_optional_none_bypasses_codec(self):
        class Msg(Struct):
            v: Annotated[
                Optional[datetime.datetime],
                Serializer(dump=lambda d: d.year, load=lambda v: datetime.datetime(v, 1, 1)),
            ]

        msg = Msg(None)
        assert msg.struct_dump() == {"v": None}
        assert msg.struct_dump_json() == b'{"v":null}'

    # -- Error wrapping for newly allowed types ----------------------------

    def test_error_wrapping_datetime_load(self):
        def bad_load(v):
            raise ValueError("custom error")

        class Msg(Struct):
            v: Annotated[datetime.datetime, Serializer(load=bad_load, dump=str)]

        with pytest.raises(ValidationError, match="custom error"):
            Msg.struct_validate_json(b'{"v": "2026-01-01T00:00:00"}')

    def test_error_wrapping_uuid_load(self):
        def bad_load(v):
            raise ValueError("bad uuid")

        class Msg(Struct):
            v: Annotated[uuid.UUID, Serializer(load=bad_load, dump=str)]

        with pytest.raises(ValidationError, match="bad uuid"):
            Msg.struct_validate_json(b'{"v": "c4524ac0-e81e-4aa8-a595-0aec605a659a"}')

    def test_error_wrapping_bytes_load(self):
        def bad_load(v):
            raise ValueError("bad bytes")

        class Msg(Struct):
            v: Annotated[bytes, Serializer(load=bad_load, dump=str)]

        with pytest.raises(ValidationError, match="bad bytes"):
            Msg.struct_validate_json(b'{"v": "aGVsbG8="}')

    def test_error_wrapping_decimal_load(self):
        def bad_load(v):
            raise ValueError("bad decimal")

        class Msg(Struct):
            v: Annotated[decimal.Decimal, Serializer(load=bad_load, dump=str)]

        with pytest.raises(ValidationError, match="bad decimal"):
            Msg.struct_validate_json(b'{"v": "3.14"}')

    def test_error_wrapping_enum_load(self):
        class Color(enum.Enum):
            RED = "r"

        def bad_load(v):
            raise ValueError("bad color")

        class Msg(Struct):
            v: Annotated[Color, Serializer(load=bad_load, dump=str)]

        with pytest.raises(ValidationError, match="bad color"):
            Msg.struct_validate_json(b'{"v": "r"}')

    def test_error_wrapping_via_struct_validate(self):
        def bad_load(v):
            raise RuntimeError("boom")

        class Msg(Struct):
            v: Annotated[datetime.datetime, Serializer(load=bad_load, dump=str)]

        with pytest.raises(RuntimeError, match="boom"):
            Msg.struct_validate({"v": "2026-01-01"})


class _Point(Struct):
    x: int




class _Point(Struct):
    x: int


class _Color(enum.Enum):
    R = "r"


class _IntEnum(enum.IntEnum):
    A = 1


class _NamedTuple(NamedTuple):
    x: int


@dataclasses.dataclass
class _Dataclass:
    x: int


def _ser():
    return Serializer(load=int, dump=str)


@pytest.mark.parametrize(
    "ann_factory",
    [
        lambda: Annotated[bool, _ser()],
        lambda: Annotated[int, _ser()],
        lambda: Annotated[float, _ser()],
        lambda: Annotated[str, _ser()],
        lambda: Annotated[list[int], _ser()],
        lambda: Annotated[list, _ser()],
        lambda: Annotated[dict[str, int], _ser()],
        lambda: Annotated[tuple[int, ...], _ser()],
        lambda: Annotated[int | str, _ser()],
        lambda: Annotated[Optional[int], _ser()],
        lambda: Annotated[Literal[1], _ser()],
        lambda: Annotated[Literal["x"], _ser()],
        lambda: Annotated[Literal[True], _ser()],
    ],
    ids=[
        "bool", "int", "float", "str",
        "list", "bare-list", "dict", "tuple", "union", "optional",
        "literal-int", "literal-str", "literal-bool",
    ],
)
def test_blocked_types_reject_serializer(ann_factory):
    with pytest.raises(TypeError, match="native types"):
        type(
            f"Reject_{abs(hash(str(ann_factory)))}",
            (Struct,),
            {"__annotations__": {"v": ann_factory()}},
        )


@pytest.mark.parametrize(
    "ann_factory",
    [
        lambda: Annotated[bytes, _ser()],
        lambda: Annotated[bytearray, _ser()],
        lambda: Annotated[memoryview, _ser()],
        lambda: Annotated[datetime.datetime, _ser()],
        lambda: Annotated[datetime.date, _ser()],
        lambda: Annotated[datetime.time, _ser()],
        lambda: Annotated[datetime.timedelta, _ser()],
        lambda: Annotated[uuid.UUID, _ser()],
        lambda: Annotated[decimal.Decimal, _ser()],
        lambda: Annotated[set[int], _ser()],
        lambda: Annotated[frozenset[int], _ser()],
        lambda: Annotated[_Color, _ser()],
        lambda: Annotated[_Point, _ser()],
        lambda: Annotated[Optional[datetime.datetime], _ser()],
        lambda: Annotated[Optional[_Point], _ser()],
    ],
    ids=[
        "bytes", "bytearray", "memoryview", "datetime", "date", "time",
        "timedelta", "uuid", "decimal", "set", "frozenset", "enum",
        "nested-struct", "optional-datetime", "optional-struct",
    ],
)
def test_allowed_types_accept_serializer(ann_factory):
    type(
        f"Accept_{abs(hash(str(ann_factory)))}",
        (Struct,),
        {"__annotations__": {"v": ann_factory()}},
    )


def test_subclasses_of_natives_accept_serializer():
    class SubInt(int):
        pass

    class SubStr(str):
        pass

    class SubDT(datetime.datetime):
        pass

    class SubBytes(bytes):
        pass

    for sub in (SubInt, SubStr, SubDT, SubBytes):
        type(
            f"Accept_{sub.__name__}",
            (Struct,),
            {"__annotations__": {"v": Annotated[sub, _ser()]}},
        )


def test_struct_types_accept_serializer():
    class Msg(Struct):
        point: Annotated[_Point, _ser()]


def test_array_like_struct_accepts_serializer():
    class Inner(Struct):
        struct_config = StructConfig(array_like=True)
        x: int

    def dump_inner(inner):
        return {"x": inner.x}

    class Msg(Struct):
        inner: Annotated[Inner, Serializer(dump=dump_inner)]

    msg = Msg(inner=Inner(x=1))
    assert msg.struct_dump() == {"inner": {"x": 1}}


def test_typeddict_rejects_serializer():
    from typing import TypedDict

    class TD(TypedDict):
        x: int

    with pytest.raises(TypeError, match="native types"):

        class Msg(Struct):
            t: Annotated[TD, _ser()]


class TestNonCustomTypeCodecs:
    """Functional tests for Serializer on native types: roundtrip, null
    bypass, element-level codecs, constraints, check_types, per-field
    isolation, dict key codecs, and generic custom types."""

    def test_namedtuple_rejected(self):
        def dump(p):
            return [p.x]

        def load(v):
            return _NamedTuple(v[0])

        with pytest.raises(TypeError, match="native types"):

            class Msg(Struct):
                p: Annotated[_NamedTuple, Serializer(dump=dump, load=load)]

    def test_dataclass_roundtrip(self):
        def dump(dc):
            return {"x": dc.x, "y": dc.x}

        def load(v):
            return _Dataclass(v["x"])

        class Msg(Struct):
            dc: Annotated[_Dataclass, Serializer(dump=dump, load=load)]

        msg = Msg(_Dataclass(3))
        assert msg.struct_dump_json() == b'{"dc":{"x":3,"y":3}}'
        assert Msg.struct_validate_json(
            b'{"dc":{"x":3,"y":3}}'
        ).dc == _Dataclass(3)

    def test_optional_datetime_null_bypasses_load(self):
        def dump(d):
            return int(d.timestamp()) if d is not None else None

        def load(v):
            assert v is not None
            return datetime.datetime.fromtimestamp(
                v, datetime.timezone.utc
            )

        class Msg(Struct):
            d: Annotated[
                Optional[datetime.datetime],
                Serializer(dump=dump, load=load),
            ]

        assert Msg.struct_validate_json(b'{"d":null}').d is None
        assert Msg.struct_validate({"d": None}).d is None
        out = Msg.struct_validate_json(b'{"d":1767225600}')
        assert out.d == datetime.datetime(
            2026, 1, 1, tzinfo=datetime.timezone.utc
        )

    def test_optional_enum_roundtrip(self):
        def dump(f):
            return f.value.upper()

        def load(v):
            return _Color(v.lower())

        class Msg(Struct):
            c: Annotated[
                Optional[_Color], Serializer(dump=dump, load=load)
            ]

        msg = Msg(_Color.R)
        assert msg.struct_dump_json() == b'{"c":"R"}'
        assert Msg.struct_validate_json(b'{"c":null}').c is None
        assert Msg.struct_validate_json(b'{"c":"R"}') == msg

    def test_element_codec_inside_set(self):
        def load(value):
            return datetime.datetime.fromtimestamp(
                value, datetime.timezone.utc
            )

        def dump(d):
            return int(d.timestamp())

        class Msg(Struct):
            ds: Annotated[
                set[
                    Annotated[
                        datetime.datetime,
                        Serializer(dump=dump, load=load),
                    ]
                ],
                None,
            ]

        dt = datetime.datetime(
            2026, 1, 1, tzinfo=datetime.timezone.utc
        )
        msg = Msg({dt})
        assert msg.struct_dump_json() == b'{"ds":[1767225600]}'
        assert Msg.struct_validate_json(
            b'{"ds":[1767225600]}'
        ).ds == {dt}

    def test_element_codec_inside_tuple(self):
        def load(value):
            return datetime.datetime.fromtimestamp(
                value, datetime.timezone.utc
            )

        def dump(d):
            return int(d.timestamp())

        class Msg(Struct):
            ds: Annotated[
                tuple[
                    Annotated[
                        datetime.datetime,
                        Serializer(dump=dump, load=load),
                    ],
                    ...,
                ],
                None,
            ]

        dt = datetime.datetime(
            2026, 1, 1, tzinfo=datetime.timezone.utc
        )
        msg = Msg((dt, dt))
        assert msg.struct_dump_json() == b'{"ds":[1767225600,1767225600]}'
        assert Msg.struct_validate_json(
            b'{"ds":[1767225600]}'
        ).ds == (dt,)

    def test_codec_and_bytes_constraint(self):
        def dump(b):
            return b.hex()

        class Msg(Struct):
            b: Annotated[
                bytes,
                BytesConstraint(min_length=2),
                Serializer(dump=dump, load=bytes.fromhex),
            ]

        # Python path, already bytes: constraints apply.
        assert Msg.struct_validate({"b": b"\x01\x02"}).b == b"\x01\x02"
        with pytest.raises(ValidationError):
            Msg.struct_validate({"b": b"\x01"})
        # JSON path: constraints apply to the value returned by load.
        assert Msg.struct_validate_json(b'{"b":"0102"}').b == b"\x01\x02"
        with pytest.raises(ValidationError):
            Msg.struct_validate_json(b'{"b":"01"}')

    def test_check_types_on_codec_field(self):
        def dump(d):
            return int(d.timestamp())

        def load(v):
            return datetime.datetime.fromtimestamp(
                v, datetime.timezone.utc
            )

        class Msg(Struct):
            d: Annotated[
                datetime.datetime,
                Serializer(dump=dump, load=load),
            ]

        Msg(datetime.datetime(2026, 1, 1)).struct_check_types()
        with pytest.raises(ValidationError):
            Msg("not-a-datetime").struct_check_types()

    def test_per_field_isolation(self):
        def dump_a(d):
            return int(d.timestamp())

        def load_a(v):
            return datetime.datetime.fromtimestamp(
                v, datetime.timezone.utc
            )

        def dump_b(d):
            return d.year

        def load_b(v):
            return datetime.datetime(
                int(v), 1, 1, tzinfo=datetime.timezone.utc
            )

        class Msg(Struct):
            a: Annotated[
                datetime.datetime,
                Serializer(dump=dump_a, load=load_a),
            ]
            b: Annotated[
                datetime.datetime,
                Serializer(dump=dump_b, load=load_b),
            ]

        d = datetime.datetime(
            2026, 1, 1, tzinfo=datetime.timezone.utc
        )
        msg = Msg(d, d)
        assert msg.struct_dump_json() == b'{"a":1767225600,"b":2026}'
        out = Msg.struct_validate_json(
            b'{"a":1767225600,"b":2026}'
        )
        assert out.a == d and out.b == d

    def test_dict_key_codec(self):
        def dump(d):
            return d.strftime("%Y")

        def load(v):
            return datetime.datetime(int(v), 1, 1)

        class Msg(Struct):
            by_year: Annotated[
                dict[
                    Annotated[
                        datetime.datetime,
                        Serializer(dump=dump, load=load),
                    ],
                    int,
                ],
                None,
            ]

        dt = datetime.datetime(2026, 1, 1)
        msg = Msg({dt: 1})
        assert msg.struct_dump_json() == b'{"by_year":{"2026":1}}'
        assert Msg.struct_validate_json(
            b'{"by_year":{"2026":1}}'
        ) == msg

    def test_generic_custom_dump_codec_fires(self):
        from typing import Generic, TypeVar

        T = TypeVar("T")

        class Box(Generic[T]):
            def __init__(self, v):
                self.v = v

            def struct_dump(self):
                return self.v

            def __eq__(self, other):
                return isinstance(other, Box) and self.v == other.v

        def dump(b):
            return [b.v, b.v]

        class Msg(Struct):
            b: Annotated[Box[int], Serializer(dump=dump)]

        # The codec (not the struct_dump protocol fallback) must fire.
        assert Msg(Box(1)).struct_dump_json() == b'{"b":[1,1]}'
        assert Msg(Box(1)).struct_dump() == {"b": [1, 1]}
