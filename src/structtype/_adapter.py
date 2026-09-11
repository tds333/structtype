import types
import typing
from typing import Any, get_args

from ._core import (  # type: ignore
    Constraint as _Constraint,
    JSONDecoder as _JSONDecoder,
    Serializer as _Serializer,
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


def _is_union(t):
    """True if ``t`` is ``typing.Union`` or a ``types.UnionType`` (``X | Y``)."""
    if getattr(t, "__origin__", None) is typing.Union:
        return True
    return isinstance(t, types.UnionType)


def _has_constraint_on_union(ann):
    """True if a ``Constraint`` is attached to a union or optional type.

    ``Struct`` rejects this at class creation; ``StructAdapter`` mirrors the
    same rule at construction.  A concrete member may still be constrained and
    then made optional, e.g. ``Annotated[T, Constraint(...)] | None``.
    """
    metadata = getattr(ann, "__metadata__", None)
    if metadata is not None:
        origin = getattr(ann, "__origin__", None)
        if (
            origin is not None
            and _is_union(origin)
            and any(isinstance(meta, _Constraint) for meta in metadata)
        ):
            return True
    supertype = getattr(ann, "__supertype__", None)  # NewType
    if supertype is not None and _has_constraint_on_union(supertype):
        return True
    value = getattr(ann, "__value__", None)  # PEP 695 type alias
    if value is not None and _has_constraint_on_union(value):
        return True
    return any(_has_constraint_on_union(arg) for arg in get_args(ann))


class StructAdapter:
    """Adapter for validating and serializing types without subclassing ``Struct``.

    Useful when you want to validate or serialize plain Python types
    (e.g. ``list[int]``) without defining a full ``Struct`` subclass.

    ``Serializer(load=...)`` / ``Serializer(dump=...)`` codecs are not supported
    on ``StructAdapter`` — annotations carrying one are rejected. Implement the
    ``struct_dump`` / ``struct_validate`` protocol methods on the custom type,
    or use a ``Struct``.

    ``Constraint`` annotations are supported on concrete types. A ``Constraint``
    attached to a union or optional type is rejected at construction, matching
    ``Struct`` class creation; make the field optional with
    ``Annotated[T, Constraint(...)] | None``.

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
        if _has_constraint_on_union(type):
            raise TypeError(
                "`Constraint` must be applied to a concrete type, not a union "
                "or optional type; use `Annotated[T, Constraint(...)] | None` "
                "for optional fields"
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
            If True (default), use strict type validation and coercion rules.
            If False, allow the documented lax-mode conversions.
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
        uuid_as_hex=False,
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
        uuid_as_hex : bool, optional
            If True, ``UUID`` values are encoded as hex strings instead of
            canonical form.
        sort_keys : bool, optional
            If True, sort dict keys and set elements for deterministic output.
        """
        return _json_encode(
            obj,
            decimal_as_number=decimal_as_number,
            uuid_as_hex=uuid_as_hex,
            sort_keys=sort_keys,
        )

    def struct_validate(self, obj, *, strict=True, from_attributes=False):
        """Validate a Python object against the adapter's type.

        Parameters
        ----------
        obj : Any
            A Python object to validate and convert.
        strict : bool, optional
            If True (default), use strict type validation and coercion rules.
            If False, allow the documented lax-mode conversions.
        from_attributes : bool, optional
            If True, accept non-dict objects by reading matching attributes.
            Dict input continues to match fields by serialized alias names.
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
