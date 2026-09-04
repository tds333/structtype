# Changelog

## Unreleased

- Fix: musllinux wheels were never built at release — a config-level skip
  overrode the release build's opt-in. The release build now produces
  musllinux wheels; local and PR builds still skip them (via
  `make wheels` / the CI workflow) to stay fast.

## 0.10.0 (2026-09-04)

- **Breaking:** remove `uuid_format` and add `uuid_as_hex`. The
  `uuid_format` keyword argument (`"canonical"`, `"hex"`) is removed from
  `struct_dump_json` on both `Struct` and `StructAdapter`; passing it now
  raises a `TypeError`. It is replaced by `uuid_as_hex: bool = False`:
  `False` (default) encodes `uuid.UUID` values as canonical strings (with
  hyphens, same as `str(uuid)`), `True` encodes them as hex strings (same as
  `uuid.hex`). Decoding still accepts both forms. Hard break with no deprecation shims.

  Migration:

  | Old | New |
  |---|---|
  | `obj.struct_dump_json(uuid_format="hex")` | `obj.struct_dump_json(uuid_as_hex=True)` |
  | `obj.struct_dump_json(uuid_format="canonical")` | `obj.struct_dump_json()` (default) |

- **Breaking:** remove `decimal_format` and add `decimal_as_number`. The
  `decimal_format` keyword argument (`"string"`, `"number"`, or a callable) is
  removed from `struct_dump_json` on both `Struct` and `StructAdapter`; passing
  it now raises a `TypeError`. It is replaced by `decimal_as_number: bool =
  False`: `False` (default) encodes `decimal.Decimal` values as JSON strings,
  `True` encodes them as JSON numbers. Callable support is removed entirely — a callable emitting
  anything other than a string or number could never be decoded back into a
  `Decimal`-typed field (only JSON strings and numbers are parsed as
  `Decimal`), so the custom-shaping use case had no working round trip. Custom
  shaping now requires a wrapper type with a `struct_dump` / `struct_validate`
  protocol method. Hard break with no deprecation shims.

  Migration:

  | Old | New |
  |---|---|
  | `obj.struct_dump_json(decimal_format="number")` | `obj.struct_dump_json(decimal_as_number=True)` |
  | `obj.struct_dump_json(decimal_format="string")` | `obj.struct_dump_json()` (default) |
  | `obj.struct_dump_json(decimal_format=fn)` | no direct equivalent — encode as str/number or use a wrapper type |

- **Breaking:** `struct_config` now returns exactly what the user specified in
  the class body (the sparse `StructConfig` dict), not the fully-resolved
  config. `__struct_config__` continues to return the fully-resolved dict with
  all 15 keys and defaults applied.

  Previously `struct_config` and `__struct_config__` were aliases — both
  returned the fully-resolved dict. Now they serve different purposes:

  | Accessor | Content |
  |----------|---------|
  | `cls.struct_config` | Original sparse dict (what user wrote) |
  | `cls.__struct_config__` | Fully-resolved dict (all 15 keys, defaults + inherited) |

  Migration:

  | Old | New |
  |---|---|
  | `cls.struct_config["frozen"]` (inherited) | `cls.__struct_config__["frozen"]` |
  | `cls.struct_config` (fully resolved) | `cls.__struct_config__` |

  Example:

  ```python
  class Base(Struct):
      struct_config = StructConfig(frozen=True, tag="base")

  class Child(Base):
      struct_config = StructConfig(eq=False)

  # Before: both returned {'frozen': True, 'tag': 'base', 'eq': False, ...}
  # After:
  Child.struct_config         # {'eq': False}
  Child.__struct_config__     # {'frozen': True, 'tag': 'base', 'eq': False, ...}
  ```

- **Performance:** enum dump improvement.
- **Performance:** string interning improvement for field names during
  encoding/decoding.
- Fix JSON schema generation for edge cases.
- Document `StructAdapter(Any)` as a generic JSON codec in the usage guide.
- Internal improvement, cleanup, more tests.

## 0.9.0 (2026-08-24)

- **Breaking:** rename the `Validator` annotation family to `Constraint`:
  `Validator` → `Constraint`, `NumericValidator` → `NumericConstraint`,
  `StrValidator` → `StrConstraint`, `BytesValidator` → `BytesConstraint`,
  `CollectionValidator` → `CollectionConstraint`, and `TimezoneValidator` →
  `TimezoneConstraint`. The new name matches how these mostly behave —
  declarative constraints on field values (`ge`, `pattern`, `min_length`, ...)
  — and aligns with the internal/JSON-Schema vocabulary. Behavior, constructor
  parameters, and the one-constraint-per-annotation rule are unchanged.
  Hard break with no deprecation shims.

  Migration:

  | Old | New |
  |---|---|
  | `Validator(f)` | `Constraint(f)` |
  | `NumericValidator(gt=0)` | `NumericConstraint(gt=0)` |
  | `StrValidator(pattern="...")` | `StrConstraint(pattern="...")` |
  | `BytesValidator(min_length=1)` | `BytesConstraint(min_length=1)` |
  | `CollectionValidator(max_length=3)` | `CollectionConstraint(max_length=3)` |
  | `TimezoneValidator(tz=True)` | `TimezoneConstraint(tz=True)` |

  Custom validators now subclass `Constraint` (or a fast subclass) instead of
  `Validator`.

