"""Tests for the exposed StructMeta metaclass."""

import gc
import re
import secrets
from abc import ABCMeta, _abc_init, abstractmethod

import pytest

import structtype
from structtype import Struct, StructMeta
from structtype._core import _json_decode, _json_encode
from structtype import StructConfig


def test_class_body_struct_config():
    """Test that struct_config in class body is applied."""
    class S(Struct):
        struct_config = StructConfig(frozen=True)
        x: int

    s = S(x=1)
    assert s.x == 1
    with pytest.raises(AttributeError):
        s.x = 2


def test_struct_config_merge_inheritance():
    """Test that child inherits parent config and can override specific options."""
    class Base(Struct):
        struct_config = StructConfig(frozen=True, tag="base")
        x: int

    class Child(Base):
        struct_config = StructConfig(eq=False)

    assert Child.struct_config["frozen"] is True
    assert Child.struct_config["tag"] == "base"
    assert Child.struct_config["eq"] is False


def test_kw_only_inherits_to_new_fields():
    """Test that kw_only from parent applies to child's new fields."""
    class Base(Struct):
        struct_config = StructConfig(kw_only=True)
        x: int

    class Child(Base):
        y: int

    c = Child(x=1, y=2)
    assert c.x == 1
    assert c.y == 2
    with pytest.raises(TypeError):
        Child(1, 2)


def test_empty_struct_config_is_noop():
    """Test that child with StructConfig() inherits all from parent."""
    class Base(Struct):
        struct_config = StructConfig(frozen=True)
        x: int

    class Child(Base):
        struct_config = StructConfig()

    assert Child.struct_config["frozen"] is True


def test_class_kwargs_silently_ignored():
    """Class-statement kwargs are silently ignored (no TypeError)."""
    class Bad(Struct, frozen=True, tag="nope"):
        x: int

    assert Bad.__struct_config__["frozen"] is False
    assert Bad.__struct_config__["tag"] is None
    assert Bad(1).x == 1


def test_custom_metaclass_can_intercept_kwargs():
    """A custom metaclass can intercept class-statement kwargs."""
    intercepted = []

    class InterceptMeta(StructMeta):
        def __new__(mcls, name, bases, namespace, **kwargs):
            intercepted.extend(kwargs.items())
            return super().__new__(mcls, name, bases, namespace)

    class Base(Struct, metaclass=InterceptMeta):
        x: int = 1

    class Child(Base, my_option=42):
        pass

    assert ("my_option", 42) in intercepted


def test_struct_config_must_be_dict():
    """Test that passing a non-dict as struct_config raises TypeError."""
    with pytest.raises(TypeError, match="struct_config must be a dict"):
        class Bad(Struct):
            struct_config = "not a dict"


def test_custom_meta_injects_struct_config():
    """Test that a custom metaclass can use dict merge to modify struct_config."""
    class KwOnlyMeta(StructMeta):
        def __new__(mcls, name, bases, namespace, **kwargs):
            cfg = namespace.get("struct_config", StructConfig())
            namespace["struct_config"] = {**cfg, "kw_only": True}
            return super().__new__(mcls, name, bases, namespace)

    class KwOnlyBase(Struct, metaclass=KwOnlyMeta):
        struct_config = StructConfig()

    class Child(KwOnlyBase):
        x: int

    with pytest.raises(TypeError):
        Child(1)


def test_struct_meta_exists():
    """Test that StructMeta is properly exposed."""
    assert hasattr(structtype, "StructMeta")
    assert isinstance(Struct, StructMeta)
    assert issubclass(StructMeta, type)


def test_struct_meta_direct_usage():
    """Test that StructMeta can be used directly as a metaclass."""

    class CustomStruct(metaclass=StructMeta):
        x: int
        y: str

    # Verify the struct works as expected
    instance = CustomStruct(x=1, y="test")
    assert instance.x == 1
    assert instance.y == "test"
    assert isinstance(instance, CustomStruct)
    assert isinstance(CustomStruct, StructMeta)


def test_struct_meta_options():
    """Test that StructMeta properly handles struct options."""

    class CustomStruct(metaclass=StructMeta):
        struct_config = StructConfig(frozen=True)
        x: int

    # Verify options were applied
    instance = CustomStruct(x=1)
    with pytest.raises(AttributeError):
        instance.x = 2  # Should be frozen


