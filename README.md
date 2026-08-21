# structtype

[![PyPI version](https://img.shields.io/pypi/v/structtype)](https://pypi.org/project/structtype/)
[![Python versions](https://img.shields.io/pypi/pyversions/structtype)](https://pypi.org/project/structtype/)
[![Build status](https://img.shields.io/github/actions/workflow/status/tds333/structtype/build.yml?branch=main)](https://github.com/tds333/structtype/actions/workflows/build.yml)

Fast Struct type with validation + JSON serialization for Python.

- **High performance** — 5-60x faster than dataclasses, attrs, or pydantic for common operations
- **Schema validation** — familiar Python type annotations, enforced at decode time
- **No runtime dependencies** — built on a monolithic C extension based on msgspec
- **Rich type support** — nested Structs, dataclasses, TypedDicts, enums, UUID, Decimal, datetime, and more
- **JSON Schema generation** — auto-generate JSON Schema 2020-12 / OpenAPI 3.1 specs

## Install

```bash
pip install structtype
```

```bash
uv add structtype
```

Requires Python ≥ 3.10.

## Quick Example

```python
from structtype import Struct

class User(Struct):
    name: str
    groups: set[str] = set()
    email: str | None = None

alice = User("alice", groups={"admin", "engineering"})

# Serialize to JSON
alice.struct_dump_json()
# b'{"name":"alice","groups":["admin","engineering"],"email":null}'

# Deserialize and validate
User.struct_validate_json(
    b'{"name":"alice","groups":["admin","engineering"],"email":null}'
)
# User(name='alice', groups={"admin", "engineering"}, email=None)
```

## Documentation

Full documentation is available at **https://tds333.github.io/structtype/**.

## Benchmarks

structtype is as fast as msgspec and about 3-5x faster than pydantic. See the
[benchmarks page](https://tds333.github.io/structtype/benchmarks.html) for the full numbers.

## Links

- [Documentation](https://tds333.github.io/structtype/)
- [Changelog](https://tds333.github.io/structtype/changelog.html)
- [PyPI](https://pypi.org/project/structtype/)
- [Source code](https://github.com/tds333/structtype)
- [Issue tracker](https://github.com/tds333/structtype/issues)

## License

New BSD. See the [License File](LICENSE).
The core is based on the work of Jim Crist-Harif from msgspec.
