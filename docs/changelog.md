# Changelog

## 0.4.0 (Unreleased)

- Remove `struct_force_setattr`. To set fields on a frozen struct inside
  `__post_init__`, use `object.__setattr__(self, ...)` (requires Python 3.13+).
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
