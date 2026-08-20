import copy
import datetime
import enum
import gc
import operator
import pickle
import sys
import textwrap
import weakref
from contextlib import contextmanager
from inspect import Parameter, Signature
from typing import Annotated, Any, Generic, TypeVar

import pytest

import structtype
from structtype import NODEFAULT, UNSET, Factory, Field, Struct, StructConfig

from .utils import temp_module

if hasattr(copy, "replace"):
    # Added in Python 3.13
    copy_replace = copy.replace
else:

    def copy_replace(s, **changes):
        return s.__replace__(**changes)


@contextmanager
def nogc():
    """Temporarily disable GC"""
    try:
        gc.disable()
        yield
    finally:
        gc.enable()


class Fruit(enum.IntEnum):
    APPLE = 1
    BANANA = 2


def as_tuple(x):
    return tuple(getattr(x, f) for f in x.__struct_fields__)


@pytest.mark.parametrize("obj, str_obj", [(UNSET, "UNSET"), (NODEFAULT, "NODEFAULT")])
@pytest.mark.parametrize("protocol", range(pickle.HIGHEST_PROTOCOL + 1))
def test_singletons(obj, str_obj, protocol):
    assert str(obj) == str_obj
    assert pickle.loads(pickle.dumps(obj, protocol=protocol)) is obj

    cls = type(obj)
    assert cls() is obj
    with pytest.raises(TypeError):
        cls(1)
    with pytest.raises(TypeError):
        cls(foo=1)
    if obj is UNSET:
        assert bool(obj) is False
    else:
        assert bool(obj) is True


def test_field():
    f1 = structtype.Field()
    assert f1.alias is None

    f2 = structtype.Field(alias="foo")
    assert f2.alias == "foo"

    f3 = structtype.Field(alias=None)
    assert f3.alias is None

    with pytest.raises(TypeError, match="keyword argument"):
        structtype.Field(default=1)

    with pytest.raises(TypeError, match="keyword argument"):
        structtype.Field(default_factory=int)

    with pytest.raises(TypeError, match="must be a str or None"):
        structtype.Field(alias=b"bad")


def test_field_repr_roundtrip():
    f = structtype.Field(gt=0)
    assert "gt=0" in repr(f)
    assert "default" not in repr(f)
    assert "default_factory" not in repr(f)


def test_factory():
    f = structtype.Factory(list)
    assert f.factory is list

    with pytest.raises(TypeError, match="factory must be callable"):
        structtype.Factory(1)

    with pytest.raises(TypeError, match="expected 1 argument"):
        structtype.Factory()

    with pytest.raises(TypeError, match="no keyword arguments"):
        structtype.Factory(factory=list)


def test_struct_class_attributes():
    assert Struct.__struct_fields__ == ()
    assert Struct.__struct_alias_fields__ == ()
    assert Struct.__struct_defaults__ == ()
    assert Struct.__match_args__ == ()
    assert Struct.__slots__ == ()
    assert Struct.__module__ == "structtype"
    assert isinstance(Struct.__struct_config__, StructConfig)


def test_struct_class_and_instance_dir():
    expected = {"__struct_fields__", "__struct_config__"}
    assert expected.issubset(dir(Struct))
    assert expected.issubset(dir(Struct()))


def test_struct_instance_attributes():
    class Test(Struct):
        c: int
        b: float
        a: str = "hello"

    x = Test(1, 2.0, a="goodbye")

    assert x.__struct_fields__ == ("c", "b", "a")
    assert x.__struct_alias_fields__ == ("c", "b", "a")
    assert x.__struct_fields__ is x.__struct_alias_fields__
    assert x.__struct_defaults__ == ("hello",)
    assert x.__slots__ == ("a", "b", "c")
    assert isinstance(x.__struct_config__, StructConfig)

    assert x.c == 1
    assert x.b == 2.0
    assert x.a == "goodbye"


def test_struct_subclass_forbids_init_new_slots():
    with pytest.raises(TypeError, match="__init__"):

        class Test1(Struct):
            a: int

            def __init__(self, a):
                pass

    with pytest.raises(TypeError, match="__new__"):

        class Test2(Struct):
            a: int

            def __new__(self, a):
                pass

    with pytest.raises(TypeError, match="__slots__"):

        class Test3(Struct):
            __slots__ = ("a",)
            a: int


def test_struct_subclass_forbidden_field_names():
    with pytest.raises(
        TypeError, match="Cannot have a struct field named '__weakref__'"
    ):

        class Test1(Struct):
            __weakref__: int

    with pytest.raises(TypeError, match="Cannot have a struct field named '__dict__'"):

        class Test2(Struct):
            __dict__: int

    with pytest.raises(
        TypeError, match="Cannot have a struct field named '__structtype_cached_hash__'"
    ):

        class Test3(Struct):
            __structtype_cached_hash__: int


class TestMixins:
    def test_mixin_no_slots(self):
        class Mixin:
            def method(self):
                pass

        class Test1(Struct, Mixin):
            pass

        assert issubclass(Test1, Mixin)
        assert Test1.__dictoffset__ != 0
        assert Test1.__weakrefoffset__ != 0

        class Test2(Struct, Mixin, dict=True, weakref=True):
            pass

        assert Test2.__dictoffset__ != 0
        assert Test2.__weakrefoffset__ != 0

    def test_mixin_slots(self):
        class Mixin:
            __slots__ = ()

            def method(self):
                pass

        class Test1(Struct, Mixin):
            pass

        assert issubclass(Test1, Mixin)
        assert Test1.__dictoffset__ == 0
        assert Test1.__weakrefoffset__ == 0

        class Test2(Struct, Mixin, dict=True, weakref=True):
            pass

        assert Test2.__dictoffset__ != 0
        assert Test2.__weakrefoffset__ != 0

    def test_mixin_nonempty_slots(self):
        class Mixin:
            __slots__ = "_state"

            def method(self):
                try:
                    return self._state
                except AttributeError:
                    self._state = self.x + 1
                    return self._state

        class Test(Struct, Mixin):
            x: int

        assert Test.__dictoffset__ == 0

        t = Test(1)
        assert t.method() == 2
        assert t.method() == 2

    def test_mixin_forbids_init(self):
        class Mixin:
            def __init__(self):
                pass

        with pytest.raises(TypeError, match="cannot define __init__"):

            class Test(Struct, Mixin):
                pass

    def test_mixin_forbids_new(self):
        class Mixin:
            def __new__(self):
                pass

        with pytest.raises(TypeError, match="cannot define __new__"):

            class Test(Struct, Mixin):
                pass

    def test_mixin_builtin_type_errors(self):
        with pytest.raises(TypeError):

            class Test(Struct, Exception):
                pass


def test_struct_subclass_forbids_non_types():
    # Currently this failcase is handled by CPython's internals, but it's good
    # to make sure this user error actually errors.
    class Foo:
        pass

    with pytest.raises(TypeError):

        class Test(structtype.Struct, Foo()):
            pass


def test_struct_subclass_forbids_mixed_layouts():
    class A(Struct):
        a: int
        b: int

    class B(Struct):
        c: int
        d: int

    # This error is raised by cpython
    with pytest.raises(TypeError, match="lay-out conflict"):

        class C(A, B):
            pass


def test_struct_errors_nicely_if_used_in_init_subclass():
    ran = False

    class Test(Struct):
        def __init_subclass__(cls):
            # Class attributes aren't yet defined, error nicely
            for attr in [
                "__struct_fields__",
                "__struct_alias_fields__",
                "__match_args__",
                "__struct_defaults__",
            ]:
                with pytest.raises(AttributeError):
                    getattr(cls, attr)

            # Init doesn't work
            with pytest.raises(Exception):
                cls()

            # Decoder/decode doesn't work
            with pytest.raises(ValueError, match="isn't fully defined"):
                structtype._core.JSONDecoder(cls)

                with pytest.raises(ValueError, match="isn't fully defined"):
                    proto.decode(b"", type=cls)

            nonlocal ran
            ran = True

    class Subclass(Test):
        x: int

    assert ran


