# Graph Report - structtype  (2026-08-04)

## Corpus Check
- 43 files · ~136,589 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1764 nodes · 4857 edges · 91 communities (60 shown, 31 thin omitted)
- Extraction: 97% EXTRACTED · 3% INFERRED · 0% AMBIGUOUS · INFERRED: 165 edges (avg confidence: 0.74)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `43ab81ac`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- _core.c
- test_schema.py
- structtype_get_global_state
- TypeNode
- EncoderState
- test_inspect.py
- _inspect.py
- dump_obj
- JSONDecoderState
- PathNode
- parametrize
- Field
- StructspecState
- test_struct_meta.py
- Py_ssize_t
- test_json.py
- visitproc
- test_struct.py
- parametrize
- TestDict
- test_check.py
- test_msgspec.py
- structtype/__init__.py
- test_constraints.py
- test_pydantic.py
- TestRename
- StructAdapter
- TestStruct
- TestDatetime
- TestEncoderMisc
- multi_type_info
- json_decode_struct_array_inner
- TestStructParameterOrdering
- bench_libs.py
- TestGetClassAnnotations
- _utils.py
- temp_module
- replace
- validate_lookup_tag
- test_raw.py
- TestRaw
- bench_gc.py
- atof.h
- Rand
- TestBoolAndNone
- TestHash
- TestStructGC
- structtype — AGENTS.md
- TestStrings
- TestDecodeFunction
- TestSequences
- TestOrderAndEq
- structtype_geojson.py
- .roundtrip
- TestStructArray
- TestRepr
- TestEncodeFunction
- TestInspectFields
- strbuilder_build
- test_cpylint.py
- test_free_threading.py
- utils.py
- structtype
- TestDecimal
- test_JSONTestSuite.py
- TestMixins
- LiteralType
- TestFloatConstraints
- TestStructArrayUnion
- TestPostInit
- bench_structs.py
- Changelog
- as_tuple
- EnumType
- TestSignature
- graphify.js
- Struct_hash
- BoolType
- .__init__
- .struct_dump_json
- .struct_validate
- .struct_dump
- __dir__
- .includes_none
- test_structadapter_pydantic
- test_decode_pydantic_direct
- test_python_validate_pydantic
- structtype

## God Nodes (most connected - your core abstractions)
1. `type_info()` - 56 edges
2. `_SchemaGenerator` - 37 edges
3. `ms_validation_error()` - 33 edges
4. `StructAdapter` - 32 edges
5. `IntType` - 32 edges
6. `TestDict` - 31 edges
7. `structtype_get_global_state()` - 29 edges
8. `json_encode_uncommon()` - 29 edges
9. `json_err_invalid()` - 28 edges
10. `validate_obj()` - 28 edges

## Surprising Connections (you probably didn't know these)
- `test_dump_json()` --calls--> `StructAdapter`  [INFERRED]
  tests/test_adapter.py → src/structtype/_adapter.py
- `test_dump_json_tagged()` --calls--> `StructAdapter`  [INFERRED]
  tests/test_adapter.py → src/structtype/_adapter.py
- `test_dump_python_non_struct()` --calls--> `StructAdapter`  [INFERRED]
  tests/test_adapter.py → src/structtype/_adapter.py
- `test_dump_python_struct()` --calls--> `StructAdapter`  [INFERRED]
  tests/test_adapter.py → src/structtype/_adapter.py
- `test_roundtrip_json()` --calls--> `StructAdapter`  [INFERRED]
  tests/test_adapter.py → src/structtype/_adapter.py

## Import Cycles
- 3-file cycle: `src/structtype/__init__.py -> src/structtype/_json_schema.py -> src/structtype/_inspect.py -> src/structtype/__init__.py`

## Communities (91 total, 31 thin omitted)

### Community 0 - "_core.c"
Cohesion: 0.06
Nodes (90): Encoder, PyObject, check_positional_nargs(), clear_slots(), _constr_as_py_ssize_t(), encode_common(), Encoder_clear(), Encoder_dealloc() (+82 more)

