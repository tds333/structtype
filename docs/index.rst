structtype
==========

``structtype`` is a *fast* struct validation and JSON serialization library for
Python. It features:

- 🚀 **High performance JSON encoder/decoder**. Regularly :doc:`benchmarks
  <benchmarks>` as the fastest option for Python.

- 🎉 **Support for a wide variety of Python types**. Additional types may
  be supported through :doc:`extensions <extending>`.

- 🔍 **Zero-cost schema validation** using familiar Python type annotations.
  In :doc:`benchmarks <benchmarks>` ``structtype`` decodes *and* validates JSON
  faster than orjson_ can decode it alone.

- ✨ **A speedy Struct type** for representing structured data. If you already
  use dataclasses_ or attrs_, :doc:`structs` should feel familiar. However,
  they're :ref:`5-60x <struct-benchmark>` faster for common operations.

All of this is included in a lightweight library
with no required dependencies.

-----

**Define** your message schemas using standard Python type annotations.

.. code-block:: python

    >>> from structtype import Struct

    >>> class User(Struct):
    ...     """A new type describing a User"""
    ...     name: str
    ...     groups: set[str] = set()
    ...     email: str | None = None

**Encode** and **decode** messages as JSON, with optional schema validation.

.. code-block:: python

    >>> alice = User("alice", groups={"admin", "engineering"})

    >>> alice
    User(name='alice', groups={"admin", "engineering"}, email=None)

    >>> alice.struct_dump_json()
    b'{"name":"alice","groups":["admin","engineering"],"email":null}'

    >>> User.struct_validate_json(
    ...     b'{"name":"alice","groups":["admin","engineering"],"email":null}'
    ... )
    User(name='alice', groups={"admin", "engineering"}, email=None)

    >>> User.struct_validate_json(b'{"name":"bob","groups":[123]}')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `str`, got `int` - at `$.groups[0]`

``structtype`` is designed to be as performant as possible, while retaining some
of the nicities of validation libraries like pydantic_. For supported types,
encoding/decoding a message with ``structtype`` can be :doc:`~10-80x faster than
alternative libraries <benchmarks>`.

Highlights
----------

- ``structtype`` is **fast**. It :doc:`benchmarks <benchmarks>` as the fastest
  serialization library for Python, outperforming all other JSON libraries
  compared.

- ``structtype`` is **friendly**. Through use of Python's type annotations,
  messages are :ref:`validated <typed-decoding>` during deserialization in a
  declarative way. ``structtype`` also works well with other type-checking tooling
  like mypy_ and pyright_, providing excellent editor integration.

- ``structtype`` is **flexible**. It natively supports a :doc:`wide range of
  Python builtin types <supported-types>`. Support for additional types can
  also be added through :doc:`extensions <extending>`.

- ``structtype`` is **lightweight**. It has no required dependencies, and the
  binary size is a fraction of that of comparable libraries.

- ``structtype`` is **correct**. The JSON encoder/decoder is strictly compliant
  with the JSON specification, providing strong guarantees of compatibility.

.. _type annotations: https://docs.python.org/3/library/typing.html
.. _JSON: https://json.org
.. _attrs: https://www.attrs.org/en/stable/
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _orjson: https://github.com/ijl/orjson
.. _pydantic: https://pydantic.dev/docs/validation/latest/get-started/
.. _mypy: https://mypy.readthedocs.io/en/stable/
.. _pyright: https://github.com/microsoft/pyright

.. toctree::
    :hidden:
    :maxdepth: 2
    :caption: Overview

    why.rst
    install.rst
    benchmarks.rst

.. toctree::
    :hidden:
    :maxdepth: 2
    :caption: User Guide

    usage.rst
    supported-types.rst
    structs.rst
    constraints.rst
    converters.rst
    jsonschema.rst
    schema-evolution.rst

.. toctree::
    :hidden:
    :maxdepth: 2
    :caption: Advanced

    extending.rst
    perf-tips.rst

.. toctree::
    :hidden:
    :maxdepth: 2
    :caption: Reference

    api.rst
    examples/index.rst
    changelog.md
