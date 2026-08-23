import base64
import datetime
import re
import sys
from typing import Annotated, Optional, Union

import pytest

from structtype import (
    BytesValidator,
    CollectionValidator,
    Field,
    NumericValidator,
    Serializer,
    StrValidator,
    Struct,
    StructAdapter,
    StructConfig,
    TimezoneValidator,
    ValidationError,
    Validator,
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


class TestValidator:
    def test_construction(self):
        v = Validator(f)
        assert v.fn is f

    def test_keyword_construction(self):
        v = Validator(fn=g)
        assert v.fn is g

    def test_fn_optional(self):
        v = Validator()
        assert v.fn is None
        assert repr(v) == "structtype.Validator()"

    def test_explicit_none_fn(self):
        v = Validator(None)
        assert v.fn is None

    @pytest.mark.parametrize("fn", [1, "x", [f]])
    def test_not_callable(self, fn):
        with pytest.raises(TypeError, match="fn must be callable"):
            Validator(fn)


class TestValidatorCall:
    def test_bare_validator_call_is_noop(self):
        assert Validator()(1) is None
        assert Validator()("anything") is None

    def test_fn_invoked_with_value(self):
        seen = []

        def record(v):
            seen.append(v)
            return "return value ignored"

        v = Validator(record)
        assert v(42) is None
        assert seen == [42]

    def test_fn_exception_propagates(self):
        def boom(v):
            raise RuntimeError("boom")

        with pytest.raises(RuntimeError, match="boom"):
            Validator(boom)(1)

    def test_requires_exactly_one_arg(self):
        v = Validator(f)
        with pytest.raises(TypeError):
            v()
        with pytest.raises(TypeError):
            v(1, 2)

    def test_user_subclass_overrides_call(self):
        class Even(Validator):
            def __call__(self, value):
                if value % 2:
                    raise ValueError("not even")

        e = Even()
        assert isinstance(e, Validator)
        assert e.fn is None
        assert e(4) is None
        with pytest.raises(ValueError, match="not even"):
            e(3)

    def test_user_subclass_custom_init(self):
        class Positive(Validator):
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
            NumericValidator(gt=0),
            StrValidator(pattern="a"),
            BytesValidator(min_length=1),
            CollectionValidator(max_length=2),
            TimezoneValidator(tz=True),
        ],
    )
    def test_subclass_instances_are_validators(self, v):
        assert isinstance(v, Validator)
        assert issubclass(type(v), Validator)

    @pytest.mark.parametrize(
        "cls",
        [
            NumericValidator,
            StrValidator,
            BytesValidator,
            CollectionValidator,
            TimezoneValidator,
        ],
    )
    def test_subclasses_of_validator(self, cls):
        assert issubclass(cls, Validator)
        # fast validators are leaf types
        with pytest.raises(TypeError):
            type("Sub", (cls,), {})


class TestNumericValidator:
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
        v = NumericValidator(**kwargs)
        for k, expected in kwargs.items():
            assert getattr(v, k) == expected
        for unset in set(kwargs) ^ {"gt", "ge", "lt", "le", "multiple_of"}:
            assert getattr(v, unset) is None

    def test_none_is_equivalent_to_unset(self):
        v = NumericValidator(gt=None, multiple_of=None)
        assert v.gt is None
        assert v.multiple_of is None
        assert v == NumericValidator()

    def test_gt_ge_mutual_exclusion(self):
        with pytest.raises(ValueError, match="both `gt` and `ge`"):
            NumericValidator(gt=0, ge=1)

    def test_lt_le_mutual_exclusion(self):
        with pytest.raises(ValueError, match="both `lt` and `le`"):
            NumericValidator(lt=0, le=1)

    @pytest.mark.parametrize("name", ["gt", "ge", "lt", "le", "multiple_of"])
    @pytest.mark.parametrize("val", [float("nan"), float("inf"), float("-inf")])
    def test_non_finite_rejected(self, name, val):
        with pytest.raises(ValueError, match="must be finite"):
            NumericValidator(**{name: val})

    @pytest.mark.parametrize("name", ["gt", "ge", "lt", "le", "multiple_of"])
    @pytest.mark.parametrize("val", ["1", True, [1]])
    def test_non_numeric_rejected(self, name, val):
        with pytest.raises(TypeError, match="must be an int or float"):
            NumericValidator(**{name: val})

    @pytest.mark.parametrize("val", [0, -1, -1.5])
    def test_multiple_of_must_be_positive(self, val):
        with pytest.raises(ValueError, match="`multiple_of` must be > 0"):
            NumericValidator(multiple_of=val)

    def test_huge_int_bound_rejected_cleanly(self):
        with pytest.raises(OverflowError):
            NumericValidator(gt=10**400)

    def test_base_fn_slot_not_aliased(self):
        v = NumericValidator(gt=5)
        assert v.fn is None


