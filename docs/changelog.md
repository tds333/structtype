# Changelog

## 0.2.0 (upcoming)

- Improve and cleanup documentation.
- Cleanup interface of Struct to be minimal and more pythonic.
- Remove jsonln support on Struct.
- Add support for types with ducktyping Structs struct_validate and struct_dump.
- Add StrAdapter, allow str like types to be handled.
- Add support for __iter__ and __getitem__ on Struct.
- Remove struct_as_dict and struct_as_tuple.

## 0.1.0 (2026-07-26)

- First release cut down from latest msgspec release.
- Goal is to have a Struct datatype with all needed methods on the class itself.
  Only minimal helper functions around.
- Support validating and dumping to Python objects.
- Support validating and dumping to JSON.
- Interface is beta and could be changed.