def test_struct_meta_field_processing():
    """Test that StructMeta properly processes fields."""

    class CustomStruct(metaclass=StructMeta):
        x: int
        y: str = "default"

    # Verify struct functionality
    instance = CustomStruct(x=1)
    assert instance.x == 1
    assert instance.y == "default"

    # Check struct metadata
    assert hasattr(CustomStruct, "__struct_fields__")
    assert "x" in CustomStruct.__struct_fields__
    assert "y" in CustomStruct.__struct_fields__


def test_struct_meta_with_struct_base():
    """Test using StructMeta with Struct as a base class."""

    class CustomStruct(Struct):
        x: int
        y: str

    # Verify the struct works as expected
    instance = CustomStruct(x=1, y="test")
    assert instance.x == 1
    assert instance.y == "test"
    assert isinstance(instance, CustomStruct)
    assert isinstance(CustomStruct, StructMeta)


def test_struct_meta_validation():
    """Test that StructMeta validation works."""
    # Should raise TypeError for invalid field name
    with pytest.raises(TypeError):

        class InvalidStruct(metaclass=StructMeta):
            __dict__: int  # __dict__ is a reserved name


def test_struct_meta_with_options():
    """Test StructMeta with various options."""

    class Point(metaclass=StructMeta):
        struct_config = StructConfig(frozen=True, eq=True, order=True)
        x: int
        y: int

    p1 = Point(x=1, y=2)
    p2 = Point(x=1, y=3)

    # Test frozen
    with pytest.raises(AttributeError):
        p1.x = 10

    # Test eq - note that we need to compare fields manually
    # since equality is based on identity by default
    assert p1.x == Point(x=1, y=2).x and p1.y == Point(x=1, y=2).y
    assert p1.x == p2.x and p1.y != p2.y

    # Test order - we can't directly compare instances
    # but we can compare their field values
    assert (p1.x, p1.y) < (p2.x, p2.y)


def test_struct_meta_inheritance():
    """Test that StructMeta can be inherited in Python code."""

    class CustomMeta(StructMeta):
        """A custom metaclass that inherits from StructMeta.

        This metaclass demonstrates injecting struct_config via namespace.
        """

        _kw_only_default_settings = {}

        def __new__(mcls, name, bases, namespace, **kwargs):
            # Check for kw_only_default in kwargs (custom kwarg)
            kw_only_default = kwargs.pop("kw_only_default", None)

            if kw_only_default is not None:
                mcls._kw_only_default_settings[name] = kw_only_default
            else:
                for base in bases:
                    base_name = base.__name__
                    if base_name in mcls._kw_only_default_settings:
                        kw_only_default = mcls._kw_only_default_settings[base_name]
                        break

                # Inject kw_only into struct_config via namespace
                if kw_only_default is not None:
                    cfg = namespace.get("struct_config", StructConfig())
                    if "kw_only" not in cfg:
                        namespace["struct_config"] = {**cfg, "kw_only": kw_only_default}

            return super().__new__(mcls, name, bases, namespace, **kwargs)

    # Test basic functionality - without kw_only_default
    class SimpleModel(metaclass=CustomMeta):
        x: int
        y: str

    # Verify the class was created correctly
    assert isinstance(SimpleModel, CustomMeta)
    assert issubclass(CustomMeta, StructMeta)

    # Test creating an instance with positional arguments (should work)
    instance = SimpleModel(1, "test")
    assert instance.x == 1
    assert instance.y == "test"

    # Test setting kw_only_default=True via custom kwarg
    class KwOnlyBase(metaclass=CustomMeta, kw_only_default=True):
        struct_config = StructConfig()
        """Base class that sets kw_only_default=True"""

    # Test a simple child class, should inherit kw_only_default
    class SimpleChild(KwOnlyBase):
        x: int

    # Should only allow keyword arguments
    with pytest.raises(TypeError):
        SimpleChild(1)

    class BadFieldOrder(KwOnlyBase):
        x: int = 0
        y: int

    BadFieldOrder(y=10)

    # Create instance with keyword arguments
    child = SimpleChild(x=1)
    assert child.x == 1

    # Test overriding inherited kw_only_default with explicit struct_config
    class NonKwOnlyChild(KwOnlyBase):
        struct_config = StructConfig(kw_only=False)
        x: int

    # Should allow positional arguments
    non_kw_child = NonKwOnlyChild(1)
    assert non_kw_child.x == 1

    # Test independent class, not inheriting kw_only_default
    class IndependentModel(metaclass=CustomMeta):
        x: int
        y: str

    # Should allow positional arguments
    independent = IndependentModel(1, "test")
    assert independent.x == 1
    assert independent.y == "test"

    # Test that kw_only_default values are correctly passed
    assert "KwOnlyBase" in CustomMeta._kw_only_default_settings
    assert CustomMeta._kw_only_default_settings["KwOnlyBase"] is True