### Community 1 - "test_schema.py"
Cohesion: 0.03
Nodes (34): parametrize, py315_or_later_only, test_array_metadata(), test_binary(), test_binary_metadata(), test_custom(), test_custom_schema_hook(), test_dataclass_or_attrs() (+26 more)

### Community 2 - "structtype_get_global_state"
Cohesion: 0.05
Nodes (74): Constraints, IntLookupEntry, IntLookupHashmap, JSONDecoder, Py_buffer, PyMODINIT_FUNC, Raw, _constr_as_f64() (+66 more)

### Community 3 - "TypeNode"
Cohesion: 0.10
Nodes (68): MS_INLINE, _err_py_ssize_t_constraint(), _ms_check_float_constraints(), _ms_check_str_constraints(), _ms_passes_array_constraints(), ms_passes_bytes_constraints(), ms_passes_float_constraints_inline(), _ms_passes_map_constraints() (+60 more)

### Community 4 - "EncoderState"
Cohesion: 0.10
Nodes (67): AssocItem, AssocList, DataclassIter, EncoderState, MS_NOINLINE, _AssocItem_lt(), AssocList_Append(), AssocList_AppendCStr() (+59 more)

### Community 5 - "test_inspect.py"
Cohesion: 0.09
Nodes (63): py312_plus, AnyType, FieldNode, IntType, ListType, A type corresponding to `typing.Any`., A type corresponding to `int`. Parameters ---------- gt: int, optional If set,…, A type corresponding to `str`. Parameters ---------- min_length: int, optional… (+55 more)

### Community 6 - "_inspect.py"
Cohesion: 0.09
Nodes (61): ByteArrayType, BytesType, CollectionType, CustomType, DataclassType, DateTimeType, DateType, DecimalType (+53 more)

### Community 7 - "dump_obj"
Cohesion: 0.08
Nodes (53): DumpState, floating_decimal_64, ascii_get_buffer(), dump_binary(), dump_date(), dump_datetime(), dump_decimal(), dump_dict() (+45 more)

### Community 8 - "JSONDecoderState"
Cohesion: 0.13
Nodes (55): JSONDecoderState, char_is_special(), char_is_special_or_nonascii(), json_decode(), json_decode_array(), json_decode_cint(), json_decode_cstr(), json_decode_dataclass() (+47 more)

### Community 9 - "PathNode"
Cohesion: 0.11
Nodes (51): PathNode, DataclassInfo_post_decode(), datetime_from_epoch(), double_as_int64(), _err_int_constraint(), json_float_hook(), ms_decode_big_pyint(), ms_decode_bigint() (+43 more)

### Community 10 - "parametrize"
Cohesion: 0.06
Nodes (9): parametrize, Tricky float values, many taken from…, Some tricky test cases from…, The digits part of these would put them over the limit to inf, but the exponent…, Most functionality is tested in `test_common.py:TestStructUnion`, this only…, TestBinary, TestFloat, TestIntegers (+1 more)

### Community 11 - "Field"
Cohesion: 0.09
Nodes (10): Field, Field_clear(), Field_dealloc(), Field_richcompare(), parametrize, Constraint validity is applied in two places: - Type checks on constraint…, TestArrayConstraints, TestBytesConstraints (+2 more)

### Community 12 - "StructspecState"
Cohesion: 0.08
Nodes (42): PyTypeObject, dict_discard(), extract_field_from_annotated(), Factory_New(), json_str_requires_escaping(), ms_encode_base64_size(), ms_encode_err_type_unsupported(), ms_encode_uuid() (+34 more)

### Community 13 - "test_struct_meta.py"
Cohesion: 0.06
Nodes (23): Tests for the exposed StructMeta metaclass., Test that StructMeta can be inherited in Python code., Test that StructMeta is properly exposed., Test if structs created by StructMeta subclasses support various function…, Test that StructMeta can be used directly as a metaclass., Test multi-level inheritance of StructMeta subclasses., Test compatibility of structs created by StructMeta subclasses with encoders., Test that StructMeta properly handles struct options. (+15 more)

