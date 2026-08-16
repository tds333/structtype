"""Type-check fixture: introspection, schema, adapter, and errors.

Checked by ``make typecheck-tests`` and ``tests/test_typecheck.py`` against the
shipped ``structtype`` type stubs. Must stay free of ``# type: ignore``.
"""

from typing import Any

from structtype import (
    ALL_BUILTIN_TYPES,
    DecodeError,
    EncodeError,
    FieldInfo,
    Struct,
    StructAdapter,
    StructConfig,
    ValidationError,
    fields,
    json_schema,
    json_schema_components,
    json_schema_dump,
)


class Item(Struct):
    name: str
    price: float = 0.0


fis: tuple[FieldInfo, ...] = fields(Item)
fname: str = fis[0].name
freq: bool = fis[0].required
fdefault = fis[0].default

adapter = StructAdapter(list[int])
decoded = adapter.struct_validate_json(b"[1,2]")
encoded: bytes = adapter.struct_dump_json([1, 2])
validated = adapter.struct_validate([1, 2])
dumped = adapter.struct_dump([1, 2])

schema: dict[str, Any] = json_schema(Item)
schema_bytes: bytes = json_schema_dump(Item)
components = json_schema_components([Item])

# Exceptions subclass ValueError, so they can be caught generically
err: ValueError = ValidationError("msg")
decode_err: ValueError = DecodeError("msg")
encode_err: ValueError = EncodeError("msg")


def validate(buf: bytes) -> Item:
    try:
        return Item.struct_validate_json(buf)
    except ValidationError:
        raise
    except (DecodeError, EncodeError) as exc:
        raise ValueError("bad") from exc


cfg: StructConfig = Item.__struct_config__
frozen: bool = cfg.frozen
builtin_types: tuple[type, ...] = ALL_BUILTIN_TYPES