def test_struct_meta_subclass_functions():
    """Test if structs created by StructMeta subclasses support various function operations."""

    # Define a custom metaclass
    class CustomMeta(StructMeta):
        """Custom metaclass that inherits from StructMeta"""

    # Use the custom metaclass to create a struct class
    class CustomStruct(metaclass=CustomMeta):
        x: int
        y: str
        z: float = 3.14

    # Create an instance
    obj = CustomStruct(x=1, y="test")
    assert obj.x == 1
    assert obj.y == "test"
    assert obj.z == 3.14

    # Test nested structs
    class NestedStruct(metaclass=CustomMeta):
        inner: CustomStruct
        name: str

    nested = NestedStruct(inner=obj, name="nested")
    assert nested.inner.x == 1
    assert nested.inner.y == "test"
    assert nested.name == "nested"


def test_struct_meta_subclass_inheritance():
    """Test multi-level inheritance of StructMeta subclasses."""

    # Define the first level custom metaclass
    class BaseMeta(StructMeta):
        """Base custom metaclass"""

    # Define the second level custom metaclass
    class DerivedMeta(BaseMeta):
        """Derived custom metaclass"""

    # Use the second level custom metaclass to create a struct class
    class DerivedStruct(metaclass=DerivedMeta):
        a: int
        b: str

    # Create an instance
    obj = DerivedStruct(a=42, b="derived")
    assert obj.a == 42
    assert obj.b == "derived"


def test_struct_meta_subclass_with_encoder():
    """Test compatibility of structs created by StructMeta subclasses with encoders."""

    # Define a custom metaclass
    class EncoderMeta(StructMeta):
        """Custom metaclass for testing encoders"""

    # Use the custom metaclass to create a struct class
    class EncoderStruct(metaclass=EncoderMeta):
        id: int
        name: str
        tags: list[str] = []

    # Create an instance
    obj = EncoderStruct(id=123, name="test")

    # Test JSON encoding and decoding
    json_bytes = _json_encode(obj)
    decoded = _json_decode(json_bytes, type=EncoderStruct)

    assert decoded.id == 123
    assert decoded.name == "test"
    assert decoded.tags == []

    # Test encoding and decoding with nested structs
    class Container(metaclass=EncoderMeta):
        item: EncoderStruct
        count: int

    container = Container(item=obj, count=1)
    json_bytes = _json_encode(container)
    decoded = _json_decode(json_bytes, type=Container)

    assert decoded.count == 1
    assert decoded.item.id == 123
    assert decoded.item.name == "test"


def test_structmeta_abcmeta_mixed_behaves_like_abc():
    class IntegerStructMeta(StructMeta, ABCMeta):
        pass

    class IntegerStructBase(Struct, metaclass=IntegerStructMeta):
        @abstractmethod
        def to_integer(self) -> int: ...

        @classmethod
        @abstractmethod
        def from_integer(cls, val: int) -> "IntegerStructBase": ...

    class ConcreteIntStruct(IntegerStructBase):
        val: int

        def to_integer(self) -> int:
            return self.val << 2

        @classmethod
        def from_integer(cls, val: int) -> "ConcreteIntStruct":
            return cls(val)

    # Abstract base cannot be instantiated when there are abstract methods
    with pytest.raises(
        TypeError,
        match=(
            r"^Can't instantiate abstract class IntegerStructBase without an "
            r"implementation for abstract methods 'from_integer', 'to_integer'$"
        ),
    ):
        IntegerStructBase()

    # Concrete subclass is fine when all abstract methods are implemented
    obj = ConcreteIntStruct(1)
    assert obj.to_integer() == 4

    # ABC semantics: issubclass / isinstance must work and not raise
    assert issubclass(ConcreteIntStruct, IntegerStructBase)
    assert isinstance(obj, IntegerStructBase)

    # structtype roundtrip still works
    encoded = _json_encode(obj)
    decoded = _json_decode(encoded, type=ConcreteIntStruct)
    assert decoded == obj

    # Repeated checks must continue working (no latent _abc_impl issues)
    for _ in range(5):
        assert issubclass(ConcreteIntStruct, IntegerStructBase)
        assert isinstance(obj, IntegerStructBase)