class TestStructParameterOrdering:
    """Tests for parsing parameter types & defaults from one or more class
    definitions."""

    def test_no_args(self):
        class Test(Struct):
            pass

        assert Test.__struct_fields__ == ()
        assert Test.__struct_defaults__ == ()
        assert Test.__match_args__ == ()
        assert Test.__slots__ == ()

    def test_all_positional(self):
        class Test(Struct):
            y: float
            x: int

        assert Test.__struct_fields__ == ("y", "x")
        assert Test.__struct_defaults__ == ()
        assert Test.__match_args__ == ("y", "x")
        assert Test.__slots__ == ("x", "y")

    def test_all_positional_with_defaults(self):
        class Test(Struct):
            y: int = 1
            x: float = 2.0

        assert Test.__struct_fields__ == ("y", "x")
        assert Test.__struct_defaults__ == (1, 2.0)
        assert Test.__match_args__ == ("y", "x")
        assert Test.__slots__ == ("x", "y")

    def test_subclass_no_change(self):
        class Test(Struct):
            y: float
            x: int

        class Test2(Test):
            pass

        assert Test2.__struct_fields__ == ("y", "x")
        assert Test2.__struct_defaults__ == ()
        assert Test2.__match_args__ == ("y", "x")
        assert Test2.__slots__ == ()

    def test_subclass_extends(self):
        class Test(Struct):
            c: int
            b: float
            d: int = 1
            a: float = 2.0

        class Test2(Test):
            e: str = "3.0"
            f: float = 4.0

        assert Test2.__struct_fields__ == ("c", "b", "d", "a", "e", "f")
        assert Test2.__struct_defaults__ == (1, 2.0, "3.0", 4.0)
        assert Test2.__match_args__ == ("c", "b", "d", "a", "e", "f")
        assert Test2.__slots__ == ("e", "f")

    def test_subclass_overrides(self):
        class Test(Struct):
            c: int
            b: int
            d: int = 1
            a: float = 2.0

        class Test2(Test):
            b: float = 3  # switch to keyword, change type
            d: int = 4  # change default
            e: float = 5.0  # new

        assert Test2.__struct_fields__ == ("c", "b", "d", "a", "e")
        assert Test2.__struct_defaults__ == (3, 4, 2.0, 5.0)
        assert Test2.__match_args__ == ("c", "b", "d", "a", "e")
        assert Test2.__slots__ == ("e",)

    def test_subclass_with_mixin(self):
        class A(Struct):
            b: int
            a: float = 1.0

        class Mixin(Struct):
            pass

        class B(A, Mixin):
            a: float = 2.0

        assert B.__struct_fields__ == ("b", "a")
        assert B.__struct_defaults__ == (2.0,)
        assert B.__match_args__ == ("b", "a")
        assert B.__slots__ == ()

    def test_positional_after_keyword_errors(self):
        with pytest.raises(TypeError) as rec:

            class Test(Struct):
                a: int
                b: int = 1
                c: float

        assert "Required field 'c' cannot follow optional fields" in str(rec.value)

    def test_positional_after_keyword_subclass_errors(self):
        class Base(Struct):
            a: int
            b: int = 1

        with pytest.raises(TypeError) as rec:

            class Test(Base):
                c: float

        assert "Required field 'c' cannot follow optional fields" in str(rec.value)

    def test_kw_only_positional(self):
        class Test(Struct, kw_only=True):
            b: int
            a: int

        assert Test.__struct_fields__ == ("b", "a")
        assert Test.__struct_defaults__ == ()
        assert Test.__match_args__ == ()
        assert Test.__slots__ == ("a", "b")

    def test_kw_only_mixed(self):
        class Test(Struct, kw_only=True):
            b: int
            a: int = 0
            c: int
            d: int = 1

        assert Test.__struct_fields__ == ("b", "a", "c", "d")
        assert Test.__struct_defaults__ == (0, NODEFAULT, 1)
        assert Test.__match_args__ == ()
        assert Test.__slots__ == ("a", "b", "c", "d")

    def test_kw_only_positional_base_class(self):
        class Base(Struct, kw_only=True):
            b: int
            a: int

        class S1(Base):
            d: int
            c: int

        class S2(Base):
            d: int
            c: int = 1

        assert S1.__struct_fields__ == ("d", "c", "b", "a")
        assert S1.__struct_defaults__ == ()
        assert S1.__match_args__ == ("d", "c")
        assert S1.__slots__ == ("c", "d")

        assert S2.__struct_fields__ == ("d", "c", "b", "a")
        assert S2.__struct_defaults__ == (1, NODEFAULT, NODEFAULT)
        assert S2.__match_args__ == ("d", "c")
        assert S2.__slots__ == ("c", "d")

    def test_kw_only_base_class(self):
        class Base(Struct, kw_only=True):
            b: int = 1
            a: int

        class S1(Base):
            d: int
            c: int = 2

        assert S1.__struct_fields__ == ("d", "c", "b", "a")
        assert S1.__struct_defaults__ == (2, 1, NODEFAULT)
        assert S1.__match_args__ == ("d", "c")
        assert S1.__slots__ == ("c", "d")

    def test_kw_only_subclass(self):
        class Base(Struct):
            b: int
            a: int

        class S1(Base, kw_only=True):
            d: int
            c: int

        assert S1.__struct_fields__ == ("b", "a", "d", "c")
        assert S1.__struct_defaults__ == ()
        assert S1.__match_args__ == ("b", "a")
        assert S1.__slots__ == ("c", "d")

    def test_kw_only_defaults_subclass(self):
        class Base(Struct):
            b: int
            a: int = 0

        class S1(Base, kw_only=True):
            d: int
            c: int = 1

        assert S1.__struct_fields__ == ("b", "a", "d", "c")
        assert S1.__struct_defaults__ == (0, NODEFAULT, 1)
        assert S1.__match_args__ == ("b", "a")
        assert S1.__slots__ == ("c", "d")

    def test_kw_only_overrides(self):
        class Base(Struct):
            b: int
            a: int = 2

        class S1(Base, kw_only=True):
            b: int
            c: int = 3

        assert S1.__struct_fields__ == ("a", "b", "c")
        assert S1.__struct_defaults__ == (2, NODEFAULT, 3)
        assert S1.__match_args__ == ("a",)
        assert S1.__slots__ == ("c",)

    def test_kw_only_overridden(self):
        class Base(Struct, kw_only=True):
            b: int
            a: int = 2

        class S1(Base):
            b: int
            c: int = 3

        assert S1.__struct_fields__ == ("b", "c", "a")
        assert S1.__struct_defaults__ == (3, 2)
        assert S1.__match_args__ == ("b", "c")
        assert S1.__slots__ == ("c",)


class TestStructInit:
    def test_init_positional(self):
        class Test(Struct):
            a: int
            b: float
            c: int = 3
            d: float = 4.0

        assert as_tuple(Test(1, 2.0)) == (1, 2.0, 3, 4.0)
        assert as_tuple(Test(1, b=2.0)) == (1, 2.0, 3, 4.0)
        assert as_tuple(Test(a=1, b=2.0)) == (1, 2.0, 3, 4.0)
        assert as_tuple(Test(1, b=2.0, c=5)) == (1, 2.0, 5, 4.0)
        assert as_tuple(Test(1, b=2.0, d=5.0)) == (1, 2.0, 3, 5.0)
        assert as_tuple(Test(1, 2.0, 5)) == (1, 2.0, 5, 4.0)
        assert as_tuple(Test(1, 2.0, 5, 6.0)) == (1, 2.0, 5, 6.0)

        with pytest.raises(TypeError, match="Missing required argument 'a'"):
            Test()

        with pytest.raises(TypeError, match="Missing required argument 'b'"):
            Test(1)

        with pytest.raises(TypeError, match="Extra positional arguments provided"):
            Test(1, 2, 3, 4, 5)

        with pytest.raises(TypeError, match="Argument 'a' given by name and position"):
            Test(1, 2, a=3)

        with pytest.raises(TypeError, match="Unexpected keyword argument 'e'"):
            Test(1, 2, e=5)

    def test_init_kw_only(self):
        class Test(Struct, kw_only=True):
            a: int
            b: float = 2.0
            c: int = 3

        assert as_tuple(Test(a=1)) == (1, 2.0, 3)
        assert as_tuple(Test(a=1, b=4.0)) == (1, 4.0, 3)
        assert as_tuple(Test(a=1, c=4)) == (1, 2.0, 4)
        assert as_tuple(Test(a=1, b=4.0, c=5)) == (1, 4.0, 5)

        with pytest.raises(TypeError, match="Missing required argument 'a'"):
            Test()

        with pytest.raises(TypeError, match="Extra positional arguments provided"):
            Test(1)

        with pytest.raises(TypeError, match="Unexpected keyword argument 'e'"):
            Test(a=1, e=5)

    def test_init_kw_only_mixed(self):
        class Base(Struct, kw_only=True):
            c: int = 3
            d: float = 4.0

        class Test(Base):
            a: int
            b: float = 2.0

        assert as_tuple(Test(1)) == (1, 2.0, 3, 4.0)
        assert as_tuple(Test(1, 5.0)) == (1, 5.0, 3, 4.0)
        assert as_tuple(Test(a=1)) == (1, 2.0, 3, 4.0)
        assert as_tuple(Test(a=1, b=5.0)) == (1, 5.0, 3, 4.0)
        assert as_tuple(Test(1, c=5)) == (1, 2.0, 5, 4.0)

        with pytest.raises(TypeError, match="Missing required argument 'a'"):
            Test()

        with pytest.raises(TypeError, match="Argument 'a' given by name and position"):
            Test(1, b=3.0, c=4, a=3)

        with pytest.raises(TypeError, match="Extra positional arguments provided"):
            Test(1, 5.0, 3)

        with pytest.raises(TypeError, match="Unexpected keyword argument 'e'"):
            Test(1, e=5)


