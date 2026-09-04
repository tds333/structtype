from typing import Any, get_args

from ._core import (
    JSONDecoder as _JSONDecoder,
)
from ._core import (
    Serializer as _Serializer,
)
from ._core import (  # type: ignore
    _dump,
    _json_encode,
    _validate,
)


def _has_serializer(ann):
    """True if the annotation carries a ``Serializer`` with ``load``/``dump``."""
    metadata = getattr(ann, "__metadata__", None)
    if metadata is not None:
        for meta in metadata:
            if isinstance(meta, _Serializer) and (
                meta.load is not None or meta.dump is not None
            ):
                return True
    supertype = getattr(ann, "__supertype__", None)  # NewType
    if supertype is not None and _has_serializer(supertype):
        return True
    value = getattr(ann, "__value__", None)  # PEP 695 type alias
    if value is not None and _has_serializer(value):
        return True
    return any(_has_serializer(arg) for arg in get_args(ann))


class StructAdapter:
    """Adapter for validating and serializing types without subclassing ``Struct``.

    Useful when you want to validate or serialize plain Python types
    (e.g. ``list[int]``) without defining a full ``Struct`` subclass.

    ``Serializer(load=...)`` / ``Serializer(dump=...)`` codecs are not supported
    on ``StructAdapter`` — annotations carrying one are rejected. Implement the
    ``struct_dump`` / ``struct_validate`` protocol methods on the custom type,
    or use a ``Struct``.

    >>> from structtype import StructAdapter
    >>> adapter = StructAdapter(list[int])
    >>> adapter.struct_validate_json(b"[1, 2, 3]")
    [1, 2, 3]
    """

    __slots__ = ("_decoder_loose", "_decoder_strict", "_type")

    def __init__(self, type: Any):
        if _has_serializer(type):
            raise TypeError(
                "`Serializer(load=...)`/`Serializer(dump=...)` codecs are not "
                "supported on StructAdapter; define `struct_dump`/"
                "`struct_validate` methods on the custom type, or use a "
                "`Struct` instead"
            )
        self._type = type
        self._decoder_loose = None
        self._decoder_strict = None

    def struct_validate_json(self, buf, *, strict=True):
        """Validate JSON bytes and decode into the adapter's type.

        Parameters
        ----------
        buf : str or bytes
            The JSON message to decode.
        strict : bool, optional
            If True (default), unmatched fields cause an error.
        """
        if strict:
            decoder = self._decoder_strict
            if decoder is None:
                decoder = _JSONDecoder(self._type, strict=True)
                self._decoder_strict = decoder
        else:
            decoder = self._decoder_loose
            if decoder is None:
                decoder = _JSONDecoder(self._type, strict=False)
                self._decoder_loose = decoder
        return decoder.decode(buf)

    def struct_dump_json(
        self,
        obj,
        *,
        decimal_as_number=False,
        uuid_format=None,
        sort_keys=False,
    ):
        """Encode a validated object to JSON bytes.

        Parameters
        ----------
        obj : Any
            A value to encode. Must match the adapter's type.
        decimal_as_number : bool, optional
            If True, ``Decimal`` values are encoded as JSON numbers instead
            of strings (may lose precision when decoded).
        uuid_format : str, optional
            Controls how ``UUID`` values are encoded.
        sort_keys : bool, optional
            If True, sort dict keys and set elements for deterministic output.
        """
        return _json_encode(
            obj,
            decimal_as_number=decimal_as_number,
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
