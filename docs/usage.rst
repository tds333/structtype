Usage
=====

``structtype`` provides a single serialization protocol: JSON. All JSON
operations are available as methods on ``Struct`` instances, or via
``StructAdapter`` for encoding non-struct objects.

Encoding
--------

Struct instances are encoded to JSON using ``struct_dump_json()``:

.. code-block:: python

    >>> from structtype import Struct

    >>> class User(Struct):
    ...     name: str
    ...     groups: set[str] = set()
    ...     email: str | None = None

    >>> alice = User("alice")
    >>> alice.struct_dump_json()
    b'{"name":"alice","groups":[],"email":null}'

For encoding non-struct objects, ``StructAdapter`` can be used:

.. code-block:: python

    >>> from structtype import StructAdapter

    >>> StructAdapter(dict).struct_dump_json({"hello": "world"})
    b'{"hello":"world"}'

Decoding
--------

JSON is decoded into a struct instance using ``struct_validate_json()``:

.. code-block:: python

    >>> User.struct_validate_json(b'{"name":"alice","groups":[],"email":null}')
    User(name='alice', groups=set(), email=None)


.. _typed-decoding:

Typed Decoding
--------------

``structtype`` validates data during decoding against the struct's type
annotations. If a message doesn't match the expected type, an error is raised
with a clear message:

.. code-block:: python

    >>> User.struct_validate_json(
    ...     b'{"name": "bill", "groups": ["devops", 123]}'
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `str`, got `int` - at `$.groups[1]`

.. _strict-vs-lax:

"Strict" vs "Lax" Mode
~~~~~~~~~~~~~~~~~~~~~~

``structtype`` won't perform unsafe implicit conversion by default ("strict"
mode). For example, if an integer is specified and a string is provided instead,
an error is raised rather than casting:

.. code-block:: python

    >>> User.struct_validate_json(
    ...     b'{"name":"alice","groups":[1,2,3]}'
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `str`, got `int` - at `$.groups[0]`

For cases where you'd like a more lax set of conversion rules, pass
``strict=False``:

.. code-block:: python

    >>> User.struct_validate_json(
    ...     b'{"name":"alice","groups":["admin"],"email":null}',
    ...     strict=False
    ... )
    User(name='alice', groups={'admin'}, email=None)

See :doc:`supported-types` for how lax mode affects individual types.

.. _to-builtins-vs-asdict:

Converting to and from Builtin Types
------------------------------------

In some cases, ``structtype`` only needs to process part of a message, and the
rest is handled by another library. For these situations, ``struct_dump`` and
``struct_validate`` convert between high-level types and plain builtin types
(`dict`, `list`, `str`, `int`, ...) without going through an encoded
representation.

- ``struct_dump`` is the "encoding" half. It applies the same semantics as
  ``struct_dump_json`` — just with builtin Python types as the output rather
  than JSON bytes. This includes:

  - Struct-level settings: ``rename``, :ref:`omit_defaults`, ``array_like``,
    and ``tag`` for :ref:`tagged unions <struct-tagged-unions>`.
  - Omission of :ref:`UNSET <unset-type>` fields.
  - Recursive expansion of nested `structtype.Struct`, `dataclasses.dataclass`,
    attrs_, `typing.TypedDict`, and `typing.NamedTuple` values.
  - Value-level conversions of types that don't map directly to builtin
    types: `bytes` / `bytearray` / `memoryview` to base64 string,
    `datetime.datetime` / `datetime.date` / `datetime.time` /
    `datetime.timedelta` to ISO 8601 string, `uuid.UUID` and
    `decimal.Decimal` to string, `set` / `frozenset` to `list`,
    ``frozendict`` to `dict`, `enum.Enum` to its member value.

- ``struct_validate`` is the "decoding" half: it takes builtin types and
  validates them against a schema, producing high-level types.

.. code-block:: python

    >>> from structtype import Struct

    >>> class User(Struct, omit_defaults=True):
    ...     name: str
    ...     groups: set[str] = set()
    ...     email: str | None = None

    >>> alice = User("alice")

    >>> # struct_dump applies omit_defaults and expands nested types
    ... alice.struct_dump()
    {'name': 'alice'}

    >>> # struct_validate is the inverse operation
    ... User.struct_validate({"name": "bill", "groups": ["devops"]})
    User(name='bill', groups={'devops'}, email=None)

See :doc:`converters` for a more detailed guide, including how to use these
functions to add ``structtype`` support for additional serialization protocols.

Note that ``dict(struct_instance)`` is *not* equivalent to
``struct_dump``. It performs a one-to-one conversion of a single struct
instance to a `dict` using the raw attribute names.

None of the semantics listed above apply. Every field is included regardless
of ``omit_defaults`` or `structtype.UNSET`, ``rename`` and ``tag`` are ignored,
nested `structtype.Struct` / `dataclasses.dataclass` / attrs_ values are left
as-is, and value-level types (`bytes`, `datetime.datetime`, `uuid.UUID`,
`decimal.Decimal`, `enum.Enum`, ...) are not converted.

Prefer ``struct_dump`` when the output is intended for serialization.

.. _type annotations: https://docs.python.org/3/library/typing.html
.. _pydantic: https://pydantic.dev/docs/validation/latest/get-started/
.. _mypy: https://mypy.readthedocs.io/en/stable/
.. _pyright: https://github.com/microsoft/pyright
.. _attrs: https://www.attrs.org/en/stable/index.html