class TestSignature:
    def test_signature_no_args(self):
        class Test(Struct):
            pass

        sig = Signature(parameters=[])
        assert Test.__signature__ == sig

    def test_signature_positional(self):
        class Test(Struct):
            b: float
            a: int = 1

        sig = Signature(
            parameters=[
                Parameter("b", Parameter.POSITIONAL_OR_KEYWORD, annotation=float),
                Parameter(
                    "a",
                    Parameter.POSITIONAL_OR_KEYWORD,
                    default=1,
                    annotation=int,
                ),
            ]
        )
        assert Test.__signature__ == sig

    def test_signature_kw_only(self):
        class Base(Struct, kw_only=True):
            c: float
            d: int = 2

        class Test(Base):
            b: float
            a: int = 1

        sig = Signature(
            parameters=[
                Parameter("b", Parameter.POSITIONAL_OR_KEYWORD, annotation=float),
                Parameter(
                    "a",
                    Parameter.POSITIONAL_OR_KEYWORD,
                    default=1,
                    annotation=int,
                ),
                Parameter("c", Parameter.KEYWORD_ONLY, annotation=float),
                Parameter("d", Parameter.KEYWORD_ONLY, default=2, annotation=int),
            ]
        )
        assert Test.__signature__ == sig


class TestRepr:
    def test_repr_base(self):
        x = Struct()
        assert repr(x) == "Struct()"
        assert x.__rich_repr__() == []

    def test_repr_empty(self):
        class Test(Struct):
            pass

        x = Test()
        assert repr(x) == "Test()"
        assert x.__rich_repr__() == []

    def test_repr_one_field(self):
        class Test(Struct):
            a: int

        x = Test(1)
        assert repr(x) == "Test(a=1)"
        assert x.__rich_repr__() == [("a", 1)]

    def test_repr_two_fields(self):
        class Test(Struct):
            a: int
            b: str

        x = Test(1, "y")
        assert repr(x) == "Test(a=1, b='y')"
        assert x.__rich_repr__() == [("a", 1), ("b", "y")]

    def test_repr_omit_defaults_empty(self):
        class Test(Struct, repr_omit_defaults=True):
            pass

        x = Test()
        assert repr(x) == "Test()"
        assert x.__rich_repr__() == []

    def test_repr_omit_defaults_one_field(self):
        class Test(Struct, repr_omit_defaults=True):
            a: int = 0

        x = Test(0)
        assert repr(x) == "Test()"
        assert x.__rich_repr__() == []

        x = Test(1)
        assert repr(x) == "Test(a=1)"
        assert x.__rich_repr__() == [("a", 1)]

    def test_repr_omit_defaults_multiple_fields(self):
        class Test(Struct, repr_omit_defaults=True):
            a: int
            b: int = 0
            c: str = ""

        x = Test(0)
        assert repr(x) == "Test(a=0)"
        assert x.__rich_repr__() == [("a", 0)]

        x = Test(0, b=1)
        assert repr(x) == "Test(a=0, b=1)"
        assert x.__rich_repr__() == [("a", 0), ("b", 1)]

        x = Test(0, c="two")
        assert repr(x) == "Test(a=0, c='two')"
        assert x.__rich_repr__() == [("a", 0), ("c", "two")]

        x = Test(0, b=1, c="two")
        assert repr(x) == "Test(a=0, b=1, c='two')"
        assert x.__rich_repr__() == [("a", 0), ("b", 1), ("c", "two")]

    def test_omit_defaults_factory_collections(self):
        class Test(Struct, omit_defaults=True):
            a: list = Factory(list)
            b: tuple = Factory(tuple)
            c: frozenset = Factory(frozenset)

        assert Test().struct_dump() == {}
        assert Test([1], (1,), frozenset({1})).struct_dump() == {
            "a": [1],
            "b": (1,),
            "c": [1],
        }

    def test_repr_omit_defaults_factory_collections(self):
        class Test(Struct, repr_omit_defaults=True):
            a: tuple = Factory(tuple)
            b: frozenset = Factory(frozenset)

        assert repr(Test()) == "Test()"
        assert repr(Test((1,), frozenset({2}))) == "Test(a=(1,), b=frozenset({2}))"

    def test_repr_recursive(self):
        class Test(Struct):
            a: int
            b: Any

        t = Test(1, Test(2, None))
        t.b.b = t
        assert repr(t) == "Test(a=1, b=Test(a=2, b=...))"

    def test_repr_missing_attr_errors(self):
        class Test(Struct):
            a: int
            b: str

        t = Test(1, "hello")
        del t.b

        with pytest.raises(AttributeError):
            repr(t)

        with pytest.raises(AttributeError):
            t.__rich_repr__()

    def test_repr_errors(self):
        msg = "Oh no!"

        class Bad:
            def __repr__(self):
                raise ValueError(msg)

        class Test(Struct):
            a: object
            b: object

        t = Test(1, Bad())

        with pytest.raises(ValueError, match=msg):
            repr(t)


def test_struct_copy():
    x = copy.copy(Struct())
    assert type(x) is Struct

    class Test(Struct):
        b: int
        a: int

    x = copy.copy(Test(1, 2))
    assert type(x) is Test
    assert x.b == 1
    assert x.a == 2


class FrozenPoint(Struct, frozen=True):
    x: int
    y: int


@pytest.mark.parametrize(
    "default",
    [
        None,
        False,
        True,
        1,
        2.0,
        1.5 + 2.32j,
        b"test",
        "test",
        (),
        frozenset(),
        frozenset((1, (2, 3, 4), 5)),
        Fruit.APPLE,
        datetime.time(1),
        datetime.date.today(),
        datetime.timedelta(seconds=2),
        datetime.datetime.now(),
        FrozenPoint(1, 2),
    ],
)
def test_struct_immutable_defaults_use_instance(default):
    class Test(Struct):
        value: object = default

    t = Test()
    assert t.value is default


@pytest.mark.parametrize("default", [[], {}, set()])
def test_struct_empty_mutable_defaults_fast_copy(default):
    class Test(Struct):
        value: object = default

    t = Test()
    assert t.value == default
    assert t.value is not default


class Point(Struct):
    x: int
    y: int


class PointKWOnly(Struct, kw_only=True):
    x: int
    y: int


@pytest.mark.parametrize("default", [[], {}, set(), bytearray()])
def test_struct_empty_mutable_defaults_work(default):
    class Test(Struct):
        value: object = default

    x = Test().value
    x == default
    assert x is not default