class TestNumericValidatorCall:
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
        assert NumericValidator(**kwargs)(value) is None

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
        v = NumericValidator(**kwargs)
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
            NumericValidator(**kwargs)(value)

    def test_mixed_int_float_bounds(self):
        # int value vs float bound and vice versa go through float comparison
        assert NumericValidator(ge=0.5)(1) is None
        with pytest.raises(ValueError, match="Expected `int` >= 0\\.5"):
            NumericValidator(ge=0.5)(0)

    def test_wrong_type_raises_type_error(self):
        v = NumericValidator(ge=0)
        for value in ["1", None, [1], True]:
            with pytest.raises(TypeError, match="Expected `int` or `float`"):
                v(value)


class TestStrValidator:
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
        v = StrValidator(**kwargs)
        for k, expected in kwargs.items():
            assert getattr(v, k) == expected
        for unset in set(kwargs) ^ {"pattern", "min_length", "max_length"}:
            assert getattr(v, unset) is None

    def test_pattern_must_be_str(self):
        with pytest.raises(TypeError, match="`pattern` must be a str"):
            StrValidator(pattern=1)

    def test_invalid_regex_raises(self):
        with pytest.raises(re.error):
            StrValidator(pattern="(")

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    def test_negative_length_rejected(self, name):
        with pytest.raises(ValueError, match=f"`{name}` must be >= 0"):
            StrValidator(**{name: -1})

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    @pytest.mark.parametrize("val", ["1", 1.5, True])
    def test_non_int_length_rejected(self, name, val):
        with pytest.raises(TypeError, match=f"`{name}` must be an int"):
            StrValidator(**{name: val})

    def test_none_is_equivalent_to_unset(self):
        v = StrValidator(pattern=None, max_length=None)
        assert v.pattern is None
        assert v.max_length is None
        assert v == StrValidator()


class TestStrValidatorCall:
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
        assert StrValidator(**kwargs)(value) is None

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
            StrValidator(**kwargs)(value)

    @pytest.mark.parametrize("value", [1, b"abc", None, ["a"]])
    def test_wrong_type_raises_type_error(self, value):
        with pytest.raises(TypeError, match="Expected `str`"):
            StrValidator(pattern="a")(value)


class TestBytesValidator:
    @pytest.mark.parametrize(
        "kwargs",
        [{}, {"min_length": 0}, {"max_length": 4}, {"min_length": 1, "max_length": 9}],
    )
    def test_valid_construction(self, kwargs):
        v = BytesValidator(**kwargs)
        for k, expected in kwargs.items():
            assert getattr(v, k) == expected
        for unset in set(kwargs) ^ {"min_length", "max_length"}:
            assert getattr(v, unset) is None

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    def test_negative_length_rejected(self, name):
        with pytest.raises(ValueError, match=f"`{name}` must be >= 0"):
            BytesValidator(**{name: -1})

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    def test_non_int_length_rejected(self, name):
        with pytest.raises(TypeError, match=f"`{name}` must be an int"):
            BytesValidator(**{name: "x"})


class TestBytesValidatorCall:
    @pytest.mark.parametrize("value", [b"ab", bytearray(b"abc"), memoryview(b"abcd")])
    def test_passes(self, value):
        assert BytesValidator(min_length=2)(value) is None

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
            BytesValidator(**kwargs)(value)

    @pytest.mark.parametrize("value", ["abc", 123, None])
    def test_wrong_type_raises_type_error(self, value):
        with pytest.raises(TypeError, match="Expected `bytes`"):
            BytesValidator(min_length=1)(value)


class TestCollectionValidator:
    @pytest.mark.parametrize(
        "kwargs",
        [{}, {"min_length": 2}, {"max_length": 7}, {"min_length": 1, "max_length": 3}],
    )
    def test_valid_construction(self, kwargs):
        v = CollectionValidator(**kwargs)
        for k, expected in kwargs.items():
            assert getattr(v, k) == expected
        for unset in set(kwargs) ^ {"min_length", "max_length"}:
            assert getattr(v, unset) is None

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    def test_negative_length_rejected(self, name):
        with pytest.raises(ValueError, match=f"`{name}` must be >= 0"):
            CollectionValidator(**{name: -1})

    @pytest.mark.parametrize("name", ["min_length", "max_length"])
    def test_non_int_length_rejected(self, name):
        with pytest.raises(TypeError, match=f"`{name}` must be an int"):
            CollectionValidator(**{name: object()})


