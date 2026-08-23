Field Annotations
=================

When using :ref:`typed-decoding` ``structtype`` will ensure decoded
messages match the specified types. For example, to decode a list of integers
from JSON:

.. code-block:: python

    >>> from structtype import Struct

    >>> class IntList(Struct):
    ...     items: list[int]

    >>> IntList.struct_validate_json(b'{"items": [1, 2, 3]}')
    IntList(items=[1, 2, 3])

    >>> IntList.struct_validate_json(b'{"items": [1, 2, "oops"]}')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int`, got `str` - at `$.items[2]`

Often this is sufficient, but sometimes you also need to impose constraints on
the *values* (rather than the *types*) found in the message.

Constraints and field metadata in ``structtype`` are specified by wrapping a
type with `typing.Annotated`, and adding a `structtype.Field` or
`structtype.Validator` annotation. The ``Annotated`` wrapper attaches arbitrary
metadata to a type without changing its runtime behavior; ``structtype`` reads
the ``Field`` and ``Validator`` instances it contains.

.. note::

    ``from __future__ import annotations`` is fully supported — lazy string
    annotations are resolved at class creation, so ``Field`` metadata,
    ``Validator`` constraints, and ``Serializer`` codecs work exactly as with
    eager annotations. Forward references that can't be resolved at class
    creation (e.g. self-referential types) are left for the decode-time
    resolver.

**Constraint options** (``gt``, ``ge``, ``lt``, ``le``, ``multiple_of``,
``min_length``, ``max_length``, ``pattern``, ``tz``) restrict the *values* a
field may take, and are specified via `structtype.Validator` subclasses.
**Metadata options** (``alias``, ``title``, ``description``,
``examples``, ``json_schema_extra``) affect encoding names and generated
:doc:`JSON Schemas <jsonschema>`, and are specified via `structtype.Field`.

For example, to constrain the list to positive integers (``> 0``), you'd make
use of the ``gt`` (greater-than) constraint:

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import NumericValidator, Struct

    >>> PositiveInt = Annotated[int, NumericValidator(gt=0)]

    >>> class PosList(Struct):
    ...     items: list[PositiveInt]

    >>> PosList.struct_validate_json(b'{"items": [1, 2, 3]}')
    PosList(items=[1, 2, 3])

    >>> PosList.struct_validate_json(b'{"items": [1, 2, -1]}')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int` >= 1 - at `$.items[2]`

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
    >>> from structtype import Struct, Field, StrValidator, NumericValidator

    >>> UnixName = Annotated[
    ...     str, StrValidator(min_length=1, max_length=32, pattern="^[a-z_][a-z0-9_-]*$")
    ... ]

    >>> class User(Struct):
    ...     name: UnixName
    ...     groups: Annotated[set[UnixName], CollectionValidator(max_length=16)] = set()
    ...     cpu_limit: Annotated[float, NumericValidator(ge=0.1, le=8)] = 1
    ...     mem_limit: Annotated[int, NumericValidator(ge=256, le=8192)] = 1024

As shown above, ``Annotated`` types can applied inline, or used to create type
aliases and then reused elsewhere (as done with ``UnixName``).

``Annotated`` metadata can also be layered. A type alias may carry one kind of
annotation (e.g. a ``Validator``), and the alias can then be wrapped again with
another kind (e.g. a ``Field``) where it's used:

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, Field, Validator

    >>> def non_negative(value):
    ...     if value < 0:
    ...         raise ValueError("must be non-negative")
    ...     return value

    >>> NonNegativeInt = Annotated[int, Validator(non_negative)]

    >>> class S(Struct):
    ...     xyz: Annotated[NonNegativeInt, Field(alias="z")]

    >>> S.struct_validate({"z": 5}).xyz
    5
    >>> S.struct_validate({"z": -1})
    Traceback (most recent call last):
      ...
    structtype.ValidationError: must be non-negative - at `$.z`