@pytest.mark.parametrize(
    "default",
    [Point(1, 2), [1], {"a": "b"}, {1, 2}, bytearray(b"test")],
)
def test_struct_nonempty_mutable_defaults_error(default):
    with pytest.raises(TypeError) as rec:

        class Test(Struct):
            value: object = default

    assert "as a default value is unsafe" in str(rec.value)
    assert repr(default) in str(rec.value)


def test_struct_defaults_from_field():
    default = []

    class Test(Struct):
        req: int
        x: int = 1
        y: int = Factory(lambda: 2)
        z: list[int] = default

    t = Test(100)
    assert t.req == 100
    assert t.x == 1
    assert t.y == 2
    assert t.z == []
    assert t.z is not default


def test_struct_defaults_from_field_annotated():
    source = """
    from typing import Annotated
    from structtype import Struct, Field, Factory

    class Test(Struct):
        a: int = 42
        b: list = Factory(list)
        c: Annotated[str, Field(alias="ccc")] = "hello"
    """
    with temp_module(source) as mod:
        t = mod.Test()
        assert t.a == 42, f"Expected 42, got {t.a}"
        assert t.b == []
        assert t.c == "hello"
        assert mod.Test.__struct_alias_fields__[2] == "ccc"

    # Test alias only (with kw_only to avoid ordering issue)
    source2 = """
    from typing import Annotated
    from structtype import Struct, Field

    class Test2(Struct, kw_only=True):
        x: int
        y: Annotated[str, Field(alias="yyy")]
    """
    with temp_module(source2) as mod2:
        assert mod2.Test2.__struct_alias_fields__[1] == "yyy"


def test_field_outside_annotated_errors():
    with pytest.raises(TypeError, match="Annotated"):

        class Test(Struct):
            x: int = Field(gt=0)


def test_field_default_kwargs_removed():
    with pytest.raises(TypeError, match="keyword argument"):

        class Test(Struct):
            x: Annotated[int, Field(default=0)]


def test_struct_default_factory_errors():
    def bad():
        raise ValueError("Oh no")

    class Test(Struct):
        x: int = Factory(bad)

    with pytest.raises(ValueError):
        Test()


def test_struct_reference_counting():
    """Test that struct operations that access fields properly decref"""

    class Test(Struct):
        value: list

    data = [1, 2, 3]

    t = Test(data)
    assert sys.getrefcount(data) <= 3

    repr(t)
    assert sys.getrefcount(data) <= 3

    t2 = t.__copy__()
    assert sys.getrefcount(data) <= 4

    assert t == t2
    assert sys.getrefcount(data) <= 4


def test_struct_gc_not_added_if_not_needed():
    """Structs aren't tracked by GC until/unless they reference a container type"""

    class Test(Struct):
        x: object
        y: object

    assert not gc.is_tracked(Test(1, 2))
    assert not gc.is_tracked(Test("hello", "world"))
    assert gc.is_tracked(Test([1, 2, 3], 1))
    assert gc.is_tracked(Test(1, [1, 2, 3]))
    # Tuples are all tracked on creation, but through GC passes eventually
    # become untracked if they don't contain tracked types
    untracked_tuple = (1, 2, 3)
    for i in range(5):
        gc.collect()
        if not gc.is_tracked(untracked_tuple):
            break
    else:
        assert False, "something has changed with Python's GC, investigate"
    assert not gc.is_tracked(Test(1, untracked_tuple))
    tracked_tuple = ([],)
    assert gc.is_tracked(Test(1, tracked_tuple))

    # On mutation, if a tracked objected is stored on a struct, an untracked
    # struct will become tracked
    t = Test(1, 2)
    assert not gc.is_tracked(t)
    t.x = 3
    assert not gc.is_tracked(t)
    t.x = untracked_tuple
    assert not gc.is_tracked(t)
    t.x = []
    assert gc.is_tracked(t)

    # An error in setattr doesn't change tracked status
    t = Test(1, 2)
    assert not gc.is_tracked(t)
    with pytest.raises(AttributeError):
        t.z = []
    assert not gc.is_tracked(t)


class TestStructGC:
    def test_init(self):
        class Test(Struct):
            x: object
            y: object

        assert not gc.is_tracked(Test(1, 2))
        assert gc.is_tracked(Test([1, 2, 3], 1))
        assert gc.is_tracked(Test(1, [1, 2, 3]))

    def test_setattr(self):
        class Test(Struct):
            x: object
            y: object

        t = Test(1, 2)
        assert not gc.is_tracked(t)
        t.x = []
        assert gc.is_tracked(t)

    def test_struct_gc_set_on_copy(self):
        """Copying doesn't go through the struct constructor"""

        class Test(Struct):
            x: object
            y: object

        assert not gc.is_tracked(copy.copy(Test(1, 2)))
        assert not gc.is_tracked(copy.copy(Test(1, ())))
        assert gc.is_tracked(copy.copy(Test(1, [])))

    def test_struct_gc_inherit(self):
        class Base(Struct):
            x: object

        class Child(Base):
            y: object

        assert gc.is_tracked(Child(1, []))
        assert not gc.is_tracked(Child(1, 2))


class TestStructDealloc:
    def test_struct_dealloc_decrefs_type(self):
        class Test1(Struct):
            x: int
            y: int

        class Test2(Struct):
            x: int
            y: int

        with nogc():
            orig_1 = sys.getrefcount(Test1)
            orig_2 = sys.getrefcount(Test2)
            t = Test1(1, 2)
            assert sys.getrefcount(Test1) <= orig_1 + 1
            del t
            assert sys.getrefcount(Test1) <= orig_1
            t = Test2(1, 2)
            assert sys.getrefcount(Test1) <= orig_1
            assert sys.getrefcount(Test2) <= orig_2 + 1
            del t
            assert sys.getrefcount(Test1) <= orig_1
            assert sys.getrefcount(Test2) <= orig_2
            gc.collect()
            assert sys.getrefcount(Test1) == orig_1
            assert sys.getrefcount(Test2) == orig_2

    def test_struct_dealloc_calls_finalizer(self):
        for _ in range(3):
            called = False

            class Test(Struct):
                x: int
                y: int

                def __del__(self):
                    nonlocal called
                    called = True

            t = Test(1, 2)
            if hasattr(gc, "is_finalized"):
                assert not gc.is_finalized(t)
            del t

            assert called

    def test_struct_dealloc_supports_finalizer_resurrection(self):
        for _ in range(3):
            called = False
            new_ref = None

            class Test(Struct):
                x: int
                y: int

                def __del__(self):
                    nonlocal called
                    nonlocal new_ref
                    if not called:
                        called = True
                        new_ref = self

            t = Test(1, 2)

            del t
            assert called
            assert new_ref is not None
            del new_ref

    def test_struct_dealloc_trashcan(self):
        N = 100
        called = set()

        class Node(Struct):
            child: "Node | None" = None

            def __del__(self):
                called.add(id(self))

        node = None
        for _ in range(N):
            node = Node(node)

        del node
        assert len(called) == N

    def test_struct_dealloc_decrefs_fields(self):
        class Test(Struct):
            x: Any

        x = object()
        t = Test(x)
        count = sys.getrefcount(x)
        del t
        assert sys.getrefcount(x) == count - 1

    def test_struct_dealloc_works_with_missing_fields(self):
        class Test(Struct):
            x: Any
            y: Any

        x = object()
        t = Test(x, None)
        del t.y
        count = sys.getrefcount(x)
        del t
        assert sys.getrefcount(x) == count - 1

    def test_struct_dealloc_dict(self):
        class Test(Struct, dict=True):
            x: int

        called = False

        class Flag:
            def __del__(self):
                nonlocal called
                called = True

        t = Test(1)
        t.flag = Flag()
        del t
        assert called

    def test_struct_dealloc_weakref(self):
        class Test(Struct, weakref=True):
            x: int

        t = Test(1)
        # smoketest dealloc weakrefable struct doesn't crash
        del t

        t = Test(1)
        ref = weakref.ref(t)
        assert ref() is not None
        del t
        assert ref() is None

    def test_struct_dealloc_in_gc_properly_handles_type_decref(self):
        def inner():
            class Box(structtype.Struct):
                a: Any

            gc.collect()

            o = Box(None)
            o.a = o

        for _ in range(5):
            inner()
            gc.collect()


