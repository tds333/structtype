Constraints
===========

When using :ref:`typed-decoding` ``structtype`` will ensure decoded
messages match the specified types. For example, to decode a list of integers
from JSON:

.. code-block:: python

    >>> from structtype import StructAdapter

    >>> StructAdapter(list[int]).struct_validate_json(b"[1, 2, 3]")
    [1, 2, 3]

    >>> StructAdapter(list[int]).struct_validate_json(b'[1, 2, "oops"]')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int`, got `str` - at `$[2]`

Often this is sufficient, but sometimes you also need to impose constraints on
the *values* (rather than the *types*) found in the message.

Constraints in ``structtype`` are specified by wrapping a type with
`typing.Annotated`, and adding a `structtype.Field` annotation.

For example, to constrain the list to positive integers (``> 0``), you'd make
use of the ``gt`` (greater-than) constraint:

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Field, StructAdapter

    >>> PositiveInt = Annotated[int, Field(gt=0)]

    >>> StructAdapter(list[PositiveInt]).struct_validate_json(b'[1, 2, 3]')
    [1, 2, 3]

    >>> StructAdapter(list[PositiveInt]).struct_validate_json(b'[1, 2, -1]')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int` >= 1 - at `$[2]`

Constraints can be combined to enforce complex requirements. Here's a more
complete example enforcing the following constraints on a ``User`` struct:

- ``name`` is a ``str`` with ``1 <= length <= 32`` matching the regular
  expression ``"^[a-z_][a-z0-9_-]*$"``.
- ``groups`` is a ``set`` of at most 16 strings, each with the same constraints
  as ``name`` above, defaulting to the empty ``set``.
- ``cpu_limit`` is a ``float`` with a value ``>= 0.1`` and ``<= 8``, defaulting
  to 1.
- ``mem_limit`` is an ``int`` with a value ``>= 256`` and ``<= 8192``,
  defaulting to 1024.

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, Field

    >>> UnixName = Annotated[
    ...     str, Field(min_length=1, max_length=32, pattern="^[a-z_][a-z0-9_-]*$")
    ... ]

    >>> class User(Struct):
    ...     name: UnixName
    ...     groups: Annotated[set[UnixName], Field(max_length=16)] = set()
    ...     cpu_limit: Annotated[float, Field(ge=0.1, le=8)] = 1
    ...     mem_limit: Annotated[int, Field(ge=256, le=8192)] = 1024

As shown above, ``Annotated`` types can applied inline, or used to create type
aliases and then reused elsewhere (as done with ``UnixName``).

The following constraints are supported:

Numeric Constraints
-------------------

These constraints are valid on `int` or `float` types:

- ``ge``: The value must be greater than or equal to ``ge``.
- ``gt``: The value must be greater than ``gt``.
- ``le``: The value must be less than or equal to ``le``.
- ``lt``: The value must be less than ``lt``.
- ``multiple_of``: The value must be a multiple of ``multiple_of``.

.. code-block:: python

    >>> from structtype import StructAdapter

    >>> from typing import Annotated
    >>> from structtype import Field

    >>> StructAdapter(Annotated[int, Field(ge=0)]).struct_validate_json(b'-1')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int` >= 0

.. warning::

    While ``multiple_of`` works on ``float`` types, we don't recommend
    specifying *non-integral* ``multiple_of`` constraints, as they may be
    erroneously marked as invalid due to floating point precision issues. For
    example, annotating a ``float`` type with ``multiple_of=10`` is fine, but
    ``multiple_of=0.1`` may lead to issues. See `this GitHub issue
    <https://github.com/json-schema-org/json-schema-spec/issues/312>`_ for more
    details.

String Constraints
------------------

These constraints are valid on `str` types:

- ``min_length``: The minimum valid length, inclusive.
- ``max_length``: The maximum valid length, inclusive.
- ``pattern``: A regular expression pattern that the value must match. Note
  that patterns are treated as *unanchored*. This means that the pattern "es"
  matches not just "es" but also "expression". If required, you must explicitly
  anchor the pattern by adding a "^" prefix and "$" suffix. For example, the
  pattern "^es$" only matches the string "es"

.. code-block:: python

    >>> from structtype import StructAdapter

    >>> from typing import Annotated
    >>> from structtype import Field

    >>> StructAdapter(Annotated[str, Field(pattern="^[a-z0-9_]*$")]).struct_validate_json(
    ...     b'"invalid username"',
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `str` matching regex '^[a-z0-9_]*$'

.. _datetime-constraints:

Datetime Constraints
--------------------

These constraints are valid on `datetime.datetime` and `datetime.time` types:

- ``tz``: Whether the annotated type is required to be timezone-aware_. Set to
  ``True`` to require timezone-aware values, or ``False`` to require
  timezone-naive values. The default is ``None``, which accepts either
  timezone-aware or timezone-naive values.

.. code-block:: python

    >>> from structtype import StructAdapter

    >>> from datetime import datetime

    >>> from typing import Annotated
    >>> from structtype import Field

    >>> StructAdapter(Annotated[datetime, Field(tz=True)]).struct_validate_json(
    ...     b'"2022-04-02T18:18:10"',
    ... )  # require timezone aware
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `datetime` with a timezone component

    >>> StructAdapter(Annotated[datetime, Field(tz=False)]).struct_validate_json(
    ...     b'"2022-04-02T18:18:10-06:00"',
    ... )  # require timezone naive
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `datetime` with no timezone component

Bytes Constraints
-----------------

These constraints are valid on `bytes` and `bytearray` types:

- ``min_length``: The minimum valid length, inclusive.
- ``max_length``: The maximum valid length, inclusive.

.. code-block:: python

    >>> from structtype import StructAdapter

    >>> from typing import Annotated
    >>> from structtype import Field

    >>> StructAdapter(Annotated[bytes, Field(min_length=10)]).struct_validate_json(
    ...     b'"ZXhhbXBsZQ=="',
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `bytes` of length >= 10

Sequence Constraints
--------------------

These constraints are valid on `list`, `tuple`, `set`, and `frozenset` types:

- ``min_length``: The minimum valid length, inclusive.
- ``max_length``: The maximum valid length, inclusive.

.. code-block:: python

    >>> from structtype import StructAdapter

    >>> from typing import Annotated
    >>> from structtype import Field

    >>> StructAdapter(Annotated[list[int], Field(max_length=3)]).struct_validate_json(
    ...     b'[1, 2, 3, 4]',
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `array` of length <= 3

Mapping Constraints
-------------------

These constraints are valid on `dict` types:

- ``min_length``: The minimum valid length, inclusive.
- ``max_length``: The maximum valid length, inclusive.

.. code-block:: python

    >>> from structtype import StructAdapter

    >>> from typing import Annotated
    >>> from structtype import Field

    >>> StructAdapter(Annotated[dict[str, int], Field(max_length=3)]).struct_validate_json(
    ...     b'{"a": 1, "b": 2, "c": 3, "d": 4}',
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `object` of length <= 3

.. _timezone-aware: https://docs.python.org/3/library/datetime.html#aware-and-naive-objects
