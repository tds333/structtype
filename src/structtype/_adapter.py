from typing import Any

from ._core import _convert, json_decode, json_encode


class StructAdapter:
    __slots__ = ("_type",)

    def __init__(self, type: Any):
        self._type = type

    def struct_validate_json(self, buf, *, strict=True, dec_hook=None):
        return json_decode(buf, type=self._type, strict=strict, dec_hook=dec_hook)

    def struct_dump_json(self, obj, *, enc_hook=None, decimal_format=None, uuid_format=None, order=None):
        return json_encode(obj, enc_hook=enc_hook, decimal_format=decimal_format, uuid_format=uuid_format, order=order)

    def struct_validate(
        self, obj, *, strict=True, dec_hook=None, from_attributes=False
    ):
        return _convert(
            obj,
            self._type,
            strict=strict,
            dec_hook=dec_hook,
            from_attributes=from_attributes,
        )

    def struct_dump(self, obj):
        if hasattr(obj, "struct_dump"):
            return obj.struct_dump()
        if hasattr(obj, "model_dump"):
            return obj.model_dump()
        return obj