@pytest.mark.parametrize("kw_only", [False, True])
@pytest.mark.parametrize("protocol", range(pickle.HIGHEST_PROTOCOL + 1))
def test_struct_pickle(kw_only, protocol):
    cls = PointKWOnly if kw_only else Point
    a = cls(x=1, y=2)
    b = cls(x=3, y=4)

    assert pickle.loads(pickle.dumps(a, protocol=protocol)) == a
    assert pickle.loads(pickle.dumps(b, protocol=protocol)) == b

    del a.x
    with pytest.raises(AttributeError, match="Struct field 'x' is unset"):
        pickle.dumps(a, protocol=protocol)


def test_struct_handles_missing_attributes():
    """If an attribute is unset, raise an AttributeError appropriately"""

    class MyStruct(Struct):
        x: int
        y: int
        z: str = "default"

    t = MyStruct(1, 2)
    del t.y
    t2 = MyStruct(1, 2)

    match = "Struct field 'y' is unset"

    with pytest.raises(AttributeError, match=match):
        repr(t)

    with pytest.raises(AttributeError, match=match):
        copy.copy(t)

    with pytest.raises(AttributeError, match=match):
        t == t2

    with pytest.raises(AttributeError, match=match):
        t2 == t


@pytest.mark.parametrize(
    "option, default",
    [
        ("frozen", False),
        ("order", False),
        ("eq", True),
        ("repr_omit_defaults", False),
        ("array_like", False),
        ("omit_defaults", False),
        ("forbid_unknown_fields", False),
    ],
)
def test_struct_option_precedence(option, default):
    def get(cls):
        return getattr(cls.__struct_config__, option)

    class Default(Struct):
        pass

    assert get(Default) is default

    class Enabled(Struct, **{option: True}):
        pass

    assert get(Enabled) is True

    class Disabled(Struct, **{option: False}):
        pass

    assert get(Disabled) is False

    class T(Enabled):
        pass

    assert get(T) is True

    class T(Enabled, **{option: False}):
        pass

    assert get(T) is False

    class T(Enabled, Default):
        pass

    assert get(T) is True

    class T(Default, Enabled):
        pass

    assert get(T) is True

    class T(Default, Disabled, Enabled):
        pass

    assert get(T) is False


def test_weakref_option():
    class Default(Struct):
        pass

    assert Default.__weakrefoffset__ == 0

    class Enabled(Struct, weakref=True):
        pass

    assert Enabled.__weakrefoffset__ != 0
    assert Enabled.__struct_config__.weakref

    class Disabled(Struct, weakref=False):
        pass

    assert Disabled.__weakrefoffset__ == 0
    assert not Disabled.__struct_config__.weakref

    class T(Enabled):
        pass

    assert T.__weakrefoffset__ != 0
    assert T.__struct_config__.weakref

    class T(Enabled, Default):
        pass

    assert T.__weakrefoffset__ != 0
    assert T.__struct_config__.weakref

    class T(Default, Disabled, Enabled):
        pass

    assert T.__weakrefoffset__ != 0
    assert T.__struct_config__.weakref

    with pytest.raises(ValueError, match="Cannot set `weakref=False`"):

        class T(Enabled, weakref=False):
            pass


def test_dict_option():
    class Default(Struct):
        pass

    assert Default.__dictoffset__ == 0

    class Enabled(Struct, dict=True):
        pass

    assert Enabled.__dictoffset__ != 0
    assert Enabled.__struct_config__.dict

    class Disabled(Struct, dict=False):
        pass

    assert Disabled.__dictoffset__ == 0
    assert not Disabled.__struct_config__.dict

    class T(Enabled):
        pass

    assert T.__dictoffset__ != 0
    assert T.__struct_config__.dict

    class T(Enabled, Default):
        pass

    assert T.__dictoffset__ != 0
    assert T.__struct_config__.dict

    class T(Default, Disabled, Enabled):
        pass

    assert T.__dictoffset__ != 0
    assert T.__struct_config__.dict

    with pytest.raises(ValueError, match="Cannot set `dict=False`"):

        class T(Enabled, dict=False):
            pass


def test_cache_hash_option():
    with pytest.raises(
        ValueError, match="Cannot set cache_hash=True without frozen=True"
    ):

        class Invalid(Struct, cache_hash=True):
            pass

    class Default(Struct, frozen=True):
        pass

    assert "__structtype_cached_hash__" not in Default.__slots__
    assert not Default.__struct_config__.cache_hash

    class Enabled(Struct, cache_hash=True, frozen=True):
        pass

    assert "__structtype_cached_hash__" in Enabled.__slots__
    assert Enabled.__struct_config__.cache_hash

    class Disabled(Struct, cache_hash=False, frozen=True):
        pass

    assert "__structtype_cached_hash__" not in Disabled.__slots__
    assert not Disabled.__struct_config__.cache_hash

    class T(Enabled):
        pass

    assert "__structtype_cached_hash__" not in T.__slots__
    assert T.__struct_config__.cache_hash

    class T(Enabled, Default):
        pass

    assert "__structtype_cached_hash__" not in T.__slots__
    assert T.__struct_config__.cache_hash

    class T(Default, Disabled, Enabled):
        pass

    assert "__structtype_cached_hash__" not in T.__slots__
    assert T.__struct_config__.cache_hash

    with pytest.raises(ValueError, match="Cannot set `cache_hash=False`"):

        class T(Enabled, cache_hash=False):
            pass


def test_invalid_option_raises():
    with pytest.raises(TypeError):

        class Foo(Struct, invalid=True):
            pass


class FrozenPoint(Struct, frozen=True):
    x: int
    y: int


class TestHash:
    def test_frozen_objects_hashable(self):
        p1 = FrozenPoint(1, 2)
        p2 = FrozenPoint(1, 2)
        p3 = FrozenPoint(1, 3)
        assert hash(p1) == hash(p2)
        assert hash(p1) != hash(p3)
        assert p1 == p2
        assert p1 != p3

    def test_frozen_objects_hash_errors_if_field_unhashable(self):
        p = FrozenPoint(1, [2])
        with pytest.raises(TypeError):
            hash(p)

    def test_frozen_hash_mutable_objects_hash_errors(self):
        p = Point(1, 2)
        with pytest.raises(TypeError, match="unhashable type"):
            hash(p)

    def test_hash_includes_type(self):
        class Ex1(Struct, frozen=True):
            x: int

        class Ex2(Struct, frozen=True):
            x: int

        class Ex3(Struct, frozen=True):
            pass

        class Ex4(Struct, frozen=True):
            pass

        assert hash(Ex1(1)) == hash(Ex1(1))
        assert hash(Ex1(1)) != hash(Ex2(1))
        assert hash(Ex3()) == hash(Ex3())
        assert hash(Ex3()) != hash(Ex4())

    def test_cache_hash(self):
        class Inner:
            def __init__(self):
                self.hash_calls = 0

            def __hash__(self):
                self.hash_calls += 1
                return 123

        class Cached(Struct, frozen=True, cache_hash=True):
            x: int
            y: Inner

        assert "__structtype_cached_hash__" in Cached.__slots__
        obj = Cached(1, Inner())
        assert not hasattr(obj, "__structtype_cached_hash__")
        assert hash(obj) == hash(obj)
        assert obj.__structtype_cached_hash__ == hash(obj)
        assert obj.y.hash_calls == 1


