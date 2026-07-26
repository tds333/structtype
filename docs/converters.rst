Converters
==========

.. currentmodule:: structtype

``structtype.Struct`` provides builtin support for ``Python`` and ``Json``
validating and dumping.
Support for additional protocols may be
added by combining a serialization library with structtype's *converter
functions*: `struct_dump` and `struct_validate`.

- `struct_dump`: takes an object composed of any :doc:`supported type
  <supported-types>` and converts it into one composed of only simple builtin
  types typically supported by Python serialization libraries.

- `struct_validate`: takes an object composed of any :doc:`supported type
  <supported-types>`, and converts it to match a specified schema (validating
  along the way). If the conversion fails due to a schema mismatch, a nice
  error message is raised.

These functions are designed to be paired with a Python serialization library as
pre/post processors for typical ``dumps`` and ``loads`` functions.

For example, if ``structtype`` didn't already provide support for ``json``, you
could add support by wrapping the standard library's `json` module as follows:

.. code-block:: ipython

    In [1]: import json
       ...: from typing import Any
       ...:
       ...: from structtype import Struct

    In [2]: def encode(obj):
       ...:     return json.dumps(obj.struct_dump())

    In [3]: def decode(msg, cls):
       ...:     return cls.struct_validate(json.loads(msg))

    In [4]: class Point(Struct):
       ...:     x: int
       ...:     y: int

    In [5]: x = Point(1, 2)

    In [6]: msg = encode(x)  # Encoding a high-level type works

    In [7]: msg
    '{"x": 1, "y": 2}'

    In [8]: decode(msg, Point)  # Decoding a high-level type works
    Point(x=1, y=2)

    In [9]: decode('{"x": "oops", "y": 2}', Point)  # Schema mismatches error
    ---------------------------------------------------------------------------
    ValidationError                           Traceback (most recent call last)
    Cell In[9], line 1
    ----> 1 decode('{"x": "oops", "y": 2}', Point)  # Schema mismatches error

    Cell In[3], line 2, in decode(msg, cls)
         1 def decode(msg, cls):
    ---> 2     return cls.struct_validate(json.loads(msg))

    ValidationError: Expected `int`, got `str` - at `$.x`


Since all serialization targets differ, `struct_dump` and `struct_validate` have
several configuration options:

- ``builtin_types``: an iterable of additional types to treat as builtin types,
  beyond the standard `dict`, `list`, `tuple`, `set`, `frozenset`, `str`,
  `int`, `float`, `bool`, and `None`.

- ``str_keys``: whether the wrapped protocol only supports strings for object
  keys, rather than any hashable type.

- ``strict``: `struct_validate` only. Whether type coercion rules should be strict.
  Defaults is True, setting to False enables a wider set of coercion rules from
  string to non-string types for all values. Among other uses, this may be used
  to handle completely untyped protocols like URL querystrings, where only
  string values exist. See :ref:`strict-vs-lax` for more information.

- ``from_attributes``: `convert` only. If True, input objects may be coerced
  to ``Struct`` / ``dataclass`` / ``attrs`` types by extracting attributes from the
  input matching fields in the output type. One use case is converting database
  query results (ORM or otherwise) to structtype structured types. The default is
  False.

- ``enc_hook`` / ``dec_hook``: the standard keyword arguments used for
  :doc:`extending` structtype to support additional types.


StructAdapter
-------------

For encoding and validating plain types without defining a full ``Struct``
subclass, use :class:`StructAdapter`. It wraps any supported type and
provides `struct_dump_json`, `struct_validate_json`,
`struct_dump`, and `struct_validate` methods:

.. code-block:: python

    >>> from structtype import StructAdapter

    >>> adapter = StructAdapter(list[int])
    >>> adapter.struct_validate_json(b"[1, 2, 3]")
    [1, 2, 3]

    >>> adapter.struct_validate_json(b'[1, 2, "oops"]')
    Traceback (most recent call last):
        ...
    structtype.ValidationError: Expected `int`, got `str` - at `$[2]`

    >>> StructAdapter(dict[str, int]).struct_dump_json({"a": 1})
    b'{"a":1}'

This is used throughout the :doc:`supported-types <supported-types>`
documentation to demonstrate structtype's type handling without
creating named ``Struct`` subclasses for every example.


StrAdapter
----------

For types that validate from and serialize to strings, :class:`StrAdapter`
provides a simpler alternative to ``enc_hook`` / ``dec_hook``. It creates a
``str`` subclass that validates values via the wrapped type's constructor:

.. code-block:: python

    >>> from structtype import StrAdapter, Struct
    >>> from pydantic import HttpUrl
    >>> from ipaddress import IPv4Address

    >>> class Config(Struct):
    ...     url: StrAdapter(HttpUrl)
    ...     ip: StrAdapter(IPv4Address)

    >>> Config.struct_validate_json(
    ...     b'{"url": "https://example.com", "ip": "10.0.0.1"}'
    ... )
    Config(url='https://example.com/', ip='10.0.0.1')

The wrapped type must accept a single string argument in its constructor
and raise an error if the value is invalid. Compatible with pydantic's
``HttpUrl``, ``PostgresDsn``, ``AnyUrl``, and stdlib's ``IPv4Address``,
``IPv6Address``.
