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
- `structtype.fields(type_or_instance)` — get FieldInfo tuple for a struct type/instance
- `structtype._inspect.type_info()` / `multi_type_info()` — type introspection

### Struct Methods

- `obj.struct_dump_json(*, enc_hook=None, decimal_format=None, uuid_format=None, order=None)` — serialize to JSON bytes
- `obj.struct_dump()` — convert to built-in Python types (uses `encode_name` for keys)
- `obj.struct_force_setattr(name, value)` — set attr on frozen struct
- `obj.struct_validate_self(*, strict=True, dec_hook=None)` — validate field values against types + constraints
- `cls.struct_validate_json(buf, *, strict=True, dec_hook=None)` — deserialize from JSON
- `cls.struct_validate(obj, *, strict=True, from_attributes=False, dec_hook=None)` — convert built-in types to struct

### Dict & Iteration Protocol

Struct instances support the mapping protocol:
- `dict(p)` — shallow dict of Python field names to values (iterates `(name, value)` pairs)
- `list(p)` / `iter(p)` — iterate `(name, value)` 2-tuples in declaration order

## Gotchas

- `make test-lf` reinstalls the C extension before running (last-failed first)
