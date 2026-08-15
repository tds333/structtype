from typing import Any, get_args

from ._core import (  # type: ignore
    Field as _Field,
)
from ._core import (
    _dump,
    _json_decode,
    _json_encode,
    _validate,
)


def _has_codec(ann):
    """True if the annotation carries a ``Field`` with ``dump``/``validate``."""
    metadata = getattr(ann, "__metadata__", None)
    if metadata is not None:
        for meta in metadata:
            if isinstance(meta, _Field) and (
                meta.dump is not None or meta.validate is not None
            ):
                return True
    supertype = getattr(ann, "__supertype__", None)  # NewType
    if supertype is not None and _has_codec(supertype):
        return True
    value = getattr(ann, "__value__", None)  # PEP 695 type alias
    if value is not None and _has_codec(value):
        return True
    return any(_has_codec(arg) for arg in get_args(ann))


class StructAdapter:
    """Adapter for validating and serializing types without subclassing ``Struct``.

    Useful when you want to validate or serialize plain Python types
    (e.g. ``list[int]``) without defining a full ``Struct`` subclass.

    ``Field(dump=...)`` / ``Field(validate=...)`` codecs are not supported on
    ``StructAdapter`` — annotations carrying one are rejected. Implement the
    ``struct_dump`` / ``struct_validate`` protocol methods on the custom type,
    or use a ``Struct``.

    >>> from structtype import StructAdapter
    >>> adapter = StructAdapter(list[int])
    >>> adapter.struct_validate_json(b"[1, 2, 3]")
    [1, 2, 3]
    """

    __slots__ = ("_type",)

    def __init__(self, type: Any):
        if _has_codec(type):
            raise TypeError(
                "`Field(dump=...)`/`Field(validate=...)` codecs are not supported "
                "on StructAdapter; define `struct_dump`/`struct_validate` methods "
                "on the custom type, or use a `Struct` instead"
            )
        self._type = type

    def struct_validate_json(self, buf, *, strict=True):
        """Validate JSON bytes and decode into the adapter's type.

        Parameters
        ----------
        buf : str or bytes
            The JSON message to decode.
        strict : bool, optional
            If True (default), unmatched fields cause an error.
        """
        return _json_decode(buf, type=self._type, strict=strict)

    def struct_dump_json(
        self,
        obj,
        *,
        decimal_format=None,
        uuid_format=None,
        sort_keys=False,
    ):
        """Encode a validated object to JSON bytes.

        Parameters
        ----------
        obj : Any
            A value to encode. Must match the adapter's type.
        decimal_format : str or callable, optional
            Controls how ``Decimal`` values are encoded.
        uuid_format : str, optional
            Controls how ``UUID`` values are encoded.
        sort_keys : bool, optional
            If True, sort dict keys and set elements for deterministic output.
        """
        return _json_encode(
            obj,
            decimal_format=decimal_format,
            uuid_format=uuid_format,
            sort_keys=sort_keys,
        )

    def struct_validate(self, obj, *, strict=True, from_attributes=False):
        """Validate a Python object against the adapter's type.

        Parameters
        ----------
        obj : Any
            A Python object to validate and convert.
        strict : bool, optional
            If True (default), unmatched fields cause an error.
        from_attributes : bool, optional
            If True, accept objects with attributes instead of dict keys.
        """
        return _validate(
            obj,
            self._type,
            strict=strict,
            from_attributes=from_attributes,
        )

    def struct_dump(
        self,
        obj,
        *,
        sort_keys=False,
        str_keys=False,
        builtin_types=None,
    ):
        """Convert a validated object to built-in Python types (``dict``, ``list``, etc.)."""
        return _dump(
            obj,
            builtin_types=builtin_types,
            str_keys=str_keys,
            sort_keys=sort_keys,
        )


class StrAdapter:
    """Create a ``str`` subclass wrapper for validating a type during
    structtype serialization.

    Wraps a type that has a single-argument string constructor
    (e.g. ``HttpUrl``, ``EmailStr``, ``IPv4Address``) into a ``str``
    subclass. The wrapped value is stored as a string but validated by
    calling ``typ(value)`` on construction. During structtype
    validation and serialization, the wrapper is treated as a native
    ``str``.

    >>> from structtype import StrAdapter, Struct
    >>> from ipaddress import IPv4Address
    >>>
    >>> class Config(Struct):
    ...     ip: StrAdapter(IPv4Address)
    """

    __slots__ = ()

    def __new__(cls, typ):
        return type(
            f"_Wrapped_{typ.__name__}",
            (str,),
            {
                "__slots__": (),
                "__new__": lambda self, v: str.__new__(self, str(typ(v))),
            },
        )
