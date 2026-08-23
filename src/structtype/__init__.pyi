# ruff: noqa: PYI041, PYI015, PYI020, UP037
import enum
from collections.abc import Callable, Iterable, Iterator, Mapping
from inspect import Signature
from typing import (
    Any,
    ClassVar,
    Final,
    Literal,
    TypeAlias,
    TypedDict,
    final,
    overload,
)

from typing_extensions import Buffer, Self, dataclass_transform

@final
class UnsetType(enum.Enum):
    UNSET = "UNSET"
    def __bool__(self) -> Literal[False]: ...

UNSET: Final = UnsetType.UNSET

@final
class _NoDefault(enum.Enum):
    NODEFAULT = "NODEFAULT"

NODEFAULT: Final = _NoDefault.NODEFAULT

class StructConfig(TypedDict, total=False):
    frozen: bool
    eq: bool
    order: bool
    kw_only: bool
    array_like: bool
    repr_omit_defaults: bool
    omit_defaults: bool
    forbid_unknown_fields: bool
    validate_on_init: bool
    weakref: bool
    dict: bool
    cache_hash: bool
    tag: bool | str | int | Callable[[str], str | int] | None
    tag_field: str | None
    rename: (
        None
        | Literal["lower", "upper", "camel", "pascal", "kebab"]
        | Callable[[str], str | None]
        | Mapping[str, str]
    )

class StructMeta(type):
    __struct_fields__: ClassVar[tuple[str, ...]]
    __struct_defaults__: ClassVar[tuple[Any, ...]]
    __struct_alias_fields__: ClassVar[tuple[str, ...]]
    __match_args__: ClassVar[tuple[str, ...]] = ...
    @property
    def __signature__(self) -> Signature: ...
    @property
    def __struct_config__(self) -> StructConfig: ...
    @property
    def struct_config(self) -> StructConfig: ...

@dataclass_transform(field_specifiers=("Field",))  # type: ignore
class Struct(metaclass=StructMeta):
    __struct_fields__: ClassVar[tuple[str, ...]]
    __struct_config__: ClassVar[StructConfig]
    struct_config: ClassVar[StructConfig]
    __struct_alias_fields__: ClassVar[tuple[str, ...]]
    __struct_defaults__: ClassVar[tuple[Any, ...]]
    __match_args__: ClassVar[tuple[str, ...]] = ...
    # A default __init__ so that Structs with unknown field types
    # won't error on every call to `__init__`
    def __init__(self, *args: Any, **kwargs: Any) -> None: ...
    def __rich_repr__(self) -> list[tuple[str, Any]]: ...
    def __copy__(self) -> Self: ...
    def __reduce__(self) -> tuple: ...
    def __replace__(self, **changes: Any) -> Self: ...
    def __iter__(self) -> Iterator[tuple[str, Any]]: ...
    def struct_dump_json(
        self,
        *,
        decimal_format: Literal["string", "number"]
        | Callable[[Any], Any]
        | None = None,
        uuid_format: Literal["canonical", "hex"] | None = None,
        sort_keys: bool = False,
    ) -> bytes: ...
    def struct_dump(
        self,
        *,
        sort_keys: bool = False,
        str_keys: bool = False,
        builtin_types: Iterable[type] | None = None,
    ) -> dict[str, Any] | list[Any]: ...
    def struct_validate_self(self) -> None: ...
    @classmethod
    def struct_validate_json(
        cls,
        buf: str | Buffer,
        *,
        strict: bool = True,
    ) -> Self: ...
    @classmethod
    def struct_validate(
        cls,
        obj: Any,
        *,
        strict: bool = True,
        from_attributes: bool = False,
    ) -> Self: ...

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
class Factory:
    def __new__(cls, factory: Callable[[], Any]) -> Any: ...
    factory: Final[Callable[[], Any]]

@final
class Field:
    def __init__(
        self,
        *,
        alias: str | None = None,
        title: str | None = None,
        description: str | None = None,
        json_schema_extra: dict[str, Any] | None = None,
        examples: list[Any] | None = None,
        deprecated: bool | None = None,
    ) -> None: ...
    alias: Final[str | None]
    title: Final[str | None]
    description: Final[str | None]
    examples: Final[list[Any] | None]
    deprecated: Final[bool | None]
    json_schema_extra: Final[dict[str, Any] | None]
    def __rich_repr__(self) -> list[tuple[str, Any]]: ...

@final
class Serializer:
    def __init__(
        self,
        *,
        load: Callable[[Any], Any] | None = None,
        dump: Callable[[Any], Any] | None = None,
    ) -> None: ...
    load: Final[Callable[[Any], Any] | None]
    dump: Final[Callable[[Any], Any] | None]

class Validator:
    def __init__(self, fn: Callable[[Any], Any] | None = None) -> None: ...
    def __call__(self, value: Any) -> None: ...

@final
class NumericValidator(Validator):
    def __init__(
        self,
        *,
        gt: int | float | None = None,
        ge: int | float | None = None,
        lt: int | float | None = None,
        le: int | float | None = None,
        multiple_of: int | float | None = None,
    ) -> None: ...
    gt: Final[int | float | None]
    ge: Final[int | float | None]
    lt: Final[int | float | None]
    le: Final[int | float | None]
    multiple_of: Final[int | float | None]

@final
class StrValidator(Validator):
    def __init__(
        self,
        *,
        pattern: str | None = None,
        min_length: _NonNegativeInt | None = None,
        max_length: _NonNegativeInt | None = None,
    ) -> None: ...
    pattern: Final[str | None]
    min_length: Final[int | None]
    max_length: Final[int | None]

@final
class BytesValidator(Validator):
    def __init__(
        self,
        *,
        min_length: _NonNegativeInt | None = None,
        max_length: _NonNegativeInt | None = None,
    ) -> None: ...
    min_length: Final[int | None]
    max_length: Final[int | None]

@final
class CollectionValidator(Validator):
    def __init__(
        self,
        *,
        min_length: _NonNegativeInt | None = None,
        max_length: _NonNegativeInt | None = None,
    ) -> None: ...
    min_length: Final[int | None]
    max_length: Final[int | None]

@final
class TimezoneValidator(Validator):
    def __init__(self, *, tz: bool) -> None: ...
    tz: Final[bool]

class FieldInfo(Struct):
    name: str
    alias: str
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
    ) -> Any: ...
    def struct_dump_json(
        self,
        obj: Any,
        *,
        decimal_format: Literal["string", "number"]
        | Callable[[Any], Any]
        | None = None,
        uuid_format: Literal["canonical", "hex"] | None = None,
        sort_keys: bool = False,
    ) -> bytes: ...
    def struct_validate(
        self,
        obj: Any,
        *,
        strict: bool = True,
        from_attributes: bool = False,
    ) -> Any: ...
    def struct_dump(
        self,
        obj: Any,
        *,
        sort_keys: bool = False,
        str_keys: bool = False,
        builtin_types: Iterable[type] | None = None,
    ) -> Any: ...

class EncodeError(ValueError): ...
class DecodeError(ValueError): ...
class ValidationError(ValueError): ...

__version__: str

ALL_BUILTIN_TYPES: tuple[type, ...]