def test_structmeta_abcmeta_intermediate_still_abstract_and_message():
    class IntegerStructMeta(StructMeta, ABCMeta):
        pass

    class IntegerStructBase(Struct, metaclass=IntegerStructMeta):
        @abstractmethod
        def to_integer(self) -> int: ...

        @classmethod
        @abstractmethod
        def from_integer(cls, val: int) -> "IntegerStructBase": ...

    class Intermediate(IntegerStructBase):
        # Implement only one of the abstract methods
        @classmethod
        def from_integer(cls, val: int) -> "Intermediate":
            return cls()

    # Intermediate remains abstract: only to_integer is missing
    with pytest.raises(
        TypeError,
        match=(
            r"^Can't instantiate abstract class Intermediate without an "
            r"implementation for abstract method 'to_integer'$"
        ),
    ):
        Intermediate()


def test_structmeta_abcmeta_single_abstract_method_message():
    class IntegerStructMeta(StructMeta, ABCMeta):
        pass

    class SingleAbstract(Struct, metaclass=IntegerStructMeta):
        @abstractmethod
        def only(self) -> int: ...

    with pytest.raises(
        TypeError,
        match=(
            r"^Can't instantiate abstract class SingleAbstract without an "
            r"implementation for abstract method 'only'$"
        ),
    ):
        SingleAbstract()


def test_structmeta_abcmeta_mixed_reverse_order():
    class IntegerStructMeta(ABCMeta, StructMeta):
        pass

    class IntegerStructBase(Struct, metaclass=IntegerStructMeta):
        @abstractmethod
        def to_integer(self) -> int: ...

        @classmethod
        @abstractmethod
        def from_integer(cls, val: int) -> "IntegerStructBase": ...

    class ConcreteIntStruct(IntegerStructBase):
        val: int

        def to_integer(self) -> int:
            return self.val + 1

        @classmethod
        def from_integer(cls, val: int) -> "ConcreteIntStruct":
            return cls(val)

    obj = ConcreteIntStruct(10)

    assert issubclass(ConcreteIntStruct, IntegerStructBase)
    assert isinstance(obj, IntegerStructBase)

    encoded = _json_encode(obj)
    decoded = _json_decode(encoded, type=ConcreteIntStruct)
    assert decoded == obj


def test_structmeta_abcmeta_mixed_supports_register():
    class IntegerStructMeta(StructMeta, ABCMeta):
        pass

    class IntegerStructBase(Struct, metaclass=IntegerStructMeta):
        @abstractmethod
        def to_integer(self) -> int: ...

        @classmethod
        @abstractmethod
        def from_integer(cls, val: int) -> "IntegerStructBase": ...

    class OtherStruct(Struct):
        val: int

    # Register a non-subclass as a virtual subclass
    IntegerStructBase.register(OtherStruct)

    other = OtherStruct(5)

    # Virtual subclassing should work
    assert issubclass(OtherStruct, IntegerStructBase)
    assert isinstance(other, IntegerStructBase)

    # structtype usage should still be fine
    encoded = _json_encode(other)
    decoded = _json_decode(encoded, type=OtherStruct)
    assert decoded == other


def test_plain_struct_not_treated_as_abc():
    class Plain(Struct):
        x: int

    obj = Plain(1)

    # Normal structtype behaviour works
    encoded = _json_encode(obj)
    decoded = _json_decode(encoded, type=Plain)
    assert decoded == obj

    # Sanity: Plain should not suddenly be an ABC
    # (we don't rely on _abc_impl directly, but this is a cheap guard)
    assert not any(
        base.__module__ == "abc" and base.__name__ == "ABC" for base in Plain.__mro__
    )