- **Breaking:** rename `struct_validate_self()` to `struct_check_types()` and
  the `validate_on_init` config option to `check_types_on_init`. The new names
  signal how these differ from `struct_validate` / `struct_validate_json`:
  they are pure type-checks on already-existing values, performing no
  conversion. Hard break with no deprecation shims.

  Migration:

  | Old | New |
  |---|---|
  | `p.struct_validate_self()` | `p.struct_check_types()` |
  | `StructConfig(validate_on_init=True)` | `StructConfig(check_types_on_init=True)` |

- Fix: `struct_check_types()` and `check_types_on_init=True` now recursively
  validate nested `Struct` instances inside containers (`list`, `dict`,
  `tuple`, `set`, `frozenset`). Previously only direct nested Struct fields
  were checked; Structs nested inside containers were trusted as-is.

## 0.8.1 (2026-08-23)

- `StructMeta.__new__` now forwards class-statement kwargs (e.g. `frozen=True`)
  to `__init_subclass__`. A user-defined
  `__init_subclass__(cls, **kwargs)` on a Struct base receives them unmodified,
  but they do **not** configure the struct: configuration comes only from the
  class-body `struct_config` attribute. A plain Struct subclass with unknown
  class-statement kwargs now raises `TypeError` (previously silently ignored).

  Custom metaclasses that subclass `StructMeta` can still intercept and consume
  kwargs in their own `__new__` before calling `super().__new__`.

## 0.8.0 (2026-08-23)

- **Breaking:** replace the `StructMeta` keyword-argument config API with a
  pydantic-style class-body `struct_config` attribute. `StructConfig` is now a
  `TypedDict` (plain dict at runtime). The metaclass validates that
  `struct_config` is a dict, rejects unknown keys (catches typos), and requires
  strict `bool` values for the 12 boolean options.

  `StructMeta.__new__` is now a catch-all: class-statement kwargs (e.g.
  `frozen=True`) are silently ignored. Custom metaclasses that subclass
  `StructMeta` can intercept and consume kwargs in their own `__new__` before
  calling `super().__new__`.

  Migration:

  | Old | New |
  |---|---|
  | `class Point(Struct, frozen=True)` | `class Point(Struct):`<br>`    struct_config = StructConfig(frozen=True)` |
  | `Point.__struct_config__.frozen` | `Point.__struct_config__["frozen"]` |

  New public API:
  - `StructConfig` is a `TypedDict(total=False)` with 15 optional keys
    (`frozen`, `eq`, `order`, `kw_only`, `repr_omit_defaults`, `array_like`,
    `omit_defaults`, `forbid_unknown_fields`, `validate_on_init`, `weakref`,
    `dict`, `cache_hash`, `tag`, `tag_field`, `rename`).
  - `struct_config` attribute on Struct types (resolved, fully-populated dict).
  - `kw_only` and `rename` now exposed on `__struct_config__` (previously hidden).

  Key-absence = inherit from base; `{"tag": None}` explicitly clears an
  inherited tag; `StructConfig()` (empty dict) is a no-op override.

  Removed:
  - `StructConfig` C object type (~500 lines of C deleted).
  - `StructConfig.replace()` method (use dict merge `{**cfg, ...}` instead).
  - `UNSET` sentinel for config options (still used for field values).
  - All class-definition keyword arguments on `StructMeta.__new__`.

  Simplified: project-wide config defaults no longer require a custom
  metaclass — a base class with `struct_config` suffices.

  Note: `isinstance(x, StructConfig)` raises `TypeError` on all Python versions
  (TypedDicts do not support instance checks). Use `isinstance(x, dict)` instead.

- Fix: `from __future__ import annotations` is now fully supported. Lazy string
  annotations are resolved at class creation, so `Serializer(dump=...)` codecs,
  `Field(alias=...)` metadata, and `Validator` constraints work exactly as with
  eager annotations. Previously `Serializer(dump=...)` codecs and `Field(alias=...)`
  were silently dropped under lazy annotations.

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

- **Breaking:** `struct_validate_self()` and `validate_on_init=True` are now
  pure type-checks. If a custom-typed field's value is not already an instance
  of the declared type, a `ValidationError` is raised immediately —
  `Serializer.load` and protocol fallbacks (`struct_validate`/`model_validate`)
  methods are **not** called. Any annotated `Validator` is still invoked on
  correctly-typed values. Previously, `load` was called and its converted result
  silently discarded; this was wasteful and conflated conversion with
  validation.

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
