import enum
from collections.abc import Callable, Iterable, Iterator, Mapping
from inspect import Signature
from typing import (
    Any,
    ClassVar,
    Final,
    Literal,
    Optional,
    Tuple,
    Type,
    TypeVar,
    Union,
    final,
    overload,
)

from typing_extensions import Buffer, Self, TypeAlias, dataclass_transform

from . import _inspect

# PEP 673 explicitly rejects using Self in metaclass definitions:
# https://peps.python.org/pep-0673/#valid-locations-for-self
#
# Typeshed works around this by using a type variable as well:
# https://github.com/python/typeshed/blob/17bde1bd5e556de001adde3c2f340ba1c3581bd2/stdlib/abc.pyi#L14-L19
_SM = TypeVar("_SM", bound="StructMeta")

class StructMeta(type):
    __struct_fields__: ClassVar[tuple[str, ...]]
    __struct_defaults__: ClassVar[tuple[Any, ...]]
    __struct_encode_fields__: ClassVar[tuple[str, ...]]
    __match_args__: ClassVar[tuple[str, ...]]
    @property
    def __signature__(self) -> Signature: ...
    @property
    def __struct_config__(self) -> structs.StructConfig: ...
    def __new__(
        mcls: type[_SM],
        name: str,
        bases: tuple[type, ...],
        namespace: dict[str, Any],
        /,
        *,
        tag: bool | str | int | Callable[[str], str | int] | None = None,
        tag_field: str | None = None,
        rename: (
            None
            | Literal["lower", "upper", "camel", "pascal", "kebab"]
            | Callable[[str], str | None]
            | Mapping[str, str]
        ) = None,
        omit_defaults: bool = False,
        forbid_unknown_fields: bool = False,
        frozen: bool = False,
        eq: bool = True,
        order: bool = False,
        kw_only: bool = False,
        repr_omit_defaults: bool = False,
        array_like: bool = False,
        gc: bool = True,
        weakref: bool = False,
        dict: bool = False,
        cache_hash: bool = False,
        validate_on_init: bool = False,
    ) -> _SM: ...

_T = TypeVar("_T")

@final
class UnsetType(enum.Enum):
    UNSET = "UNSET"
    def __bool__(self) -> Literal[False]: ...

UNSET: Final = UnsetType.UNSET

@final
class _NoDefault(enum.Enum):
    NODEFAULT = "NODEFAULT"

NODEFAULT: Final = _NoDefault.NODEFAULT

@dataclass_transform(field_specifiers=(Field,))
class Struct(metaclass=StructMeta):
    __struct_fields__: ClassVar[tuple[str, ...]]
    __struct_config__: ClassVar[StructConfig]
    __struct_encode_fields__: ClassVar[tuple[str, ...]]
    __struct_defaults__: ClassVar[tuple[Any, ...]]
    __match_args__: ClassVar[tuple[str, ...]]
    # A default __init__ so that Structs with unknown field types
    # won't error on every call to `__init__`
    def __init__(self, *args: Any, **kwargs: Any) -> None: ...
    def __init_subclass__(
        cls,
        tag: bool | str | int | Callable[[str], str | int] | None = None,
        tag_field: str | None = None,
        rename: (
            None
            | Literal["lower", "upper", "camel", "pascal", "kebab"]
            | Callable[[str], str | None]
            | Mapping[str, str]
        ) = None,
        omit_defaults: bool = False,
        forbid_unknown_fields: bool = False,
        frozen: bool = False,
        eq: bool = True,
        order: bool = False,
        kw_only: bool = False,
        repr_omit_defaults: bool = False,
        array_like: bool = False,
        gc: bool = True,
        weakref: bool = False,
        dict: bool = False,
        cache_hash: bool = False,
        validate_on_init: bool = False,
    ) -> None: ...
    def __rich_repr__(self) -> list[tuple[str, Any]]: ...
    def __copy__(self) -> Self: ...
    def __reduce__(self) -> tuple: ...
    def __replace__(self, **changes: Any) -> Self: ...
    def __iter__(self) -> Iterator[tuple[str, Any]]: ...
    def struct_dump_json(
        self,
        *,
        enc_hook: Callable[[Any], Any] | None = None,
        decimal_format: Literal["string", "number"]
        | Callable[[Any], Any]
        | None = None,
        uuid_format: Literal["canonical", "hex"] | None = None,
        order: Literal["deterministic", "sorted"] | None = None,
    ) -> bytes: ...
    def struct_dump(
        self,
        *,
        enc_hook: Callable[[Any], Any] | None = None,
        order: Literal["deterministic", "sorted"] | None = None,
        str_keys: bool = False,
        builtin_types: Iterable[type] | None = None,
    ) -> dict[str, Any] | list[Any]: ...
    def struct_force_setattr(self, name: str, value: Any) -> None: ...
    def struct_validate_self(self) -> None: ...
    @classmethod
    def struct_validate_json(
        cls: type[_T],
        buf: str | Buffer,
        *,
        strict: bool = True,
        dec_hook: Callable[[type[Any], Any], Any] | None = None,
    ) -> _T: ...
    @classmethod
    def struct_validate(
        cls: type[_T],
        obj: Any,
        *,
        strict: bool = True,
        from_attributes: bool = False,
        dec_hook: Callable[[type[Any], Any], Any] | None = None,
    ) -> _T: ...

# Lie and say `Raw` is a subclass of `bytes`, so mypy will accept it in most
# places where an object that implements the buffer protocol is valid
@final
class Raw(bytes):
    @overload
    def __new__(cls) -> "Raw": ...
    @overload
    def __new__(cls, msg: Buffer | str) -> "Raw": ...
    def copy(self) -> "Raw": ...

#: We can't represent this in types, only via a name:
_NonNegativeInt: TypeAlias = int