class TestSetAttr:
    def test_frozen_objects_no_setattr(self):
        p = FrozenPoint(1, 2)
        with pytest.raises(AttributeError, match="immutable type: 'FrozenPoint'"):
            p.x = 3

    @pytest.mark.parametrize("base_frozen", [True, False])
    def test_override_setattr(self, base_frozen):
        called = False

        class Base(Struct, frozen=base_frozen):
            pass

        class Test(Struct, frozen=False):
            x: Any

            def __setattr__(self, name, value):
                nonlocal called
                called = True
                super().__setattr__(name, value)

        t = Test(1)
        assert not called
        t.x = 2
        assert called
        assert not gc.is_tracked(t)
        t.x = [1]
        assert gc.is_tracked(t)

    def test_override_setattr_inherit(self):
        called = False

        class Base(Struct):
            x: Any

            def __setattr__(self, name, value):
                nonlocal called
                called = True
                super().__setattr__(name, value)

        class Test(Base):
            pass

        t = Test(1)
        assert not called
        t.x = 2
        assert called
        assert not gc.is_tracked(t)
        t.x = [1]
        assert gc.is_tracked(t)

    def test_force_setattr_removed(self):
        class Ex(Struct, frozen=True):
            x: Any

        obj = Ex(1)

        # struct_force_setattr was removed; plain setattr outside __post_init__
        # still raises for frozen structs
        assert not hasattr(obj, "struct_force_setattr")

        with pytest.raises(AttributeError):
            obj.x = 2

    def test_frozen_post_init_plain_setattr_blocked(self):
        class Ex(Struct, frozen=True):
            x: int
            y: int = 0

            def __post_init__(self):
                self.y = self.x * 2

        with pytest.raises(AttributeError):
            Ex(2)

        with pytest.raises(AttributeError):
            Ex.struct_validate_json(b'{"x": 3}')

    @pytest.mark.skipif(
        sys.version_info < (3, 13),
        reason="object.__setattr__ on struct instances requires Python 3.13+",
    )
    def test_frozen_post_init_object_setattr(self):
        class Ex(Struct, frozen=True):
            x: int
            y: int = 0

            def __post_init__(self):
                object.__setattr__(self, "y", self.x * 2)

        obj = Ex(2)
        assert obj.y == 4

        obj2 = Ex.struct_validate_json(b'{"x": 3}')
        assert obj2.y == 6

        obj3 = Ex.struct_validate({"x": 4})
        assert obj3.y == 8


class TestOrderAndEq:
    @staticmethod
    def assert_eq(a, b):
        assert a == b
        assert not a != b

    @staticmethod
    def assert_neq(a, b):
        assert a != b
        assert not a == b

    def test_order_no_eq_errors(self):
        with pytest.raises(ValueError, match="Cannot set eq=False and order=True"):

            class Test(Struct, order=True, eq=False):
                pass

    def test_struct_eq_false(self):
        class Point(Struct, eq=False):
            x: int
            y: int

        p = Point(1, 2)
        # identity based equality
        self.assert_eq(p, p)
        self.assert_neq(p, Point(1, 2))
        # identity based hash
        assert hash(p) == hash(p)
        assert hash(p) != hash(Point(1, 2))

    def test_struct_eq(self):
        class Test(Struct):
            a: int
            b: int

        class Test2(Test):
            pass

        x = Struct()

        self.assert_eq(x, Struct())
        self.assert_neq(x, None)

        x = Test(1, 2)
        self.assert_eq(x, Test(1, 2))
        self.assert_neq(x, None)
        self.assert_neq(x, Test(1, 3))
        self.assert_neq(x, Test(2, 2))
        self.assert_neq(x, Test2(1, 2))

    def test_struct_override_eq(self):
        class Ex(Struct):
            a: int
            b: int

            def __eq__(self, other):
                return self.a == other.a

        x = Ex(1, 2)
        y = Ex(1, 3)
        z = Ex(2, 3)

        self.assert_eq(x, y)
        self.assert_neq(x, z)

    def test_struct_eq_identity_fastpath(self):
        class Bad:
            def __eq__(self, other):
                raise ValueError("Oh no!")

        class Test(Struct):
            a: int
            b: Bad

        t = Test(1, Bad())
        self.assert_eq(t, t)

    @pytest.mark.parametrize("op", ["le", "lt", "ge", "gt"])
    def test_struct_order(self, op):
        func = getattr(operator, op)

        class Point(Struct, order=True):
            x: int
            y: int

        origin = Point(0, 0)
        for x in [-1, 0, 1]:
            for y in [-1, 0, 1]:
                sol = func((0, 0), (x, y))
                res = func(origin, Point(x, y))
                assert res == sol

        assert func(origin, origin) == func(1, 1)

    @pytest.mark.parametrize("eq, order", [(False, False), (True, False), (True, True)])
    def test_struct_compare_returns_notimplemented(self, eq, order):
        class Test(Struct, eq=eq, order=order):
            x: int

        t1 = Test(1)
        t2 = Test(2)
        assert t1.__eq__(t2) is (False if eq else NotImplemented)
        assert t1.__lt__(t2) is (True if order else NotImplemented)
        assert t1.__eq__(None) is NotImplemented
        assert t1.__lt__(None) is NotImplemented

    @pytest.mark.parametrize("op", ["eq", "ne", "le", "lt", "ge", "gt"])
    def test_struct_compare_errors(self, op):
        func = getattr(operator, op)

        class Bad:
            def __eq__(self, other):
                raise ValueError("Oh no!")

        class Test(Struct, order=True):
            a: object
            b: object

        t = Test(1, Bad())
        t2 = Test(1, 2)

        with pytest.raises(ValueError, match="Oh no!"):
            func(t, t2)
        with pytest.raises(ValueError, match="Oh no!"):
            func(t2, t)


class TestTagAndTagField:
    @pytest.mark.parametrize(
        "opts, tag_field, tag",
        [
            # Default & explicit NULL
            ({}, None, None),
            ({"tag": None, "tag_field": None}, None, None),
            # tag=True
            ({"tag": True}, "type", "Test"),
            ({"tag": True, "tag_field": "test"}, "test", "Test"),
            # tag=False
            ({"tag": False}, None, None),
            ({"tag": False, "tag_field": "kind"}, None, None),
            # tag str
            ({"tag": "test"}, "type", "test"),
            (dict(tag="test", tag_field="kind"), "kind", "test"),
            # tag int
            ({"tag": 1}, "type", 1),
            (dict(tag=1, tag_field="kind"), "kind", 1),
            # tag callable
            (dict(tag=lambda n: n.lower()), "type", "test"),
            (dict(tag=lambda n: n.lower(), tag_field="kind"), "kind", "test"),
            # tag_field alone
            (dict(tag_field="kind"), "kind", "Test"),
        ],
    )
    def test_config(self, opts, tag_field, tag):
        class Test(Struct, **opts):
            x: int
            y: int

        assert Test.__struct_config__.tag_field == tag_field
        assert Test.__struct_config__.tag == tag

    @pytest.mark.parametrize(
        "opts1, opts2, tag_field, tag",
        [
            # tag=True
            ({"tag": True}, {}, "type", "S2"),
            ({"tag": True}, {"tag": None}, "type", "S2"),
            ({"tag": True}, {"tag": False}, None, None),
            ({"tag": True}, {"tag_field": "foo"}, "foo", "S2"),
            # tag str
            ({"tag": "test"}, {}, "type", "test"),
            ({"tag": "test"}, {"tag": "test2"}, "type", "test2"),
            ({"tag": "test"}, {"tag": None}, "type", "test"),
            ({"tag": "test"}, {"tag_field": "foo"}, "foo", "test"),
            # tag int
            ({"tag": 1}, {}, "type", 1),
            ({"tag": 1}, {"tag": "test2"}, "type", "test2"),
            ({"tag": 1}, {"tag": None}, "type", 1),
            ({"tag": 1}, {"tag_field": "foo"}, "foo", 1),
            # tag callable
            ({"tag": lambda n: n.lower()}, {}, "type", "s2"),
            ({"tag": lambda n: n.lower()}, {"tag": False}, None, None),
            ({"tag": lambda n: n.lower()}, {"tag": None}, "type", "s2"),
            ({"tag": lambda n: n.lower()}, {"tag_field": "foo"}, "foo", "s2"),
        ],
    )
    def test_inheritance(self, opts1, opts2, tag_field, tag):
        class S1(Struct, **opts1):
            pass

        class S2(S1, **opts2):
            pass

        assert S2.__struct_config__.tag_field == tag_field
        assert S2.__struct_config__.tag == tag

    def test_tag_uses_simple_qualname(self):
        class S1(Struct, tag=True):
            class S2(Struct, tag=True):
                pass

        assert S1.__struct_config__.tag == "S1"
        assert S1.S2.__struct_config__.tag == "S1.S2"

        class S1(Struct, tag=str.lower):
            class S2(Struct, tag=str.lower):
                pass

        assert S1.__struct_config__.tag == "s1"
        assert S1.S2.__struct_config__.tag == "s1.s2"

    @pytest.mark.parametrize("tag", [b"bad", lambda n: b"bad"])
    def test_tag_wrong_type(self, tag):
        with pytest.raises(TypeError, match="`tag` must be a `str` or an `int`"):

            class Test(Struct, tag=tag):
                pass

    @pytest.mark.parametrize("tag", [-(2**63) - 1, 2**63])
    def test_tag_integer_out_of_range(self, tag):
        with pytest.raises(ValueError, match="Integer `tag` values must be"):

            class Test(Struct, tag=tag):
                pass

    def test_tag_field_wrong_type(self):
        with pytest.raises(TypeError, match="`tag_field` must be a `str`"):

            class Test(Struct, tag_field=b"bad"):
                pass

    def test_tag_field_collision(self):
        with pytest.raises(ValueError, match="tag_field='y'"):

            class Test(Struct, tag_field="y"):
                x: int
                y: int

    def test_tag_field_inheritance_collision(self):
        # Inherit the tag field
        class Base(Struct, tag_field="y"):
            pass

        with pytest.raises(ValueError, match="tag_field='y'"):

            class Test(Base):
                x: int
                y: int

        # Inherit the field
        class Base(Struct):
            x: int
            y: int

        with pytest.raises(ValueError, match="tag_field='y'"):

            class Test(Base, tag_field="y"):  # noqa
                pass