### Community 14 - "Py_ssize_t"
Cohesion: 0.14
Nodes (32): Py_ssize_t, DataclassInfo_lookup_key(), datetime_round_up_micros(), days_in_month(), is_leap_year(), json_decode_binary(), json_decode_dict_key_fallback(), json_decode_string() (+24 more)

### Community 15 - "test_json.py"
Cohesion: 0.06
Nodes (12): FruitInt, FruitStr, # TODO: remove when 3.10 support is dropped:, Most tests are in `test_common`, this just tests some JSON peculiarities, Most tests are in `test_common`, this just tests some JSON peculiarities, Most tests are in `test_common`, this just tests some JSON peculiarities, TestDataclass, TestDecoderMisc (+4 more)

### Community 16 - "visitproc"
Cohesion: 0.09
Nodes (31): DataclassInfo, Factory, LiteralInfo, NamedTupleInfo, DataclassInfo_clear(), DataclassInfo_dealloc(), DataclassInfo_traverse(), Factory_clear() (+23 more)

### Community 17 - "test_struct.py"
Cohesion: 0.07
Nodes (15): Fruit, PointKWOnly, Struct, If an attribute is unset, raise an AttributeError appropriately, Test that struct operations that access fields properly decref, Structs aren't tracked by GC until/unless they reference a container type, test_field_default_conflict_with_class_body(), test_field_outside_annotated_errors() (+7 more)

### Community 18 - "parametrize"
Cohesion: 0.09
Nodes (12): nogc(), parametrize, Temporarily disable GC, test_singletons(), test_struct_empty_mutable_defaults_fast_copy(), test_struct_empty_mutable_defaults_work(), test_struct_immutable_defaults_use_instance(), test_struct_nonempty_mutable_defaults_error() (+4 more)

### Community 20 - "test_check.py"
Cohesion: 0.14
Nodes (23): Named, Nested, Point, Struct, Ranged, test_bad_kwarg_raises(), test_extra_positional_args_raises(), test_ge_constraint_violation() (+15 more)

### Community 21 - "test_msgspec.py"
Cohesion: 0.10
Nodes (25): Container, MsgspecItem, MsgspecPoint, MsgspecUser, Struct, Full JSON roundtrip preserves msgspec data., Nested msgspec struct field in a struct — encoded via structtype encoder., Direct msgspec struct encode via StructAdapter. (+17 more)

### Community 22 - "structtype/__init__.py"
Cohesion: 0.11
Nodes (19): The type of `UNSET`. See Also -------- UNSET, UnsetType, Create a ``str`` subclass wrapper for validating a type during structtype…, StrAdapter, _build_name_map(), _collect_component_types(), _get_class_name(), _get_doc() (+11 more)

### Community 23 - "test_constraints.py"
Cohesion: 0.09
Nodes (9): assert_eq(), assert_ne(), _JsonProto, proto(), fixture, TestIntConstraints, TestMapConstraints, TestStrConstraints (+1 more)

### Community 24 - "test_pydantic.py"
Cohesion: 0.13
Nodes (21): BaseModel, Container, Point, Struct, Full JSON roundtrip preserves pydantic data., Nested pydantic field in a struct — encoded via structtype encoder., Direct pydantic encode via StructAdapter., Nested pydantic field decoded via structtype validator. (+13 more)

### Community 26 - "StructAdapter"
Cohesion: 0.15
Nodes (17): Validate JSON bytes and decode into the adapter's type. Parameters ----------…, Adapter for validating and serializing types without subclassing ``Struct``.…, StructAdapter, test_dump_json(), test_dump_json_tagged(), test_dump_python_non_struct(), test_dump_python_struct(), test_json_schema_constrained() (+9 more)

### Community 27 - "TestStruct"
Cohesion: 0.10
Nodes (3): Person, Uint64 values aren't currently valid tag values, but we still want to raise a…, TestStruct

### Community 28 - "TestDatetime"
Cohesion: 0.10
Nodes (4): skipif, Both T & Z can be upper/lowercase, structtype supports a few relaxations of the RFC3339 format., TestDatetime

