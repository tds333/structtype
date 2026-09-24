# structtype — AGENTS.md

## Project

Fast struct validation + JSON serialization for Python.
Core is a monolithic C extension (`src/structtype/_core.c`, ~22K lines).
No runtime deps.

## Setup

```bash
uv sync --frozen
```

## Commands

Use `make` targets where available. Targeted tests can be run directly with
`uv run` as shown below.

| Task | Command |
|---|---|
| Unit tests (reinstall + full suite) | `make test` |
| Targeted tests | `uv run --reinstall pytest tests/test_json.py -k test_something` |
| Coverage (Python) | `make test-cov` |
| Coverage (Python + C) | `make test-cov-c` |
| Coverage (Python + C, all Pythons, merged) | `make test-cov-c-all` |
| Tests in all supported Pythons | `make test-all` |
| Doctests | `make test-doc` |
| Build sdist + wheel | `make build` |
| Build docs | `make docs` |
| Format | `make format` |
| Lint | `make ruff-check` |
| Type check | `make type-check` |
| Static checks | `make check` |

Benchmark targets: `make bench`, `make bench-validators`, `make bench-codecs`,
`make bench-field-types`, `make bench-csv-1m`, `make bench-strings`
(run sequentially; CPU-bound; never in parallel).

Benchmarks run on free-threaded CPython (3.15t) via `BENCH_PYTHON` (override
with `make bench BENCH_PYTHON=3.15`); see `docs/benchmarks.rst`.

## Conventions

- **88-char lines**, formatted with `ruff format`
- `ruff check` with Ruff's default rule set (the project config sets only
  `target-version` and isort `combine-as-imports`; line length is enforced by
  `ruff format`, not `E501`)
- Private modules/functions prefixed with `_`
- C code uses `ms_`/`MS_` prefix
- Type stubs (`.pyi`) alongside public modules
- Sentinel values: `NODEFAULT`, `UNSET`, `_NoDefault`, `UnsetType`
- never do git commit
- check for performance regressions, speed is a goal
- no parallel benchmarks
- keep C code simple, fast, readable and threadsafe

## Key API

- `structtype.Struct` — base class with config options (frozen, tag, rename, etc.)
- `structtype.Field` — field metadata (alias, title, description, examples, deprecated, json_schema_extra)
- `structtype.Constraint` — base constraint (callable `fn`); subclasses: `NumericConstraint`, `StrConstraint`, `BytesConstraint`, `CollectionConstraint`, `TimezoneConstraint`
- `structtype.Serializer` — load/dump codecs for supported custom and native types
- `structtype.fields(type_or_instance)` — get FieldInfo tuple for a struct type/instance
- `structtype._inspect.type_info()` / `multi_type_info()` — type introspection
- `structtype.StructAdapter` — validate and serialize values against arbitrary types

### Struct Methods

- `obj.struct_dump_json(*, decimal_as_number=False, uuid_as_hex=False, sort_keys=False)` — serialize to JSON bytes
- `obj.struct_dump(*, sort_keys=False, str_keys=False, builtin_types=None)` — convert to built-in Python types (uses `alias` for keys)
- `obj.struct_check_types()` — validate field values against types + constraints (pure type-check, no conversion)
- `cls.struct_validate_json(buf, *, strict=True)` — deserialize from JSON
- `cls.struct_validate(obj, *, strict=True, from_attributes=False)` — convert built-in types to struct
- `obj.struct_dump_csv()` — encode one row as `list[str]` for `csv.writer.writerow()`; flat scalar fields only, positional
- `cls.struct_validate_csv(row, *, null_values=("",))` — decode one CSV row (sequence of cell strings); always lax

### Dict & Iteration Protocol

Struct instances support the mapping protocol:
- `dict(p)` — shallow dict of Python field names to values (iterates `(name, value)` pairs)
- `list(p)` / `iter(p)` — iterate `(name, value)` 2-tuples in declaration order

## Gotchas

- `make test-cov` reinstalls the C extension before running. `make test-cov-c` builds an `-O0 --coverage` instrumented extension **in place**; afterwards any reinstalling target (`make test`, `make test-cov`) restores the optimized build.
- C coverage requires `lcov`/`genhtml`; report lands in `htmlcov-c/`.
- `make check` runs static type and Ruff checks only; it does not run tests,
  documentation builds, or formatting.
- Validation matches keys by the **alias** name only, except `struct_validate(obj, from_attributes=True)` on a **non-dict object**, which matches by both the python field name and the alias. Dict/JSON input (even with `from_attributes=True`) and all dump/serialization use only the alias name.
- For `Annotated[MyType, Serializer(load=...)]`, an existing `MyType` instance bypasses `load`; other Python values and JSON representations are passed to `load`. Serializers remain rejected on `Any`, `bool`, `int`, `float`, `str`, `list`, `dict`, `tuple`, `TypedDict`, `NamedTuple`, `frozendict`, and `Literal`. `Serializer` and `Constraint` must attach to a concrete type inside `Annotated` — `Annotated[Union[A, B], ...]` and `Annotated[Optional[T], ...]` are rejected; use `Annotated[T, ...] | None` for optional fields.