class TestCollectionValidatorCall:
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
        assert CollectionValidator(min_length=2)(value) is None

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
            CollectionValidator(**kwargs)(value)

    @pytest.mark.parametrize("value", ["abc", b"abc", 123])
    def test_wrong_type_raises_type_error(self, value):
        with pytest.raises(TypeError, match="Expected `list`"):
            CollectionValidator(min_length=1)(value)


class TestTimezoneValidatorCall:
    AWARE_DT = datetime.datetime(2020, 1, 1, tzinfo=datetime.timezone.utc)
    NAIVE_DT = datetime.datetime(2020, 1, 1)
    AWARE_TIME = datetime.time(12, 0, tzinfo=datetime.timezone.utc)
    NAIVE_TIME = datetime.time(12, 0)

    def test_aware_required_passes(self):
        v = TimezoneValidator(tz=True)
        assert v(self.AWARE_DT) is None
        assert v(self.AWARE_TIME) is None

    def test_naive_required_passes(self):
        v = TimezoneValidator(tz=False)
        assert v(self.NAIVE_DT) is None
        assert v(self.NAIVE_TIME) is None

    @pytest.mark.parametrize("value_name", ["NAIVE_DT", "NAIVE_TIME"])
    def test_aware_required_fails(self, value_name):
        with pytest.raises(ValueError, match="with a timezone component"):
            TimezoneValidator(tz=True)(getattr(self, value_name))

    @pytest.mark.parametrize("value_name", ["AWARE_DT", "AWARE_TIME"])
    def test_naive_required_fails(self, value_name):
        with pytest.raises(ValueError, match="with no timezone component"):
            TimezoneValidator(tz=False)(getattr(self, value_name))

    @pytest.mark.parametrize(
        "value", ["2020-01-01", 0, None, datetime.date(2020, 1, 1)]
    )
    def test_wrong_type_raises_type_error(self, value):
        with pytest.raises(TypeError, match="Expected `datetime` or `time`"):
            TimezoneValidator(tz=True)(value)