### Community 29 - "TestEncoderMisc"
Cohesion: 0.11
Nodes (3): emscripten_stack_limited, Node, TestEncoderMisc

### Community 30 - "multi_type_info"
Cohesion: 0.11
Nodes (13): FieldInfo, fields(), _merge_json(), multi_type_info(), _origin_args_metadata(), Any, Struct, A record describing a field in a struct. (+5 more)

### Community 31 - "json_decode_struct_array_inner"
Cohesion: 0.22
Nodes (19): Factory_Call(), get_default(), json_decode_struct_array_inner(), ms_error_unknown_field(), ms_missing_required_field(), Struct_alloc(), Struct_copy(), Struct_decode_post_init() (+11 more)

### Community 33 - "bench_libs.py"
Cohesion: 0.12
Nodes (12): Dir_ms, Dir_pd, Dir_st, File_ms, File_pd, File_st, Item_ms, Item_pd (+4 more)

### Community 35 - "_utils.py"
Cohesion: 0.17
Nodes (13): _apply_params(), _eval_type(), _forward_ref(), _get_class_annotations(), _get_class_mro_and_typevar_mappings(), get_dataclass_info(), get_pydantic_info(), get_typeddict_info() (+5 more)

### Community 36 - "temp_module"
Cohesion: 0.14
Nodes (7): test_component_names_collide(), Annotations that start with `ClassVar`/`typing.ClassVar` but don't end there…, test_struct_defaults_from_field_annotated(), TestClassVar, parametrize, Mutually recursive struct types defined inside functions don't work (and…, temp_module()

### Community 37 - "replace"
Cohesion: 0.25
Nodes (5): copy_replace(), Point, fixture, replace(), TestReplace

### Community 38 - "validate_lookup_tag"
Cohesion: 0.26
Nodes (14): IntLookup, Lookup, fast_long_extract_parts(), IntLookup_clear(), IntLookup_dealloc(), IntLookup_GetInt64(), IntLookup_GetInt64OrError(), IntLookup_GetPyIntOrError() (+6 more)

### Community 39 - "test_raw.py"
Cohesion: 0.14
Nodes (5): requires_subprocess, parametrize, See https://github.com/tds333/structtype/pull/709, test_raw_constructor(), test_raw_copy_doesnt_leak()

### Community 41 - "bench_gc.py"
Cohesion: 0.21
Nodes (10): bench_gc(), format_table(), main(), Point, PointClass, PointClassSlots, PointGCFalse, print_header() (+2 more)

### Community 42 - "atof.h"
Cohesion: 0.32
Nodes (11): ms_hpd, ms_uint128, eisel_lemire(), ms_clzll(), ms_hpd_lshift_num_new_digits(), ms_hpd_rounded_integer(), ms_hpd_small_lshift(), ms_hpd_small_rshift() (+3 more)

### Community 43 - "Rand"
Cohesion: 0.17
Nodes (6): package_dir(), fixture, Rand, Random source, pulled out into fixture with repr so the seed is displayed on…, str(n) -> random string of length `n`. str(n, m) -> random string between…, random bytes of length `n`

### Community 45 - "TestHash"
Cohesion: 0.18
Nodes (3): FrozenPoint, TestHash, TestSetAttr

### Community 46 - "TestStructGC"
Cohesion: 0.17
Nodes (3): skipif, Copying doesn't go through the struct constructor, TestStructGC

### Community 47 - "structtype — AGENTS.md"
Cohesion: 0.18
Nodes (10): Commands, Conventions, Dict & Iteration Protocol, Gotchas, graphify, Key API, Project, Setup (+2 more)

### Community 48 - "TestStrings"
Cohesion: 0.18
Nodes (3): Exercise all the branches in the unrolled loops in the JSON str encoding…, A test designed to get full coverage of the unrolled loops in the string…, TestStrings

### Community 52 - "structtype_geojson.py"
Cohesion: 0.20
Nodes (9): Feature, FeatureCollection, GeometryCollection, LineString, MultiLineString, MultiPoint, MultiPolygon, Point (+1 more)

### Community 58 - "strbuilder_build"
Cohesion: 0.57
Nodes (8): Field_repr(), _meta_repr_part(), strbuilder_build(), strbuilder_extend(), strbuilder_extend_unicode(), strbuilder_reset(), Struct_repr(), strbuilder

### Community 59 - "test_cpylint.py"
Cohesion: 0.25
Nodes (7): fixture, This file contains some simple linters for catching some common but easy to…, Ensure all code that calls `Py_EnterRecursiveCall` doesn't return without…, Ensure all code that calls `Py_ReprEnter` doesn't return without calling…, source(), test_recursive_call_blocks(), test_recursive_repr_blocks()

### Community 60 - "test_free_threading.py"
Cohesion: 0.36
Nodes (5): Point, Struct, test_dump_json(), test_import_works(), test_validate_json()

### Community 61 - "utils.py"
Cohesion: 0.25
Nodes (4): test_is_struct_runtime(), test_struct_abc_via_init_subclass_and__abc_init(), Base, Base2

### Community 62 - "structtype"
Cohesion: 0.29
Nodes (6): Benchmarks, Documentation, Install, License, Quick Example, structtype

### Community 63 - "TestDecimal"
Cohesion: 0.29
Nodes (3): Most decimal tests are in test_common.py, the ones here are for json specific…, Check that decimal strings that `decimal.Decimal` will happily parse but aren't…, TestDecimal

### Community 64 - "test_JSONTestSuite.py"
Cohesion: 0.38
Nodes (6): _case_param(), _max_container_depth(), parametrize, These test cases are from https://github.com/nst/JSONTestSuite. They don't…, test_invalid(), test_valid()

### Community 66 - "LiteralType"
Cohesion: 0.33
Nodes (6): LiteralType, A type corresponding to a `typing.Literal` type. Parameters ---------- values:…, test_bool_literal(), test_int_literal(), test_mixed_literal(), test_str_literal()

### Community 70 - "bench_structs.py"
Cohesion: 0.70
Nodes (4): bench(), format_table(), main(), print_header()

### Community 71 - "Changelog"
Cohesion: 0.40
Nodes (4): 0.1.0 (2026-07-26), 0.2.0 (2026-07-27), 0.3.0 (2026-08-02), Changelog

### Community 73 - "EnumType"
Cohesion: 0.50
Nodes (4): EnumType, A type corresponding to an `enum.Enum` type. Parameters ---------- cls: type…, test_enum(), test_int_enum()

### Community 76 - "Struct_hash"
Cohesion: 0.67
Nodes (3): Py_hash_t, Field_hash(), Struct_hash()

### Community 78 - "BoolType"
Cohesion: 0.67
Nodes (3): BoolType, A type corresponding to `bool`., test_bool()

## Knowledge Gaps
- **47 isolated node(s):** `Point`, `PointGCFalse`, `Item_st`, `Order_st`, `Item_ms` (+42 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **31 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Field_repr()` connect `strbuilder_build` to `_core.c`, `Field`?**
  _High betweenness centrality (0.071) - this node is a cross-community bridge._
- **Why does `extract_field_from_annotated()` connect `StructspecState` to `_core.c`, `Field`?**
  _High betweenness centrality (0.066) - this node is a cross-community bridge._
- **Why does `constraints_update()` connect `structtype_get_global_state` to `_core.c`, `Field`?**
  _High betweenness centrality (0.063) - this node is a cross-community bridge._
- **Are the 89 inferred relationships involving `Field` (e.g. with `test_json_schema_constrained()` and `test_validate_json_constrained()`) actually correct?**
  _`Field` has 89 INFERRED edges - model-reasoned connections that need verification._
- **What connects `Point`, `PointGCFalse`, `Item_st` to the rest of the system?**
  _47 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `_core.c` be split into smaller, more focused modules?**
  _Cohesion score 0.05543345543345543 - nodes in this community are weakly interconnected._
- **Should `test_schema.py` be split into smaller, more focused modules?**
  _Cohesion score 0.03259493670886076 - nodes in this community are weakly interconnected._