At most one ``Field``, one ``Serializer``, and one ``Validator`` may apply to a
single field, across all nesting levels. Using two of the same kind (e.g. two
``Field`` wrappers) raises a ``TypeError``.

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

    >>> from typing import Annotated
    >>> from structtype import Struct, NumericValidator

    >>> class Value(Struct):
    ...     val: Annotated[int, NumericValidator(ge=0)]

    >>> Value.struct_validate_json(b'{"val": -1}')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int` >= 0 - at `$.val`

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

    >>> from typing import Annotated
    >>> from structtype import Struct, StrValidator

    >>> class UserName(Struct):
    ...     name: Annotated[str, StrValidator(pattern="^[a-z0-9_]*$")]

    >>> UserName.struct_validate_json(
    ...     b'{"name": "invalid username"}',
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `str` matching regex '^[a-z0-9_]*$' - at `$.name`

.. _datetime-constraints:

Datetime Constraints
--------------------

These constraints are valid on `datetime.datetime` and `datetime.time` types:

- ``tz``: Whether the annotated type is required to be timezone-aware_. Set to
  ``True`` to require timezone-aware values, or ``False`` to require
  timezone-naive values. The default is ``None``, which accepts either
  timezone-aware or timezone-naive values.

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, TimezoneValidator

    >>> from datetime import datetime

    >>> class EventTZ(Struct):
    ...     at: Annotated[datetime, TimezoneValidator(tz=True)]

    >>> EventTZ.struct_validate_json(
    ...     b'{"at": "2022-04-02T18:18:10"}',
    ... )  # require timezone aware
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `datetime` with a timezone component - at `$.at`

    >>> class EventNaive(Struct):
    ...     at: Annotated[datetime, TimezoneValidator(tz=False)]

    >>> EventNaive.struct_validate_json(
    ...     b'{"at": "2022-04-02T18:18:10-06:00"}',
    ... )  # require timezone naive
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `datetime` with no timezone component - at `$.at`

Bytes Constraints
-----------------

These constraints are valid on `bytes` and `bytearray` types:

- ``min_length``: The minimum valid length, inclusive.
- ``max_length``: The maximum valid length, inclusive.

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, BytesValidator

    >>> class Payload(Struct):
    ...     data: Annotated[bytes, BytesValidator(min_length=10)]

    >>> Payload.struct_validate_json(
    ...     b'{"data": "ZXhhbXBsZQ=="}',
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `bytes` of length >= 10 - at `$.data`

Sequence Constraints
--------------------

These constraints are valid on `list`, `tuple`, `set`, and `frozenset` types:

- ``min_length``: The minimum valid length, inclusive.
- ``max_length``: The maximum valid length, inclusive.

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, CollectionValidator

    >>> class SmallList(Struct):
    ...     items: Annotated[list[int], CollectionValidator(max_length=3)]

    >>> SmallList.struct_validate_json(
    ...     b'{"items": [1, 2, 3, 4]}',
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `array` of length <= 3 - at `$.items`

Mapping Constraints
-------------------

These constraints are valid on `dict` types:

- ``min_length``: The minimum valid length, inclusive.
- ``max_length``: The maximum valid length, inclusive.

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, CollectionValidator

    >>> class SmallDict(Struct):
    ...     items: Annotated[dict[str, int], CollectionValidator(max_length=3)]

    >>> SmallDict.struct_validate_json(
    ...     b'{"items": {"a": 1, "b": 2, "c": 3, "d": 4}}',
    ... )
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `object` of length <= 3 - at `$.items`

Field Metadata
--------------

These ``Field`` options don't constrain values — they attach metadata that
affects encoding names or generated JSON Schemas.

``alias``
~~~~~~~~~

Set an alternative name for the field, used when encoding/decoding messages.
The original name is still used in Python code. This is the field-level
alternative to the struct-level ``rename`` option; see :ref:`Renaming Fields
<renaming-fields>` in the usage guide for the full comparison.

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, Field

    >>> class Example(Struct):
    ...     x: int
    ...     y: Annotated[int, Field(alias="field_y")]

    >>> Example(1, 2).struct_dump_json()
    b'{"x":1,"field_y":2}'

    >>> Example.struct_validate_json(b'{"x": 1, "field_y": 2}')
    Example(x=1, y=2)

``title`` and ``description``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Human-readable metadata for the field, included in generated JSON Schemas:

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, Field

    >>> class Product(Struct):
    ...     name: Annotated[str, Field(
    ...         title="Product Name",
    ...         description="The display name of the product"
    ...     )]

``examples``
~~~~~~~~~~~~

Provide example values that appear in generated JSON Schemas:

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, Field

    >>> class Product(Struct):
    ...     name: Annotated[str, Field(examples=["Widget", "Gadget"])]

``deprecated``
~~~~~~~~~~~~~~

Mark a field as deprecated in the generated JSON Schema:

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, Field

    >>> class Product(Struct):
    ...     sku: Annotated[str, Field(deprecated=True)]

``json_schema_extra``
~~~~~~~~~~~~~~~~~~~~~

Add arbitrary extra properties to the generated JSON Schema for a field:

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, Field

    >>> class Product(Struct):
    ...     sku: Annotated[str, Field(json_schema_extra={"format": "sku"})]

.. _timezone-aware: https://docs.python.org/3/library/datetime.html#aware-and-naive-objects