class TestTimezoneValidator:
    @pytest.mark.parametrize("tz", [True, False])
    def test_keyword_construction(self, tz):
        v = TimezoneValidator(tz=tz)
        assert v.tz is tz

    @pytest.mark.parametrize("tz", [True, False])
    def test_positional_construction(self, tz):
        v = TimezoneValidator(tz)
        assert v.tz is tz

    def test_required(self):
        with pytest.raises(TypeError):
            TimezoneValidator()

    @pytest.mark.parametrize("tz", ["yes", 1, None, []])
    def test_non_bool_rejected(self, tz):
        with pytest.raises(TypeError, match="`tz` must be a bool"):
            TimezoneValidator(tz=tz)


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
        assert Validator(f) == Validator(f)
        assert Validator(f) != Validator(g)
        assert hash(Validator(f)) == hash(Validator(f))
        assert repr(Validator(f)) == f"structtype.Validator(fn={f!r})"
        assert Validator(f).__rich_repr__() == [("fn", f)]

    def test_numeric_eq_hash(self):
        assert NumericValidator(gt=1) == NumericValidator(gt=1)
        assert NumericValidator(gt=1) != NumericValidator(gt=2)
        assert NumericValidator(gt=1) != NumericValidator(ge=1)
        assert hash(NumericValidator(gt=1)) == hash(NumericValidator(gt=1))
        assert repr(NumericValidator(gt=1)) == "structtype.NumericValidator(gt=1)"
        assert repr(NumericValidator()) == "structtype.NumericValidator()"
        expected = [("gt", 1), ("le", 2)]
        assert NumericValidator(gt=1, le=2).__rich_repr__() == expected

    def test_str_eq_hash(self):
        assert StrValidator(pattern="a") == StrValidator(pattern="a")
        assert StrValidator(pattern="a") != StrValidator(pattern="b")
        assert hash(StrValidator(pattern="a")) == hash(StrValidator(pattern="a"))
        assert (
            repr(StrValidator(pattern="a", max_length=3))
            == "structtype.StrValidator(pattern='a', max_length=3)"
        )

    def test_bytes_collection_timezone_eq_hash(self):
        assert BytesValidator(min_length=1) == BytesValidator(min_length=1)
        assert BytesValidator(min_length=1) != CollectionValidator(min_length=1)
        assert CollectionValidator() == CollectionValidator()
        assert hash(TimezoneValidator(tz=True)) == hash(TimezoneValidator(tz=True))
        assert TimezoneValidator(tz=True) == TimezoneValidator(tz=True)
        assert TimezoneValidator(tz=True) != TimezoneValidator(tz=False)
        assert (
            repr(TimezoneValidator(False)) == "structtype.TimezoneValidator(tz=False)"
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

        with pytest.raises(TypeError, match="custom types"):

            class Msg(Struct):
                value: Annotated[int, Serializer(load=f)]

    def test_native_type_rejected_in_nested_annotation_at_class_creation(self):
        def f(x):
            return x

        with pytest.raises(TypeError, match="custom types"):

            class Msg(Struct):
                values: list[Annotated[str, Serializer(dump=str)]]

    def test_native_type_rejected_in_direct_decoder(self):
        def f(x):
            return x

        with pytest.raises(TypeError, match="custom types"):
            JSONDecoder(Annotated[int, Serializer(load=f)])

    def test_native_type_rejected_in_direct_decoder_nested(self):
        def f(x):
            return x

        with pytest.raises(TypeError, match="custom types"):
            JSONDecoder(list[Annotated[int, Serializer(load=f)]])

    def test_multiple_serializers_in_one_position_rejected(self):
        def f(x):
            return x

        # Class creation tolerates it for now; the lazy decoder build rejects.
        with pytest.raises(TypeError, match="Multiple `Serializer` annotations"):
            JSONDecoder(Annotated[Color, Serializer(load=f), Serializer(load=f)])

    def test_serializer_without_callables_ignored(self):
        # An empty Serializer is inert and applicable to any type, matching
        # the old permissive `Field()` behavior.
        class MsgCustom(Struct):
            color: Annotated[Color, Serializer()]

        class MsgNative(Struct):
            value: Annotated[int, Serializer()]

        c = Color("red")
        out = MsgCustom.struct_validate({"color": c})
        assert out.color is c
        assert MsgNative.struct_validate_json(b'{"value": 42}') == MsgNative(42)

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
        ta = StructAdapter(Annotated[list[int], CollectionValidator(min_length=1)])
        assert ta.struct_validate_json(b"[1]") == [1]
        ta = StructAdapter(Annotated[int, NumericValidator(ge=0)])
        assert ta.struct_validate_json(b"42") == 42


class TestNumericValidatorLowering:
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
            x: Annotated[int, NumericValidator(**kwargs)]

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
            x: Annotated[float, NumericValidator(**kwargs)]

        assert Ex.struct_validate_json(b'{"x": %r}' % good) == Ex(good)
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": %r}' % bad)

    def test_nested_in_list_element(self):
        class Ex(Struct):
            xs: list[Annotated[int, NumericValidator(ge=0)]]

        out = Ex.struct_validate_json(b'{"xs": [1, 2]}')
        assert out.xs == [1, 2]
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"xs": [1, -1]}')

    def test_error_message_carries_path(self):
        class Ex(Struct):
            x: Annotated[int, NumericValidator(ge=0)]

        with pytest.raises(ValidationError, match=r"Expected `int` >= 0 - at `\$.x`"):
            Ex.struct_validate_json(b'{"x": -1}')

    def test_empty_numeric_validator_is_inert(self):
        class Ex(Struct):
            x: Annotated[int, NumericValidator()]

        assert Ex.struct_validate_json(b'{"x": -5}') == Ex(-5)


class TestStrValidatorLowering:
    def test_passes(self):
        class Ex(Struct):
            x: Annotated[str, StrValidator(min_length=2, max_length=4, pattern="a+")]

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
            x: Annotated[str, StrValidator(min_length=2, max_length=4, pattern="a+")]

        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": %s}' % value.encode())


class TestBytesValidatorLowering:
    def test_min_length(self):
        class Ex(Struct):
            x: Annotated[bytes, BytesValidator(min_length=1)]

        good = base64.b64encode(b"ab")
        bad = base64.b64encode(b"")
        assert Ex.struct_validate_json(b'{"x": "%s"}' % good) == Ex(b"ab")
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": "%s"}' % bad)


class TestCollectionValidatorLowering:
    def test_list_min_length(self):
        class Ex(Struct):
            x: Annotated[list[int], CollectionValidator(min_length=1)]

        assert Ex.struct_validate_json(b'{"x": [1]}') == Ex([1])
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": []}')

    def test_dict_max_length(self):
        class Ex(Struct):
            x: Annotated[dict[str, int], CollectionValidator(max_length=1)]

        assert Ex.struct_validate_json(b'{"x": {"a": 1}}') == Ex({"a": 1})
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": {"a": 1, "b": 2}}')


