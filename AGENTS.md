# structtype — AGENTS.md

## Project

Fast struct validation + JSON serialization for Python.
Core is a monolithic C extension (`src/structtype/_core.c`, ~19K lines).
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
| Targeted tests | `uv run --reinstall pytest tests/test_json.py -k test_something` |
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
- never do git commit

## Key API

- `structtype.Struct` — base class with config options (frozen, tag, rename, etc.)
- `structtype.Field` — field constraints (gt, ge, lt, le, min_length, etc.)
- `structtype.Raw` — lazy JSON passthrough
- `structtype.fields(type_or_instance)` — get FieldInfo tuple for a struct type/instance
- `structtype._inspect.type_info()` / `multi_type_info()` — type introspection

### Struct Methods

- `obj.struct_dump_json(*, decimal_format=None, uuid_format=None, sort_keys=False)` — serialize to JSON bytes
- `obj.struct_dump(*, sort_keys=False, str_keys=False, builtin_types=None)` — convert to built-in Python types (uses `alias` for keys)
- `obj.struct_validate_self()` — validate field values against types + constraints
- `cls.struct_validate_json(buf, *, strict=True)` — deserialize from JSON
- `cls.struct_validate(obj, *, strict=True, from_attributes=False)` — convert built-in types to struct

### Dict & Iteration Protocol

Struct instances support the mapping protocol:
- `dict(p)` — shallow dict of Python field names to values (iterates `(name, value)` pairs)
- `list(p)` / `iter(p)` — iterate `(name, value)` 2-tuples in declaration order

## Gotchas

- `make test-cov` reinstalls the C extension before running (last-failed first)
- Validation matches keys by the **alias** name only, except `struct_validate(obj, from_attributes=True)` on a **non-dict object**, which matches by both the python field name and the alias. Dict/JSON input (even with `from_attributes=True`) and all dump/serialization use only the alias name.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

When the user types `/graphify`, use the installed graphify skill or instructions before doing anything else.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- Dirty graphify-out/ files are expected after hooks or incremental updates; dirty graph files are not a reason to skip graphify. Only skip graphify if the task is about stale or incorrect graph output, or the user explicitly says not to use it.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