class TestRename:
    def test_field_name(self):
        class Test(Struct):
            x: Annotated[int, Field(alias="field_x")]

        assert Test.__struct_alias_fields__ == ("field_x",)

    def test_rename_mixed_with_field_name(self):
        class Test(Struct, rename="upper"):
            x: Annotated[int, Field(alias="field_x")]
            y: int

        assert Test.__struct_alias_fields__ == ("field_x", "Y")

    def test_rename_no_change(self):
        class Test(Struct, rename="lower"):
            x: int

        assert Test.__struct_fields__ is Test.__struct_alias_fields__

    def test_field_name_no_change(self):
        class Test(Struct):
            x: Annotated[int, Field(alias="x")]

        assert Test.__struct_fields__ is Test.__struct_alias_fields__

    def test_field_name_none(self):
        class Test(Struct):
            x: Annotated[int, Field(alias=None)]

        assert Test.__struct_fields__ is Test.__struct_alias_fields__

        class Test(Struct, rename="upper"):
            x: Annotated[int, Field(alias=None)]

        assert Test.__struct_alias_fields__ == ("X",)

    def test_rename_explicit_none(self):
        class Test(Struct, rename=None):
            field_one: int
            field_two: str

        assert Test.__struct_alias_fields__ == ("field_one", "field_two")
        assert Test.__struct_fields__ is Test.__struct_alias_fields__

    def test_rename_lower(self):
        class Test(Struct, rename="lower"):
            field_One: int
            field_Two: str

        assert Test.__struct_alias_fields__ == ("field_one", "field_two")

    def test_rename_upper(self):
        class Test(Struct, rename="upper"):
            field_one: int
            field_two: str

        assert Test.__struct_alias_fields__ == ("FIELD_ONE", "FIELD_TWO")

    def test_rename_kebab(self):
        class Test(Struct, rename="kebab"):
            field_one: int
            field_two_with_suffix: str
            __field_three__: bool
            field4: float
            _field_five: int

        assert Test.__struct_alias_fields__ == (
            "field-one",
            "field-two-with-suffix",
            "field-three",
            "field4",
            "field-five",
        )

    def test_rename_camel(self):
        class Test(Struct, rename="camel"):
            field_one: int
            field_two_with_suffix: str
            __field__three__: bool
            field4: float
            _field_five: int

        assert Test.__struct_alias_fields__ == (
            "fieldOne",
            "fieldTwoWithSuffix",
            "__fieldThree",
            "field4",
            "_fieldFive",
        )

    def test_rename_pascal(self):
        class Test(Struct, rename="pascal"):
            field_one: int
            field_two_with_suffix: str
            __field__three__: bool
            field4: float
            _field_five: int

        assert Test.__struct_alias_fields__ == (
            "FieldOne",
            "FieldTwoWithSuffix",
            "__FieldThree",
            "Field4",
            "_FieldFive",
        )

    def test_rename_callable(self):
        class Test(Struct, rename=str.title):
            field_one: int
            field_two: str

        assert Test.__struct_alias_fields__ == ("Field_One", "Field_Two")

    def test_rename_callable_returns_none(self):
        class Test(Struct, rename={"from_": "from"}.get):
            from_: str
            to: str

        assert Test.__struct_alias_fields__ == ("from", "to")

    def test_rename_callable_returns_non_string(self):
        with pytest.raises(
            TypeError,
            match="Expected calling `rename` to return a `str` or `None`, got `int`",
        ):

            class Test(Struct, rename=lambda x: 1):
                aa1: int
                aa2: int
                ab1: int

    def test_rename_mapping(self):
        class Test(Struct, rename={"from_": "from"}):
            from_: str
            to: str

        assert Test.__struct_alias_fields__ == ("from", "to")

    def test_rename_bad_value(self):
        with pytest.raises(ValueError, match="rename='invalid' is unsupported"):

            class Test(Struct, rename="invalid"):
                x: int

    def test_rename_bad_type(self):
        with pytest.raises(TypeError, match="str, callable, or mapping"):

            class Test(Struct, rename=1):
                x: int

    def test_rename_fields_collide(self):
        with pytest.raises(ValueError, match="Multiple fields rename to the same name"):

            class Test(Struct, rename=lambda x: x[:2]):
                aa1: int
                aa2: int
                ab1: int

    @pytest.mark.parametrize("field", ["foo\\bar", 'foo"bar', "foo\tbar"])
    def test_rename_field_invalid_characters(self, field):
        with pytest.raises(ValueError) as rec:

            class Test(Struct, rename=lambda x: field):
                x: int

        assert field in str(rec.value)
        assert "must not contain" in str(rec.value)

    def test_rename_inherit(self):
        class Base(Struct, rename="upper"):
            pass

        class Test1(Base):
            x: int

        assert Test1.__struct_alias_fields__ == ("X",)

        class Test2(Base, rename="camel"):
            my_field: int

        assert Test2.__struct_alias_fields__ == ("myField",)

        class Test3(Test2, rename="kebab"):
            my_other_field: int

        assert Test3.__struct_alias_fields__ == ("myField", "my-other-field")

        class Test4(Base, rename=None):
            my_field: int

        assert Test4.__struct_alias_fields__ == ("my_field",)

    def test_rename_fields_only_used_for_encode_and_decode(self):
        """Check that the renamed fields don't show up elsewhere"""

        class Test(Struct, rename="upper"):
            one: int
            two: str

        t = Test(one=1, two="test")
        assert t.one == 1
        assert t.two == "test"
        assert repr(t) == "Test(one=1, two='test')"
        with pytest.raises(TypeError, match="Missing required argument 'two'"):
            Test(one=1)


@pytest.fixture
def replace():
    return copy_replace