class TestTimezoneValidatorLowering:
    AWARE = b'"2020-01-01T00:00:00+00:00"'
    NAIVE = b'"2020-01-01T00:00:00"'

    def test_aware_required(self):
        class Ex(Struct):
            x: Annotated[datetime.datetime, TimezoneValidator(tz=True)]

        assert b"+00:00" in Ex.struct_validate_json(b'{"x": %s}' % self.AWARE).x.isoformat().encode()
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": %s}' % self.NAIVE)

    def test_naive_required(self):
        class Ex(Struct):
            x: Annotated[datetime.datetime, TimezoneValidator(tz=False)]

        assert Ex.struct_validate_json(b'{"x": %s}' % self.NAIVE).x == datetime.datetime(
            2020, 1, 1
        )
        with pytest.raises(ValidationError):
            Ex.struct_validate_json(b'{"x": %s}' % self.AWARE)

    def test_time_field(self):
        class Ex(Struct):
            x: Annotated[datetime.time, TimezoneValidator(tz=True)]

        assert Ex.struct_validate_json(b'{"x": "12:00:00+00:00"}').x.tzinfo is not None


class TestValidatorApplicability:
    @pytest.mark.parametrize(
        "typ",
        [str, bytes, list[int], dict[str, int], datetime.datetime],
    )
    def test_numeric_validator_on_non_numeric(self, typ):
        with pytest.raises(TypeError, match="numeric"):
            JSONDecoder(Annotated[typ, NumericValidator(ge=0)])

    def test_str_validator_on_int(self):
        with pytest.raises(TypeError, match="str"):
            JSONDecoder(Annotated[int, StrValidator(pattern="a+")])

    def test_bytes_validator_on_int(self):
        with pytest.raises(TypeError, match="bytes"):
            JSONDecoder(Annotated[int, BytesValidator(min_length=1)])

    def test_collection_validator_on_int(self):
        with pytest.raises(TypeError, match="collection"):
            JSONDecoder(Annotated[int, CollectionValidator(min_length=1)])

    def test_timezone_validator_on_int(self):
        with pytest.raises(TypeError, match="datetime or time"):
            JSONDecoder(Annotated[int, TimezoneValidator(tz=True)])

    def test_applicability_checked_at_decoder_build(self):
        # Errors surface when the decoder is built, not when values decode.
        dec = None
        with pytest.raises(TypeError):
            dec = JSONDecoder(Annotated[str, NumericValidator(ge=0)])
        assert dec is None


class TestMultipleValidatorsRejected:
    def test_two_base_validators(self):
        with pytest.raises(TypeError, match="Multiple"):
            JSONDecoder(Annotated[int, Validator(f), Validator(g)])

    def test_base_plus_fast(self):
        with pytest.raises(TypeError, match="Multiple"):
            JSONDecoder(Annotated[int, Validator(f), NumericValidator(ge=0)])

    def test_fast_plus_fast(self):
        with pytest.raises(TypeError, match="Multiple"):
            JSONDecoder(Annotated[int, NumericValidator(ge=0), StrValidator(pattern="a")])

    def test_subclass_instances(self):
        class Even(Validator):
            def __call__(self, v):
                if v % 2:
                    raise ValueError("not even")

        with pytest.raises(TypeError, match="Multiple"):
            JSONDecoder(Annotated[int, Even(), Even()])
        with pytest.raises(TypeError, match="Multiple"):
            JSONDecoder(Annotated[int, Even(), NumericValidator(gt=0)])


class TestUserValidatorSmoke:
    """Full user-validator behavior lands in a later task; here we only check
    that annotations carrying base/user validators don't break decoder builds."""

    def test_bare_validator_is_noop(self):
        class Ex(Struct):
            x: Annotated[int, Validator()]

        assert Ex.struct_validate_json(b'{"x": 5}') == Ex(5)
        # No fast constraints lowered; anything passes for now
        assert Ex.struct_validate_json(b'{"x": -5}') == Ex(-5)

    def test_validator_with_fn_is_noop_on_decode(self):
        calls = []

        def record(v):
            calls.append(v)

        class Ex(Struct):
            x: Annotated[int, Validator(record)]

        assert Ex.struct_validate_json(b'{"x": 5}') == Ex(5)

    def test_user_subclass_instance(self):
        class Even(Validator):
            def __call__(self, v):
                if v % 2:
                    raise ValueError("not even")

        class Ex(Struct):
            x: Annotated[int, Even()]

        # Decoder construction must not crash; invocation lands later
        assert Ex.struct_validate_json(b'{"x": 4}') == Ex(4)

    def test_user_validator_with_serializer_on_custom_type(self):
        class MsgCustom(Struct):
            color: Annotated[Color, Serializer(), Validator()]

        c = Color("red")
        assert MsgCustom.struct_validate({"color": c}).color is c


