from collections.abc import Callable, Mapping
from typing import Literal, TypedDict


class StructConfig(TypedDict, total=False):
    """Configuration for a Struct type, mirroring the Struct config options.

    Used as a class-body ``struct_config`` attribute. Keys not present inherit
    from the base class; ``__struct_config__`` returns the fully-resolved dict.
    """

    frozen: bool
    eq: bool
    order: bool
    kw_only: bool
    repr_omit_defaults: bool
    array_like: bool
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