class TestReplace:
    def test_replace_not_a_struct(self):
        with pytest.raises((TypeError, AttributeError)):
            copy_replace(1, x=3)

    def test_replace_no_kwargs(self, replace):
        p = Point(1, 2)
        assert replace(p) == p

    def test_replace_kwargs(self, replace):
        p = Point(1, 2)
        assert replace(p, x=3) == Point(3, 2)
        assert replace(p, y=4) == Point(1, 4)
        assert replace(p, x=3, y=4) == Point(3, 4)

    def test_replace_unknown_field(self, replace):
        p = Point(1, 2)
        with pytest.raises(TypeError, match="`Point` has no field 'oops'"):
            replace(p, oops=3)

    def test_replace_errors_unset_fields(self, replace):
        p = Point(1, 2)
        del p.x

        with pytest.raises(AttributeError, match="Struct field 'x' is unset"):
            replace(p)

        with pytest.raises(AttributeError, match="Struct field 'x' is unset"):
            replace(p, y=1)

        assert replace(p, x=3) == Point(3, 2)

    def test_replace_frozen(self, replace):
        class Test(structtype.Struct, frozen=True):
            x: int
            y: int

        assert replace(Test(1, 2), x=3) == Test(3, 2)

    def test_replace_gc_delayed_tracking(self, replace):
        class Test(structtype.Struct):
            x: int
            y: list[int] | None

        obj = Test(1, None)
        assert not gc.is_tracked(replace(obj))
        assert not gc.is_tracked(replace(obj, x=10))
        assert not gc.is_tracked(replace(obj, y=None))
        assert gc.is_tracked(replace(obj, y=[1, 2, 3]))

        obj = Test(1, [1, 2, 3])
        assert gc.is_tracked(replace(obj))
        assert gc.is_tracked(replace(obj, x=1))
        assert not gc.is_tracked(replace(obj, y=None))

    def test_replace_reference_counts(self, replace):
        class Test(structtype.Struct):
            x: Any
            y: int

        x = object()
        t = Test(x, 1)

        x_count = sys.getrefcount(x)

        t2 = replace(t)
        assert sys.getrefcount(x) == x_count + 1
        del t2

        t2 = replace(t, x=None)
        assert sys.getrefcount(x) == x_count
        del t2

        t2 = replace(t, y=2)
        assert sys.getrefcount(x) == x_count + 1
        del t2

        x2 = object()
        x2_count = sys.getrefcount(x2)
        t2 = replace(t, x=x2)
        assert sys.getrefcount(x) == x_count
        assert sys.getrefcount(x2) == x2_count + 1
        del t2

    def test_replace_calls_post_init(self, replace):
        count = 0

        class Ex(Struct):
            def __post_init__(self):
                nonlocal count
                count += 1

        x1 = Ex()
        assert count == 1
        x2 = replace(x1)
        assert x1 == x2


class TestInspectFields:
    def test_fields_bad_arg(self):
        T = TypeVar("T")

        class Bad(Generic[T]):
            x: T

        for val in [1, int, Bad, Bad[int]]:
            with pytest.raises(TypeError, match="struct type or instance"):
                structtype.fields(val)

    def test_fields_struct_meta(self):
        class CustomMeta(structtype.StructMeta):
            pass

        class Base(metaclass=CustomMeta):
            pass

        class Model(Base):
            pass

        assert structtype.fields(Model) == ()

    def test_fields_struct_meta_instance(self):
        class CustomMeta(structtype.StructMeta):
            pass

        class Base(metaclass=CustomMeta):
            pass

        class Model(Base):
            pass

        assert structtype.fields(Model()) == ()

    def test_fields_no_fields(self):
        assert structtype.fields(structtype.Struct) == ()

    @pytest.mark.parametrize("instance", [False, True])
    def test_fields(self, instance):
        def factory():
            return 1

        class Example(structtype.Struct):
            x: int
            y: int = 0
            z: int = structtype.Factory(factory)

        arg = Example(1, 2, 3) if instance else Example
        fields = structtype.fields(arg)
        x_field, y_field, z_field = fields

        assert x_field.required
        assert x_field.default is NODEFAULT
        assert x_field.default_factory is NODEFAULT

        assert not y_field.required
        assert y_field.default == 0
        assert y_field.default_factory is NODEFAULT

        assert not z_field.required
        assert z_field.default is NODEFAULT
        assert z_field.default_factory is factory

    def test_fields_keyword_only(self):
        class Example(structtype.Struct, kw_only=True):
            a: int
            b: int = 1
            c: int
            d: int = 2

        sol = (
            structtype._inspect.FieldInfo("a", "a", int),
            structtype._inspect.FieldInfo("b", "b", int, default=1),
            structtype._inspect.FieldInfo("c", "c", int),
            structtype._inspect.FieldInfo("d", "d", int, default=2),
        )
        assert structtype.fields(Example) == sol

    def test_fields_alias(self):
        class Example(structtype.Struct, rename="camel"):
            field_one: int
            field_two: int

        sol = (
            structtype._inspect.FieldInfo("field_one", "fieldOne", int),
            structtype._inspect.FieldInfo("field_two", "fieldTwo", int),
        )

        assert structtype.fields(Example) == sol

    def test_fields_generic(self):
        T = TypeVar("T")

        class Example(structtype.Struct, Generic[T]):
            x: T
            y: int

        sol = (
            structtype._inspect.FieldInfo("x", "x", T),
            structtype._inspect.FieldInfo("y", "y", int),
        )
        assert structtype.fields(Example) == sol
        assert structtype.fields(Example(1, 2)) == sol

        sol = (
            structtype._inspect.FieldInfo("x", "x", str),
            structtype._inspect.FieldInfo("y", "y", int),
        )
        assert structtype.fields(Example[str])


class TestClassVar:
    def case1(self):
        return """
        from typing import ClassVar
        from structtype import Struct

        class Ex(Struct):
            a: int
            cv1: ClassVar
            b: int
            cv2: ClassVar[int] = 1
        """

    def case2(self):
        return """
        import typing
        from structtype import Struct

        class Ex(Struct):
            a: int
            cv1: typing.ClassVar
            b: int
            cv2: typing.ClassVar[int] = 1
        """

    def case3(self):
        return """
        import typing
        from structtype import Struct

        ClassVar = typing.List

        class Ex(Struct):
            a: ClassVar
            b: ClassVar[int]
            cv2: typing.ClassVar[int] = 1
        """

    def case4(self):
        return """
        from typing import ClassVar, List
        from structtype import Struct

        class typing:
            ClassVar = List

        class Ex(Struct):
            a: typing.ClassVar
            b: typing.ClassVar[int]
            cv2: ClassVar[int] = 1
        """

    def case5(self):
        """Annotations that start with `ClassVar`/`typing.ClassVar` but don't
        end there aren't treated as false-positives"""
        return """
        from typing import ClassVar, List
        from structtype import Struct

        ClassVariable = List

        class typing:
            ClassVariable = List

        class Ex(Struct):
            a: typing.ClassVariable
            b: ClassVariable[int]
            cv2: ClassVar[int] = 1
        """

    @pytest.mark.parametrize("case", [1, 2, 3, 4, 5])
    @pytest.mark.parametrize("future_annotations", [True, False])
    def test_classvar(self, case, future_annotations):
        source = getattr(self, f"case{case}")()
        if future_annotations:
            source = "from __future__ import annotations\n" + textwrap.dedent(source)
        with temp_module(source) as mod:
            assert mod.Ex.__struct_fields__ == ("a", "b")
            assert not hasattr(mod.Ex, "cv1")
            assert mod.Ex.cv2 == 1

    def test_wrong_classvar(self):
        # See https://github.com/tds333/structtype/issues/1096
        source = """
        from __future__ import annotations
        from structtype import Struct

        class typing:
            pass

        class Ex(Struct):
            a: typing.ClassVar
        """

        with pytest.raises(
            AttributeError,
            match="'typing' has no attribute 'ClassVar'",
        ):
            temp_module(source).__enter__()  # It used to crash, but must not!


class TestPostInit:
    def test_post_init(self):
        called = False
        singleton = object()

        class Ex(Struct):
            x: int

            def __post_init__(self):
                nonlocal called
                called = True
                return singleton

        Ex(1)
        assert called
        # Return value is decref'd
        assert sys.getrefcount(singleton) <= 2  # 1 for ref, 1 for call

    def test_post_init_errors(self):
        class Ex(Struct):
            x: int

            def __post_init__(self):
                raise ValueError("Oh no!")

        with pytest.raises(ValueError, match="Oh no!"):
            Ex(1)

    def test_post_init_invalid(self):
        class Bad1(Struct):
            __post_init__ = 1

        class Bad2(Struct):
            def __post_init__(self, other):
                pass

        with pytest.raises(TypeError):
            Bad1()

        with pytest.raises(TypeError):
            Bad2()

    def test_post_init_inheritance(self):
        called = False

        class Base:
            def __post_init__(self):
                nonlocal called
                called = True

        class Ex(Struct, Base):
            x: int

        Ex(1)
        assert called

    def test_post_init_not_called_on_copy(self):
        count = 0

        class Ex(Struct):
            def __post_init__(self):
                nonlocal count
                count += 1

        x1 = Ex()
        assert count == 1
        x2 = x1.__copy__()
        assert x1 == x2
        assert count == 1
