from typing import Any

from ._core import _dump, _json_decode, _json_encode, _validate


class StructAdapter:
    """Adapter for validating and serializing types without subclassing ``Struct``.

    Useful when you want to validate or serialize plain Python types
    (e.g. ``list[int]``) without defining a full ``Struct`` subclass.

    >>> from structtype import StructAdapter
    >>> adapter = StructAdapter(list[int])
    >>> adapter.struct_validate_json(b"[1, 2, 3]")
    [1, 2, 3]
    """

    __slots__ = ("_type",)

    def __init__(self, type: Any):
        self._type = type

    def struct_validate_json(self, buf, *, strict=True, dec_hook=None):
        """Validate JSON bytes and decode into the adapter's type.

        Parameters
        ----------
        buf : str or bytes
            The JSON message to decode.
        strict : bool, optional
            If True (default), unmatched fields cause an error.
        dec_hook : callable, optional
            A callback for customizing decoding of specific types.
        """
        return _json_decode(buf, type=self._type, strict=strict, dec_hook=dec_hook)

    def struct_dump_json(
        self, obj, *, enc_hook=None, decimal_format=None, uuid_format=None, order=None
    ):
        """Encode a validated object to JSON bytes.

        Parameters
        ----------
        obj : Any
            A value to encode. Must match the adapter's type.
        enc_hook : callable, optional
            A callback for customizing encoding of specific types.
        decimal_format : str or callable, optional
            Controls how ``Decimal`` values are encoded.
        uuid_format : str, optional
            Controls how ``UUID`` values are encoded.
        order : str, optional
            Determines key ordering in JSON objects.
        """
        return _json_encode(
            obj,
            enc_hook=enc_hook,
            decimal_format=decimal_format,
            uuid_format=uuid_format,
            order=order,
        )

    def struct_validate(
        self, obj, *, strict=True, dec_hook=None, from_attributes=False
    ):
        """Validate a Python object against the adapter's type.

        Parameters
        ----------
        obj : Any
            A Python object to validate and convert.
        strict : bool, optional
            If True (default), unmatched fields cause an error.
        dec_hook : callable, optional
            A callback for customizing decoding of specific types.
        from_attributes : bool, optional
            If True, accept objects with attributes instead of dict keys.
        """
        return _validate(
            obj,
            self._type,
            strict=strict,
            dec_hook=dec_hook,
            from_attributes=from_attributes,
        )

    def struct_dump(
        self, obj, *, enc_hook=None, order=None, str_keys=False, builtin_types=None
    ):
        """Convert a validated object to built-in Python types (``dict``, ``list``, etc.)."""
        return _dump(
            obj,
            builtin_types=builtin_types,
            str_keys=str_keys,
            enc_hook=enc_hook,
            order=order,
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
