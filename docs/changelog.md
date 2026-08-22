# Changelog

## 0.7.0 (2026-08-22)

- **Breaking:** Split `Field` into three focused annotation types:
  `Field` (identity + schema metadata), `Serializer` (value conversion),
  and `Validator` (validation constraints). This is a hard break with no
  deprecation shims.

  Migration table:

  | Old | New |
  |---|---|
  | `Field(gt=...)` | `NumericValidator(gt=...)` |
  | `Field(ge=...)` | `NumericValidator(ge=...)` |
  | `Field(lt=...)` | `NumericValidator(lt=...)` |
  | `Field(le=...)` | `NumericValidator(le=...)` |
  | `Field(multiple_of=...)` | `NumericValidator(multiple_of=...)` |
  | `Field(pattern=...)` | `StrValidator(pattern=...)` |
  | `Field(min_length=...)` | `StrValidator(min_length=...)` / `BytesValidator(min_length=...)` / `CollectionValidator(min_length=...)` |
  | `Field(max_length=...)` | same as `min_length` |
  | `Field(tz=...)` | `TimezoneValidator(tz=...)` |
  | `Field(validate=f)` | `Serializer(load=f)` |
  | `Field(dump=g)` | `Serializer(dump=g)` |
  | `Field(alias=...)`, `Field(title=...)`, `Field(description=...)`, etc. | Unchanged — still `Field(...)` |

  New public API:
  - `Serializer(*, load=None, dump=None)` — value conversion codecs.
  - `Validator(fn=None)` — base class for validation; callable with a value.
  - `NumericValidator(*, gt=None, ge=None, lt=None, le=None, multiple_of=None)` — numeric constraints.
  - `StrValidator(*, pattern=None, min_length=None, max_length=None)` — string constraints.
  - `BytesValidator(*, min_length=None, max_length=None)` — bytes constraints.
  - `CollectionValidator(*, min_length=None, max_length=None)` — list/set/frozenset/tuple/dict constraints.
  - `TimezoneValidator(*, tz=True)` — datetime/timezone constraints.

  Composition: at most one `Field`, one `Serializer`, and one `Validator` per
  annotation position; cross-kind combinations encouraged. Passing constraint
  params to `Field` now raises `TypeError`.

## 0.6.0 (2026-08-21)

- **Breaking:** rename `__struct_encode_fields__` to `__struct_alias_fields__`
  for consistency with the alias terminology. Internal C identifiers are also
  renamed (e.g. `struct_encode_fields` → `struct_alias_fields`).
- **Breaking:** rename `FieldInfo.encode_name` to `FieldInfo.alias`.
- **Breaking:** remove `StrAdapter`. For custom types constructed from a single
  string argument (`IPv4Address`, `HttpUrl`, ...), use
  `Annotated[T, Field(dump=str, validate=T)]` codecs instead.
- **Breaking:** remove the `gc` Struct configuration option. All Struct types
  now always support cyclic garbage collection, while retaining deferred tracking
  for scalar-only instances.
- Harden JSON serialization buffer sizing against integer overflow, preventing
  crashes or out-of-bounds writes when processing oversized values.

## 0.5.0 (2026-08-15)

- **Breaking:** remove the `enc_hook` / `dec_hook` keyword arguments from
  `struct_dump`, `struct_dump_json`, `struct_validate`, and
  `struct_validate_json` on both `Struct` and `StructAdapter`. Custom types
  now implement the `struct_dump` / `struct_validate` protocol methods
  (pydantic's `model_dump` / `model_validate` are also recognized), or use the
  new per-field codecs. Passing these arguments now raises a `TypeError`.
- Add `Field(dump=...)` and `Field(validate=...)` codecs, used as
  `Annotated[X, Field(dump=..., validate=...)]`. Codecs may only be attached
  to custom types; attaching one to a natively supported type or to a union
  raises a `TypeError` at class creation time, as does attaching two
  conflicting `dump` codecs within a single field. Codecs are supported on
  `Struct` fields only — `StructAdapter` rejects codec'd annotations (use a
  `struct_dump` / `struct_validate` protocol method there).
- Remove `Field(default=...)` and `Field(default_factory=...)`. Defaults are now
  always specified on the class body: a constant via `x: int = 3`, and a
  per-instance default by wrapping a callable in the new `structtype.Factory`,
  e.g. `x: list = Factory(list)`. `Field` is now a pure constraints/metadata
  object and may only be used inside `typing.Annotated`.
- Add `structtype.Factory(func)`, a wrapper for per-instance default values.
  `= Factory(list)` is equivalent to the previous `Field(default_factory=list)`.
- Add `Field(deprecated=True)` to mark a field as deprecated in the generated
  JSON Schema.

## 0.4.0 (2026-08-09)

- Remove `struct_force_setattr`. To set fields on a frozen struct inside
  `__post_init__`, use `object.__setattr__(self, ...)` (requires Python 3.13+).
- Replace the encoder `order=` argument with `sort_keys: bool` on
  `struct_dump`, `struct_dump_json`.
  `sort_keys=True` sorts dict keys and set elements for deterministic output;
  struct, dataclass, and object fields keep their declaration order. The
  previous `order='sorted'` mode is removed.
- Add `tuple` with all builtin types usable in `struct_dump`.
- Improved validation error messages.
- Fix possible memory leak.
- Improve free threading support.
- Add more safeguards to the C core.
- Improve `FieldInfo` repr.

## 0.3.0 (2026-08-02)

- Improve C compilation, fix warnings
- Add Windows wheel builds.
- Add Python 3.15 wheel builds.
- Add pydantic json schema support.
- Internal renames and fix StructAdapter struct_dump.
- Pass through struct_dump parameters.

## 0.2.0 (2026-07-27)

- Improve and cleanup documentation.
- Cleanup interface of Struct to be minimal and more pythonic.
- Remove jsonln support on Struct.
- Add support for types with ducktyping Structs struct_validate and struct_dump.
- Add StrAdapter, allow str like types to be handled.
- Add support for __iter__ on Struct, supports dict(Struct).
- Remove struct_as_dict and struct_as_tuple.

## 0.1.0 (2026-07-26)

- First release cut down from latest msgspec release.
- Goal is to have a Struct datatype with all needed methods on the class itself.
  Only minimal helper functions around.
- Support validating and dumping to Python objects.
- Support validating and dumping to JSON.
- Interface is beta and could be changed.