def test_structmeta_abcmeta_mixed_nested_subclass():
    class IntegerStructMeta(StructMeta, ABCMeta):
        pass

    class IntegerStructBase(Struct, metaclass=IntegerStructMeta):
        @abstractmethod
        def to_integer(self) -> int: ...

        @classmethod
        @abstractmethod
        def from_integer(cls, val: int) -> "IntegerStructBase": ...

    class Intermediate(IntegerStructBase):
        @classmethod
        def from_integer(cls, val: int) -> "Intermediate":
            return cls()

    class Concrete(Intermediate):
        val: int

        def to_integer(self) -> int:
            return self.val

        @classmethod
        def from_integer(cls, val: int) -> "Concrete":
            return cls(val)

    obj = Concrete(7)

    assert issubclass(Concrete, IntegerStructBase)
    assert isinstance(obj, IntegerStructBase)
    assert isinstance(obj, Intermediate)


def test_structmeta_abcmeta_with_no_abstract_methods_is_concrete():
    class IntegerStructMeta(StructMeta, ABCMeta):
        pass

    class ConcreteBase(Struct, metaclass=IntegerStructMeta):
        # no @abstractmethod
        def foo(self) -> int:
            return 1

    # Should be instantiable (no TypeError)
    obj = ConcreteBase()
    assert obj.foo() == 1

    # And should not be considered abstract
    assert getattr(ConcreteBase, "__abstractmethods__", frozenset()) in (
        frozenset(),
        set(),
    )


def test_struct_abc_via_init_subclass_and__abc_init():
    class ABCStruct(Struct):
        def __init_subclass__(cls, **kwargs):
            super().__init_subclass__(**kwargs)
            _abc_init(cls)

    class Base(ABCStruct):
        @abstractmethod
        def foo(self) -> int: ...

    # Base is abstract; instantiation should fail
    with pytest.raises(
        TypeError,
        match=r"Can't instantiate abstract class Base without an implementation for abstract method 'foo'",
    ):
        Base()

    class Concrete(Base):
        x: int

        def foo(self) -> int:
            return self.x

    c = Concrete(5)
    assert c.foo() == 5


def test_struct_meta_pattern_ref_leak():
    # ensure that we're not keeping around references to re.Pattern longer than necessary
    # see https://github.com/tds333/structtype/pull/899 for details

    # clear cache to get a baseline
    re.purge()

    # use a random string to create a pattern, to ensure there can never be an overlap
    # with any cached pattern
    pattern_string = secrets.token_hex()
    structtype.StrValidator(pattern=pattern_string)
    # purge cache and gc again
    re.purge()
    gc.collect()
    # there shouldn't be an re.Pattern with our pattern any more. if there is, it's
    # being kept alive by some reference
    assert not any(
        o
        for o in gc.get_objects()
        if isinstance(o, re.Pattern) and o.pattern == pattern_string
    )


def test_struct_config_spec_constructor_and_unsets():
    cfg = StructConfig(frozen=True, order=True, tag="x", kw_only=True, rename="camel")
    assert cfg["frozen"] is True
    assert cfg["order"] is True
    assert cfg["tag"] == "x"
    assert cfg["kw_only"] is True
    assert cfg["rename"] == "camel"
    assert "eq" not in cfg
    assert "validate_on_init" not in cfg
    assert "forbid_unknown_fields" not in cfg
    assert "omit_defaults" not in cfg
    assert "repr_omit_defaults" not in cfg
    assert "array_like" not in cfg
    assert "weakref" not in cfg
    assert "dict" not in cfg
    assert "cache_hash" not in cfg
    assert "tag_field" not in cfg


def test_struct_config_dict_merge():
    cfg = {**StructConfig(frozen=True), "order": True}
    assert cfg["frozen"] is True
    assert cfg["order"] is True
    assert "eq" not in cfg

    cfg2 = {**StructConfig(frozen=True, tag="a"), "tag": "b", "order": True}
    assert cfg2["frozen"] is True
    assert cfg2["tag"] == "b"
    assert cfg2["order"] is True


def test_struct_config_class_attribute_matches_view():
    class S(Struct):
        struct_config = StructConfig(frozen=True)
        x: int

    assert isinstance(S.struct_config, dict)
    assert S.struct_config["frozen"] is True
    assert S(1).struct_config["frozen"] is True
    assert isinstance(S(1).struct_config, dict)


def test_struct_config_view_exposes_rename():
    class S(Struct):
        struct_config = StructConfig(rename="camel")
        my_field: int

    cfg = S.__struct_config__
    assert cfg["rename"] == "camel"