@final
class Field:
    # Numeric:
    @overload
    def __init__(
        self,
        *,
        gt: int | float | None = None,
        lt: int | float | None = None,
        multiple_of: int | float | None = None,
        default: Any = NODEFAULT,
        default_factory: Callable[[], Any] | None = None,
        alias: str | None = None,
        title: str | None = None,
        description: str | None = None,
        json_schema_extra: dict[str, Any] | None = None,
        examples: list[Any] | None = None,
    ) -> None: ...
    @overload
    def __init__(
        self,
        *,
        gt: int | float | None = None,
        le: int | float | None = None,
        multiple_of: int | float | None = None,
        default: Any = NODEFAULT,
        default_factory: Callable[[], Any] | None = None,
        alias: str | None = None,
        title: str | None = None,
        description: str | None = None,
        json_schema_extra: dict[str, Any] | None = None,
        examples: list[Any] | None = None,
    ) -> None: ...
    @overload
    def __init__(
        self,
        *,
        ge: int | float | None = None,
        lt: int | float | None = None,
        multiple_of: int | float | None = None,
        default: Any = NODEFAULT,
        default_factory: Callable[[], Any] | None = None,
        alias: str | None = None,
        title: str | None = None,
        description: str | None = None,
        json_schema_extra: dict[str, Any] | None = None,
        examples: list[Any] | None = None,
    ) -> None: ...
    @overload
    def __init__(
        self,
        *,
        ge: int | float | None = None,
        le: int | float | None = None,
        multiple_of: int | float | None = None,
        default: Any = NODEFAULT,
        default_factory: Callable[[], Any] | None = None,
        alias: str | None = None,
        title: str | None = None,
        description: str | None = None,
        json_schema_extra: dict[str, Any] | None = None,
        examples: list[Any] | None = None,
    ) -> None: ...
    # Other (string/datetime):
    @overload
    def __init__(
        self,
        *,
        pattern: str | None = None,
        min_length: _NonNegativeInt | None = None,
        max_length: _NonNegativeInt | None = None,
        tz: bool | None = None,
        default: Any = NODEFAULT,
        default_factory: Callable[[], Any] | None = None,
        alias: str | None = None,
        title: str | None = None,
        description: str | None = None,
        json_schema_extra: dict[str, Any] | None = None,
        examples: list[Any] | None = None,
    ) -> None: ...
    default: Final[Any]
    default_factory: Final[Callable[[], Any] | None]
    alias: Final[str | None]
    gt: Final[int | float | None]
    ge: Final[int | float | None]
    lt: Final[int | float | None]
    le: Final[int | float | None]
    multiple_of: Final[int | float | None]
    pattern: Final[str | None]
    min_length: Final[int | None]
    max_length: Final[int | None]
    tz: Final[int | None]
    title: Final[str | None]
    description: Final[str | None]
    examples: Final[list[Any] | None]
    json_schema_extra: Final[dict[str, Any] | None]
    def __rich_repr__(self) -> list[tuple[str, Any]]: ...

class StructConfig:
    frozen: bool
    eq: bool
    order: bool
    array_like: bool
    gc: bool
    repr_omit_defaults: bool
    omit_defaults: bool
    forbid_unknown_fields: bool
    validate_on_init: bool
    weakref: bool
    dict: bool
    cache_hash: bool
    tag: str | int | None
    tag_field: str | None

class FieldInfo(Struct):
    name: str
    encode_name: str
    type: Any
    default: Any = NODEFAULT
    default_factory: Any = NODEFAULT

    @property
    def required(self) -> bool: ...

def fields(type_or_instance: Struct | type[Struct]) -> tuple[FieldInfo, ...]: ...
def json_schema(
    type: Any,
    *,
    schema_hook: Callable[[type[Any]], dict[str, Any]] | None = None,
    ref_template: str = "#/$defs/{name}",
) -> dict[str, Any]: ...
def json_schema_dump(
    type: Any,
    *,
    schema_hook: Callable[[type[Any]], dict[str, Any]] | None = None,
    ref_template: str = "#/$defs/{name}",
) -> bytes: ...
def json_schema_components(
    types: Iterable[Any],
    *,
    schema_hook: Callable[[type[Any]], dict[str, Any]] | None = None,
    ref_template: str = "#/$defs/{name}",
) -> tuple[tuple[dict[str, Any], ...], dict[str, Any]]: ...

class StructAdapter:
    def __init__(self, type: Any): ...
    def struct_validate_json(
        self,
        buf: str | Buffer,
        *,
        strict: bool = True,
        dec_hook: Callable[[type[Any], Any], Any] | None = None,
    ) -> Any: ...
    def struct_dump_json(
        self,
        obj: Any,
        *,
        enc_hook: Callable[[Any], Any] | None = None,
        decimal_format: Literal["string", "number"]
        | Callable[[Any], Any]
        | None = None,
        uuid_format: Literal["canonical", "hex"] | None = None,
        order: Literal["deterministic", "sorted"] | None = None,
    ) -> bytes: ...
    def struct_validate(
        self,
        obj: Any,
        *,
        strict: bool = True,
        dec_hook: Callable[[type[Any], Any], Any] | None = None,
        from_attributes: bool = False,
    ) -> Any: ...
    def struct_dump(
        self,
        obj: Any,
        *,
        enc_hook: Callable[[Any], Any] | None = None,
        order: Literal["deterministic", "sorted"] | None = None,
        str_keys: bool = False,
        builtin_types: Iterable[type] | None = None,
    ) -> Any: ...

class StrAdapter:
    def __new__(cls, type: type[Any]) -> type[str]: ...

class EncodeError(ValueError): ...
class DecodeError(ValueError): ...
class ValidationError(ValueError): ...

__version__: str