class TestUserValidatorInvocation:
    """Task 7: base/user Validator instances are invoked at runtime, after
    type-check/load, wherever constraints are enforced."""

    def test_container_typed_validator(self):
        # The getter must return the validator instance itself, not an
        # element TypeNode (which would crash or misfire).
        class Ex(Struct):
            xs: Annotated[list[int], Validator(lambda v: None)]

        assert Ex.struct_validate_json(b'{"xs": [1, 2]}') == Ex([1, 2])

    def test_custom_type_load_then_validator_in_order(self):
        order = []

        def load(value):
            order.append("load")
            return Color(tuple(value))

        def check(color):
            order.append(("check", color))

        class Msg(Struct):
            color: Annotated[Color, Serializer(load=load), Validator(check)]

        out = Msg.struct_validate_json(b'{"color": [1, 2]}')
        assert out.color == Color((1, 2))
        # load runs first; the validator receives the loaded instance
        assert order == ["load", ("check", Color((1, 2)))]

    def test_validator_failure_wraps_path_context(self):
        def fail(v):
            raise ValueError("no good")

        class Ex(Struct):
            x: Annotated[int, Validator(fail)]

        with pytest.raises(ValidationError, match=r"no good - at `\$.x`"):
            Ex.struct_validate_json(b'{"x": 5}')

    def test_validator_failure_nested_path(self):
        def fail(v):
            raise ValueError("no good")

        class Ex(Struct):
            xs: list[Annotated[int, Validator(fail)]]

        with pytest.raises(ValidationError, match=r"no good - at `\$.xs\[0\]`"):
            Ex.struct_validate_json(b'{"xs": [0, 1]}')

    def test_pure_python_subclass_invoked(self):
        class Even(Validator):
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
        class Even(Validator):
            def __call__(self, v):
                if v % 2:
                    raise ValueError("not even")

        class First(Struct):
            x: Annotated[int, Even()]

        class Other(Struct):
            x: Annotated[int, Validator()]

        # Regression: on Python 3.10, bare Validator() == Even() due to
        # inherited richcompare, causing typing.Annotated to cache them as
        # the same object. Verify the two struct types are independent.
        assert Other.struct_validate_json(b'{"x": -5}') == Other(x=-5)
        assert First.struct_validate_json(b'{"x": 4}') == First(x=4)

    @pytest.mark.parametrize(
        "annotation,payload",
        [
            ('Annotated[int, Validator()]', b'{"x": -5}'),
            ('Annotated[list[int], Validator()]', b'{"x": []}'),
            ('Annotated[Shade, Validator()]', b'{"x": {"name": "t"}}'),
        ],
    )
    def test_bare_validator_is_inert(self, annotation, payload):
        ns = {
            "Annotated": Annotated,
            "Validator": Validator,
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
            inner: Annotated[Inner, Validator()]

        assert Outer.struct_validate({"inner": Inner()}).inner.y == 0

    def test_numeric_fast_subclass_as_instance_still_fires(self):
        # Fast subclasses lower to bitflag checks; the ge=0 violation must
        # still be caught even though no USER_VALIDATOR call happens.
        class Ex(Struct):
            x: Annotated[int, NumericValidator(ge=0)]

        with pytest.raises(ValidationError, match="Expected `int` >= 0"):
            Ex.struct_validate_json(b'{"x": -1}')
        with pytest.raises(ValidationError, match="Expected `int` >= 0"):
            Ex(-1).struct_validate_self()

    def test_optional_null_skips_validator(self):
        seen = []

        class Ex(Struct):
            x: Optional[Annotated[str, Validator(lambda v: seen.append(v))]]

        assert Ex.struct_validate_json(b'{"x": null}') == Ex(None)
        assert seen == []
        Ex.struct_validate_json(b'{"x": "s"}')
        assert seen == ["s"]

    def test_struct_validate_self_field_context(self):
        def fail(v):
            raise ValueError("selfcheck boom")

        class Ex(Struct):
            x: Annotated[int, Validator(fail)]

        with pytest.raises(
            ValidationError, match=r"selfcheck boom - at `\$\.x`"
        ):
            Ex(1).struct_validate_self()

    def test_struct_validate_self_nested_struct_field(self):
        class Inner(Struct):
            y: int = 0

        def fail(v):
            raise ValueError("bad inner")

        class Outer(Struct):
            inner: Annotated[Inner, Validator(fail)]

        with pytest.raises(ValidationError, match=r"bad inner - at `\$.inner`"):
            Outer(Inner()).struct_validate_self()

    def test_dict_key_validator(self):
        dec = JSONDecoder(dict[Annotated[int, NumericValidator(ge=0)], int])
        assert dec.decode(b'{"1": 2}') == {1: 2}
        with pytest.raises(ValidationError):
            dec.decode(b'{"-1": 2}')

    def test_adhoc_decoder_top_level(self):
        dec = JSONDecoder(Annotated[list[int], CollectionValidator(min_length=1)])
        assert dec.decode(b"[1]") == [1]

    def test_no_validation_on_encode(self):
        calls = []

        class Ex(Struct):
            x: Annotated[int, Validator(calls.append)]

        assert Ex(5).struct_dump_json() == b'{"x":5}'
        assert calls == []


class TestValidateSelfCheckTypesOnly:
    """struct_validate_self and validate_on_init are pure type-checks:
    no Serializer.load, no protocol conversion, only isinstance + Validators."""

    def test_custom_field_wrong_type_raises(self):
        calls = []

        class Color:
            def __init__(self, v):
                self.v = v

        ser = Serializer(load=lambda s: (calls.append("load"), Color(s))[1])

        class Ex(Struct):
            struct_config = StructConfig(validate_on_init=True)
            c: Annotated[Color, ser]

        # validate_on_init should raise because "blue" isn't a Color
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
            struct_config = StructConfig(validate_on_init=True)
            c: Annotated[Color, ser]

        # Already the right type — passes, load not called
        Ex(Color("red")).struct_validate_self()
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
            ex.struct_validate_self()
        assert calls == []  # load was NOT called

    def test_protocol_not_called_on_mismatch(self):
        calls = []

        class Color:
            @classmethod
            def struct_validate(cls, d):
                calls.append("protocol")
                return cls(d["v"])

        class Ex(Struct):
            struct_config = StructConfig(validate_on_init=True)
            c: Color

        with pytest.raises(ValidationError, match="Expected `Color`, got `dict`"):
            Ex({"v": 1})
        assert calls == []  # protocol was NOT called

    def test_optional_none_passes(self):
        class Color:
            def __init__(self, v):
                self.v = v

        class Ex(Struct):
            struct_config = StructConfig(validate_on_init=True)
            c: Optional[Annotated[Color, Serializer(load=Color)]]

        # None is valid for Optional[Color]
        Ex(None).struct_validate_self()

    def test_validator_called_on_correct_type(self):
        seen = []

        class Color:
            def __init__(self, v):
                self.v = v

        class Ex(Struct):
            struct_config = StructConfig(validate_on_init=True)
            c: Annotated[Color, Serializer(load=Color), Validator(lambda v: seen.append(v.v))]

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

    def test_validate_on_init_raises(self):
        calls = []

        class Color:
            def __init__(self, v):
                self.v = v

        ser = Serializer(load=lambda s: (calls.append("load"), Color(s))[1])

        class Ex(Struct):
            struct_config = StructConfig(validate_on_init=True)
            c: Annotated[Color, ser]

        with pytest.raises(ValidationError):
            Ex("blue")
        assert calls == []  # load was NOT called on constructor


class TestCompositionRules:
    """Class-creation enforcement of at most one Field, Serializer, Validator
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
        with pytest.raises(TypeError, match="Multiple `Validator` annotations"):
            class Bad(Struct):
                x: Annotated[int, Validator(), Validator()]

    def test_multiple_fast_validators_rejected(self):
        with pytest.raises(TypeError, match="Multiple `Validator` annotations"):
            class Bad(Struct):
                x: Annotated[int, NumericValidator(ge=0), StrValidator(min_length=1)]

    def test_cross_kind_combo_allowed(self):
        def fn(v):
            pass

        def dump(v):
            return v

        class Good(Struct):
            x: Annotated[Color, Field(title="x"), Serializer(dump=dump), Validator(fn)]

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
                Color, Field(title="x"), Serializer(load=load, dump=dump), Validator(fn)
            ]

        assert Good.__struct_fields__ == ("x",)


class TestValidatorApplicability:
    """Validator-vs-type-kind checks enforced at class creation time."""

    def test_numeric_validator_on_str_rejected(self):
        with pytest.raises(TypeError, match="NumericValidator.*numeric types"):
            class Bad(Struct):
                x: Annotated[str, NumericValidator(ge=0)]

    def test_str_validator_on_int_rejected(self):
        with pytest.raises(TypeError, match="StrValidator.*str.*types"):
            class Bad(Struct):
                x: Annotated[int, StrValidator(min_length=1)]

    def test_timezone_validator_on_int_rejected(self):
        with pytest.raises(TypeError, match="TimezoneValidator.*datetime.*time"):
            class Bad(Struct):
                x: Annotated[int, TimezoneValidator(tz=True)]

    def test_collection_validator_on_int_rejected(self):
        with pytest.raises(TypeError, match="CollectionValidator.*collection"):
            class Bad(Struct):
                x: Annotated[int, CollectionValidator(min_length=1)]

    def test_bytes_validator_on_int_rejected(self):
        with pytest.raises(TypeError, match="BytesValidator.*bytes-like"):
            class Bad(Struct):
                x: Annotated[int, BytesValidator(min_length=1)]

    def test_numeric_validator_on_float_ok(self):
        class Good(Struct):
            x: Annotated[float, NumericValidator(ge=0.0)]

        assert Good.__struct_fields__ == ("x",)

    def test_collection_validator_on_list_ok(self):
        class Good(Struct):
            x: Annotated[list[int], CollectionValidator(min_length=1)]

        assert Good.__struct_fields__ == ("x",)

    def test_collection_validator_on_dict_ok(self):
        class Good(Struct):
            x: Annotated[dict[str, int], CollectionValidator(min_length=1)]

        assert Good.__struct_fields__ == ("x",)

    def test_collection_validator_on_set_ok(self):
        class Good(Struct):
            x: Annotated[set[int], CollectionValidator(min_length=1)]

        assert Good.__struct_fields__ == ("x",)

    def test_collection_validator_on_tuple_ok(self):
        class Good(Struct):
            x: Annotated[tuple[int, ...], CollectionValidator(min_length=1)]

        assert Good.__struct_fields__ == ("x",)

    def test_timezone_validator_on_datetime_ok(self):
        class Good(Struct):
            x: Annotated[datetime.datetime, TimezoneValidator(tz=True)]

        assert Good.__struct_fields__ == ("x",)

    def test_timezone_validator_on_time_ok(self):
        class Good(Struct):
            x: Annotated[datetime.time, TimezoneValidator(tz=False)]

        assert Good.__struct_fields__ == ("x",)

    def test_bytes_validator_on_bytes_ok(self):
        class Good(Struct):
            x: Annotated[bytes, BytesValidator(min_length=1)]

        assert Good.__struct_fields__ == ("x",)

    def test_str_validator_on_str_ok(self):
        class Good(Struct):
            x: Annotated[str, StrValidator(min_length=1)]

        assert Good.__struct_fields__ == ("x",)

    def test_numeric_validator_on_int_ok(self):
        class Good(Struct):
            x: Annotated[int, NumericValidator(ge=0)]

        assert Good.__struct_fields__ == ("x",)

    def test_union_deferred(self):
        """Union types should NOT raise at class creation; defer to lazy decoder."""
        class Good(Struct):
            x: Annotated[Union[int, str], NumericValidator(ge=0)]

        assert Good.__struct_fields__ == ("x",)

    def test_union_deferred_at_definition(self):
        """Python 3.10+ union syntax should also defer."""
        class Good(Struct):
            x: Annotated[int | str, NumericValidator(ge=0)]

        assert Good.__struct_fields__ == ("x",)

    def test_adhoc_decoder_still_raises(self):
        """Ad-hoc JSONDecoder(invalid combo) still raises at decoder build time."""
        with pytest.raises(TypeError, match="numeric"):
            JSONDecoder(Annotated[str, NumericValidator(ge=0)])

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
        """Serializer.dump works alongside Validator constraints on same field."""
        def dump(v):
            return list(v.rgb)

        def load(v):
            return Color(tuple(v))

        class Good(Struct):
            x: Annotated[
                Color,
                Serializer(load=load, dump=dump),
                Validator(lambda v: None),
            ]

        obj = Good(Color((1, 2, 3)))
        assert obj.struct_dump_json() == b'{"x":[1,2,3]}'

    def test_nested_collection_element_validator(self):
        """Validator on element type inside a collection defers (inner is leaf)."""
        class Good(Struct):
            x: list[Annotated[int, NumericValidator(ge=0)]]

        assert Good.__struct_fields__ == ("x",)

    def test_nested_collection_element_validator_rejects(self):
        """Validator mismatch on element type inside a collection raises."""
        with pytest.raises(TypeError, match="NumericValidator.*numeric"):
            class Bad(Struct):
                x: list[Annotated[str, NumericValidator(ge=0)]]
