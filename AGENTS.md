# structtype — AGENTS.md

## Project

Fast struct validation + JSON serialization for Python.
Core is a monolithic C extension (`src/structtype/_core.c`, ~20K lines).
No runtime deps.

## Setup

```bash
uv sync --frozen
```

## Commands

All commands go through `make`.

| Task | Command |
|---|---|
| Unit tests (reinstall + last-failed) | `make test-lf` |
| Targeted tests | `uv run --reinstall pytest tests/unit/test_json.py -k test_something` |
| Coverage | `make test-cov` |
| Build docs | `make docs` |
| Format | `make format` |
| Lint | `make ruff-check` |
| Type check | `make type-check` |
| All checks | `make check` |

## Conventions

- **88-char lines**, formatted with `ruff format`
- `ruff check` with rules `E`, `F`, `I`, `W`
- Private modules/functions prefixed with `_`
- C code uses `ms_`/`MS_` prefix
- Type stubs (`.pyi`) alongside public modules
- Sentinel values: `NODEFAULT`, `UNSET`, `_NoDefault`, `UnsetType`

## Key API

- `structtype.Struct` — base class with config options (frozen, tag, rename, etc.)
- `structtype.Field` — field constraints (gt, ge, lt, le, min_length, etc.)
- `structtype.Raw` — lazy JSON passthrough
- `structtype._inspect.type_info()` / `multi_type_info()` — type introspection

### Struct Methods

- `obj.struct_dump_json()` — serialize to JSON bytes
- `obj.struct_dump()` — convert to built-in Python types
- `obj.struct_to_dict()` — shallow field dict
- `obj.struct_to_tuple()` — shallow field tuple
- `obj.struct_force_setattr(name, value)` — set attr on frozen struct
- `obj.struct_check()` — validate field values against types + constraints
- `cls.struct_validate_json(buf)` — deserialize from JSON
- `cls.struct_validate_jsonln(buf)` — deserialize newline-delimited JSON to a list of structs
- `cls.struct_dump_jsonln(items)` — serialize a list of structs as newline-delimited JSON
- `cls.struct_validate(obj)` — convert built-in types to struct

## Gotchas

- `pytest-randomly` shuffles test order
- `make test-lf` reinstalls the C extension before running (last-failed first)
