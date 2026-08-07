Supported Types
===============

``structtype`` uses Python `type annotations`_ to describe the expected types.
Most combinations of the following types are supported (with a few restrictions):

**Builtin Types**

- `None`
- `bool`
- `int`
- `float`
- `str`
- `bytes`
- `bytearray`
- `tuple` / `typing.Tuple`
- `list` / `typing.List`
- `dict` / `typing.Dict`
- `set` / `typing.Set`
- `frozenset` / `typing.FrozenSet`
- `frozendict <https://docs.python.org/3.15/library/stdtypes.html#frozendict>`__
  for Python 3.15+

**Structtype types**

- `structtype.Raw`
- `structtype.UNSET`
- `structtype.Struct` types

**Standard Library Types**

- `datetime.datetime`
- `datetime.date`
- `datetime.time`
- `datetime.timedelta`
- `uuid.UUID`
- `decimal.Decimal`
- `enum.Enum` types
- `enum.IntEnum` types
- `enum.StrEnum` types
- `enum.Flag` types
- `enum.IntFlag` types
- `dataclasses.dataclass` types

**Typing module types**

- `typing.Any`
- `typing.Optional`
- `typing.Union`
- `typing.Literal`
- `typing.NewType`
- `typing.Final`
- `typing.TypeAliasType`
- `typing.TypeAlias`
- `typing.NamedTuple` / `collections.namedtuple`
- `typing.TypedDict`
- `typing.Generic`
- `typing.TypeVar`

**Abstract types**

- `collections.abc.Collection` / `typing.Collection`
- `collections.abc.Sequence` / `typing.Sequence`
- `collections.abc.MutableSequence` / `typing.MutableSequence`
- `collections.abc.Set` / `typing.AbstractSet`
- `collections.abc.MutableSet` / `typing.MutableSet`
- `collections.abc.Mapping` / `typing.Mapping`
- `collections.abc.MutableMapping` / `typing.MutableMapping`

**Third-Party Libraries**

- attrs_ types
- pydantic_ types (inherited from BaseModel)

Additional types may be supported through :doc:`extensions <extending>`.

Note that except where explicitly stated, subclasses of these types are not
supported by default (see :doc:`extending` for how to add support yourself).

Here we document how structtype maps Python objects to/from JSON.

``None``
--------

`None` maps to the ``null`` value in JSON.

.. code-block:: python

    >>> from structtype import StructAdapter
    >>> from typing import Any

    >>> StructAdapter(Any).struct_dump_json(None)
    b'null'

    >>> StructAdapter(Any).struct_validate_json(b'null')
    None

If ``strict=False`` is specified, a string value of ``"null"`` (case
insensitive) may also be coerced to ``None``. See :ref:`strict-vs-lax` for more
information.

.. code-block:: python

   >>> StructAdapter(None).struct_validate_json(b'"null"', strict=False)
   None

``bool``
--------

Booleans map to their corresponding ``true`` / ``false`` values in JSON.

.. code-block:: python

    >>> StructAdapter(Any).struct_dump_json(True)
    b'true'

    >>> StructAdapter(Any).struct_validate_json(b'true')
    True

If ``strict=False`` is specified, values of ``"true"`` / ``"1"`` / ``1`` or
``"false"`` / ``"0"`` / ``0`` (case insensitive for strings) may also be coerced to
``True`` / ``False`` respectively. See :ref:`strict-vs-lax` for more information.

.. code-block:: python

   >>> StructAdapter(bool).struct_validate_json(b'"false"', strict=False)
   False

   >>> StructAdapter(bool).struct_validate_json(b'"TRUE"', strict=False)
   True

   >>> StructAdapter(bool).struct_validate_json(b'1', strict=False)
   True

``int``
-------

Integers map to integers in JSON.

Support for large integers varies by encoding:

- ``JSON`` only supports encoding/decoding integers within
  ``[-2**63, 2**64 - 1]``, inclusive.

.. code-block:: python

    >>> StructAdapter(Any).struct_dump_json(123)
    b"123"

    >>> StructAdapter(int).struct_validate_json(b"123")
    123

If ``strict=False`` is specified, string values may also be coerced to
integers, following the same restrictions as above. Likewise floats that have
an exact integer representation (i.e. no decimal component) may also be coerced
as integers. See :ref:`strict-vs-lax` for more information.

.. code-block:: python

   >>> StructAdapter(int).struct_validate_json(b'"123"', strict=False)
   123

   >>> StructAdapter(int).struct_validate_json(b'123.0', strict=False)
   123


``float``
---------

Floats map to floats in JSON. Note that per RFC8259_, JSON
doesn't support nonfinite numbers (``nan``, ``infinity``, ``-infinity``);
JSON encoding handles this by encoding these values as ``null``.

For JSON, if a `float` type is specified and an `int` value is
provided, the `int` will be automatically converted.

.. code-block:: python

    >>> StructAdapter(Any).struct_dump_json(123.0)
    b"123.0"

    >>> # JSON doesn't support nonfinite values, these serialize as null
    ... StructAdapter(Any).struct_dump_json(float("nan"))
    b"null"

    >>> StructAdapter(float).struct_validate_json(b"123.0")
    123.0

    >>> # Ints are automatically converted to floats
    ... StructAdapter(float).struct_validate_json(b"123")
    123.0

If ``strict=False`` is specified, string values may also be coerced to floats.
Note that in this case the strings ``"nan"``, ``"inf"`` / ``"infinity"``,
``"-inf"`` / ``"-infinity"`` (case insensitive) will coerce to
``nan`` / ``inf`` / ``-inf``. See :ref:`strict-vs-lax` for more information.

.. code-block:: python

   >>> StructAdapter(float).struct_validate_json(b'"123.45"', strict=False)
   123.45

   >>> StructAdapter(float).struct_validate_json(b'"-inf"', strict=False)
   -inf

``str``
-------

Strings map to strings in JSON.

Note that for JSON, only the characters required by RFC8259_ are escaped to
ascii; unicode characters (e.g. ``"𝄞"``) are *not* escaped and are serialized
directly as UTF-8 bytes.

.. code-block:: python

    >>> StructAdapter(Any).struct_dump_json("Hello, world!")
    b'"Hello, world!"'

    >>> StructAdapter(Any).struct_dump_json("𝄞 is not escaped")
    b'"\xf0\x9d\x84\x9e is not escaped"'

    >>> StructAdapter(Any).struct_validate_json(b'"Hello, world!"')
    "Hello, world!"

``bytes`` / ``bytearray`` / ``memoryview``
------------------------------------------

Bytes-like objects map to base64-encoded strings in JSON.

.. code-block:: python

    >>> msg = StructAdapter(Any).struct_dump_json(b"\xf0\x9d\x84\x9e")

    >>> msg
    b'"85+Eng=="'

    >>> StructAdapter(bytes).struct_validate_json(msg)
    b'\xf0\x9d\x84\x9e'

    >>> StructAdapter(bytearray).struct_validate_json(msg)
    bytearray(b'\xf0\x9d\x84\x9e')


.. note::

    `memoryview` objects will be decoded as
    direct views into the larger buffer containing the input message being
    decoded. This may be useful for implementing efficient zero-copy handling
    of large binary messages, but is also a potential footgun. As long as a
    decoded ``memoryview`` remains in memory, the input message buffer will
    also be persisted, potentially resulting in unnecessarily large memory
    usage. The usage of ``memoryview`` types in this manner is considered an
    advanced topic, and should only be used when you know their usage will
    result in a performance benefit.

    `memoryview` objects are decoded as copies, and will likely be slightly
    slower than decoding into a `bytes` object.


``datetime``
------------

The encoding used for `datetime.datetime` objects depends on whether these objects are timezone-aware_ or timezone-naive:

- **JSON**: Timezone-aware datetimes are encoded as RFC3339_ compatible
  strings. Timezone-naive datetimes are encoded the same, but lack the timezone
  component (making them not strictly RFC3339_ compatible, but still ISO8601_
  compatible).



Note that you can require a `datetime.datetime` object to be timezone-aware or
timezone-naive by specifying a ``tz`` constraint (see
:ref:`datetime-constraints` for more information).

.. code-block:: python

    >>> import datetime

    >>> tz = datetime.timezone(datetime.timedelta(hours=6))

    >>> tz_aware = datetime.datetime(2021, 4, 2, 18, 18, 10, 123, tzinfo=tz)

    >>> msg = StructAdapter(Any).struct_dump_json(tz_aware)

    >>> msg
    b'"2021-04-02T18:18:10.000123+06:00"'

    >>> StructAdapter(datetime.datetime).struct_validate_json(msg)
    datetime.datetime(2021, 4, 2, 18, 18, 10, 123, tzinfo=datetime.timezone(datetime.timedelta(seconds=21600)))

    >>> tz_naive = datetime.datetime(2021, 4, 2, 18, 18, 10, 123)

    >>> msg = StructAdapter(Any).struct_dump_json(tz_naive)

    >>> msg
    b'"2021-04-02T18:18:10.000123"'

    >>> StructAdapter(datetime.datetime).struct_validate_json(msg)
    datetime.datetime(2021, 4, 2, 18, 18, 10, 123)

    >>> StructAdapter(datetime.datetime).struct_validate_json(b'"oops"')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Invalid RFC3339 encoded datetime


Additionally, if ``strict=False`` is specified, JSON will decode ints,
floats, or strings containing ints/floats as timezone-aware datetimes,
interpreting the value as seconds since the epoch in UTC (a `Unix Timestamp
<https://en.wikipedia.org/wiki/Unix_time>`__). See :ref:`strict-vs-lax` for
more information.

.. code-block:: python

    >>> StructAdapter(datetime.datetime).struct_validate_json(b"1617405490.000123")
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `datetime`, got `float`

    >>> StructAdapter(datetime.datetime).struct_validate_json(b"1617405490.000123", strict=False)
    datetime.datetime(2021, 4, 2, 18, 18, 10, 123, tzinfo=datetime.timezone.utc)

``date``
--------

`datetime.date` values map to:

- **JSON**: RFC3339_ encoded strings

.. code-block:: python

    >>> import datetime

    >>> date = datetime.date(2021, 4, 2)

    >>> msg = StructAdapter(Any).struct_dump_json(date)

    >>> msg
    b'"2021-04-02"'

    >>> StructAdapter(datetime.date).struct_validate_json(msg)
    datetime.date(2021, 4, 2)

    >>> StructAdapter(datetime.date).struct_validate_json(b'"oops"')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Invalid RFC3339 encoded date

``time``
--------

The encoding used for `datetime.time` objects is dependent on whether these
objects are timezone-aware_ or timezone-naive:

- **JSON**: Timezone-aware times are encoded as RFC3339_ compatible strings.
  Timezone-naive times are encoded the same, but lack the timezone component
  (making them not strictly RFC3339_ compatible, but still ISO8601_ compatible).

Note that you can require a `datetime.time` object to be timezone-aware or
timezone-naive by specifying a ``tz`` constraint (see
:ref:`datetime-constraints` for more information).

.. code-block:: python

    >>> import datetime

    >>> tz = datetime.timezone(datetime.timedelta(hours=6))

    >>> tz_aware = datetime.time(18, 18, 10, 123, tzinfo=tz)

    >>> msg = StructAdapter(Any).struct_dump_json(tz_aware)

    >>> msg
    b'"18:18:10.000123+06:00"'

    >>> StructAdapter(datetime.time).struct_validate_json(msg)
    datetime.time(18, 18, 10, 123, tzinfo=datetime.timezone(datetime.timedelta(seconds=21600)))

    >>> tz_naive = datetime.time(18, 18, 10, 123)

    >>> msg = StructAdapter(Any).struct_dump_json(tz_naive)

    >>> msg
    b'"18:18:10.000123"'

    >>> StructAdapter(datetime.time).struct_validate_json(msg)
    datetime.time(18, 18, 10, 123)

    >>> StructAdapter(datetime.time).struct_validate_json(b'"oops"')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Invalid RFC3339 encoded time

``timedelta``
-------------

`datetime.timedelta` values map to extended `ISO 8601 duration strings`_ in JSON.

The format as described in the ISO specification is fairly lax and a bit
underspecified, leading most real-world implementations to implement a stricter
subset.

The duration format used here is as follows:

.. code-block:: text

   [+/-]P[#D][T[#H][#M][#S]]

- The format starts with an optional sign (``-`` or ``+``). If negative, the
  whole duration is negated.

- The letter ``P`` follows (case insensitive)

- There are then four segments, each consisting of a number and unit. The units
  are ``D``, ``H``, ``M``, ``S`` (case insensitive) for days, hours, minutes,
  and seconds respectively. These segments must occur in this order.

  - If a segment would have a 0 value it may be omitted, with the caveat that at
    least one segment must be present.

  - If a time (hour, minute, or second) segment is present then the letter ``T``
    (case insensitive) must precede the first time segment. Likewise if a ``T``
    is present, there must be at least 1 segment after the ``T``.

  - Each segment is composed of 1 or more digits, followed by the unit. Leading
    0s are accepted. The *final* segment may include a decimal component if
    needed.

A few examples:

.. code-block:: python

   "P0D"                # 0 days
   "P1D"                # 1 Day
   "PT1H30S"            # 1 Hour and 30 minutes
   "PT1.5H"             # 1 Hour and 30 minutes
   "-PT1M30S"           # -90 seconds
   "PT1H30M25.5S"       # 1 Hour, 30 minutes, and 25.5 seconds

While structtype will decode duration strings making use of the ``H`` (hour) or
``M`` (minute) units, durations encoded by structtype will only consist of ``D``
(day) and ``S`` (second) segments.

The implementation in ``structtype`` is compatible with the ones in:

- Java's ``time.Duration.parse`` (`docs <https://docs.oracle.com/en/java/javase/17/docs/api/java.base/java/time/Duration.html#parse(java.lang.CharSequence)>`__)
- Javascript's proposed ``Temporal.Duration`` standard API (`docs <https://tc39.es/proposal-temporal/docs/duration.html>`__)
- Python libraries like pendulum_ or pydantic_.

Duration strings produced by structtype should be interchangeable with these
libraries, as well as similar ones in other language ecosystems.

.. code-block:: python

    >>> from datetime import timedelta

    >>> StructAdapter(Any).struct_dump_json(timedelta(seconds=123))
    b'"PT123S"'

    >>> StructAdapter(Any).struct_dump_json(timedelta(days=1, seconds=30, microseconds=123))
    b'"P1DT30.000123S"'

    >>> StructAdapter(timedelta).struct_validate_json(b'"PT123S"')
    datetime.timedelta(seconds=123)

    >>> StructAdapter(timedelta).struct_validate_json(b'"PT1.5M"')
    datetime.timedelta(seconds=90)

    >>> StructAdapter(datetime.timedelta).struct_validate_json(b'"oops"')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Invalid ISO8601 duration

Additionally, if ``strict=False`` is specified, JSON will decode ints,
floats, or strings containing ints/floats as timedeltas, interpreting the value
as total seconds. See :ref:`strict-vs-lax` for more information.

.. code-block:: python

    >>> StructAdapter(datetime.timedelta).struct_validate_json(b"123.4")
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `duration`, got `float`

    >>> StructAdapter(datetime.timedelta).struct_validate_json(b"123.4", strict=False)
    datetime.timedelta(seconds=123, microseconds=400000)

``uuid``
--------

`uuid.UUID` values are serialized as RFC4122_ encoded canonical strings in JSON by default. Subclasses of `uuid.UUID` are also supported for encoding
only.

.. code-block:: python

    >>> from structtype import StructAdapter
    >>> from typing import Any
    >>> import uuid

    >>> u = uuid.UUID("c4524ac0-e81e-4aa8-a595-0aec605a659a")

    >>> StructAdapter(Any).struct_dump_json(u)
    b'"c4524ac0-e81e-4aa8-a595-0aec605a659a"'

    >>> StructAdapter(uuid.UUID).struct_validate_json(b'"c4524ac0-e81e-4aa8-a595-0aec605a659a"')
    UUID('c4524ac0-e81e-4aa8-a595-0aec605a659a')

    >>> StructAdapter(uuid.UUID).struct_validate_json(b'"oops"')
    Traceback (most recent call last):
        File "<stdin>", line 1, in <module>
    structtype.ValidationError: Invalid UUID

Alternative formats are supported by the JSON encoder. The format may be
selected by passing ``uuid_format`` to ``struct_dump_json`` (on a ``Struct`` or
``StructAdapter``). The following options are supported:

- ``canonical``: UUIDs are encoded as RFC4122_ canonical strings (same as
  ``str(uuid)``). This is the default.
- ``hex``: UUIDs are encoded as RFC4122_ hex strings (same as ``uuid.hex``).

When decoding, any of the above formats are accepted.

.. code-block:: python

    >>> from structtype import StructAdapter

    >>> uuid_hex = StructAdapter(uuid.UUID).struct_dump_json(u, uuid_format="hex")

    >>> uuid_hex
    b'"c4524ac0e81e4aa8a5950aec605a659a"'

    >>> StructAdapter(uuid.UUID).struct_validate_json(uuid_hex)
    UUID('c4524ac0-e81e-4aa8-a595-0aec605a659a')

    >>> StructAdapter(uuid.UUID).struct_dump_json(u, uuid_format="hex")
    b'"c4524ac0e81e4aa8a5950aec605a659a"'


``decimal``
-----------

`decimal.Decimal` values are encoded as their string representation in
JSON by default. This ensures no precision loss during serialization, as
would happen with a float representation.

.. code-block:: python

    >>> from structtype import StructAdapter
    >>> from typing import Any
    >>> import decimal

    >>> x = decimal.Decimal("1.2345")

    >>> msg = StructAdapter(Any).struct_dump_json(x)

    >>> msg
    b'"1.2345"'

    >>> StructAdapter(decimal.Decimal).struct_validate_json(msg)
    Decimal('1.2345')

    >>> StructAdapter(decimal.Decimal).struct_validate_json(b'"oops"')
    Traceback (most recent call last):
        File "<stdin>", line 1, in <module>
    structtype.ValidationError: Invalid decimal string

For JSON you may instead encode decimal values the same as numbers by passing
``decimal_format="number"`` to ``struct_dump_json``:

.. code-block:: python

    >>> StructAdapter(decimal.Decimal).struct_dump_json(x, decimal_format="number")
    b'1.2345'

For JSON you may also pass a callable to ``decimal_format`` to
customize how decimals are encoded. The callable will be called with the
`decimal.Decimal` instance as the only argument, and must return a JSON-serializable
value (but **not** another `decimal.Decimal` or a nested structure containing
`decimal.Decimal`). This is useful for custom rounding, formatting, or transforming
decimal values before encoding.

.. code-block:: python

    >>> import decimal
    >>> from structtype import Struct

    >>> class Price(Struct):
    ...     amount: decimal.Decimal
    ...     currency: str

    >>> Price(amount=decimal.Decimal("10.123456"), currency="USD").struct_dump_json(
    ...     decimal_format=lambda d: str(d.quantize(decimal.Decimal("0.01")))
    ... )
    b'{"amount":"10.12","currency":"USD"}'

.. warning::

    The callable passed to ``decimal_format`` must **not** return a
    `decimal.Decimal` instance or any nested structure (list, dict, tuple, set)
    containing `decimal.Decimal`, as this would cause infinite recursion. The
    callable should return a `str`, `int`, `float`, or other JSON-serializable
    type. If the callable returns a value containing a `decimal.Decimal`, a
    `TypeError` will be raised.

JSON will also decode `decimal.Decimal` values from ``int`` or
``float`` inputs. For JSON the value is parsed directly from the serialized
bytes, avoiding any precision loss:

.. code-block:: python

   >>> StructAdapter(decimal.Decimal).struct_validate_json(b"1.3")
   Decimal('1.3')

   >>> StructAdapter(decimal.Decimal).struct_validate_json(b"1.300")
   Decimal('1.300')

   >>> StructAdapter(decimal.Decimal).struct_validate_json(b"0.1234567891234567811")
   Decimal('0.1234567891234567811')


``list`` / ``tuple`` / ``set`` / ``frozenset``
----------------------------------------------

`list`, `tuple`, `set`, and `frozenset` objects map to arrays in JSON.
An error is raised if the elements don't match the specified element type (if
provided).

Subclasses of these types are also supported for encoding only. To decode into
a ``list`` subclass you'll need to implement a ``dec_hook`` (see
:doc:`extending`).

.. code-block:: python

    >>> StructAdapter(Any).struct_dump_json([1, 2, 3])
    b'[1,2,3]'

    >>> StructAdapter(Any).struct_dump_json({1, 2, 3})
    b'[1,2,3]'

    >>> StructAdapter(set).struct_validate_json(b'[1,2,3]')
    {1, 2, 3}

    >>> # Decode as a set of ints
    ... StructAdapter(set[int]).struct_validate_json(b'[1, 2, 3]')
    {1, 2, 3}

    >>> # Oops, all elements should be ints
    ... StructAdapter(set[int]).struct_validate_json(b'[1, 2, "oops"]')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int`, got `str` - at `$[2]`

``NamedTuple``
--------------

`typing.NamedTuple` types map to arrays in JSON.  An error is raised
during decoding if the type doesn't match or if any required fields are
missing.

Note that ``structtype`` supports both `typing.NamedTuple` and
`collections.namedtuple`, although the latter lacks a way to specify field
types.

When possible we recommend using `structtype.Struct` (possibly with
``array_like=True`` and ``frozen=True``) instead of ``NamedTuple`` for
specifying schemas - :doc:`Structs <usage>` are faster, more ergonomic, and support
additional features.  Still, you may want to use a ``NamedTuple`` if you're
already using them elsewhere, or if you have downstream code that requires a
``tuple`` instead of an object.

.. code-block:: python

    >>> from typing import NamedTuple

    >>> class Person(NamedTuple):
    ...     name: str
    ...     age: int

    >>> ben = Person("ben", 25)

    >>> msg = StructAdapter(Any).struct_dump_json(ben)

    >>> StructAdapter(Person).struct_validate_json(msg)
    Person(name='ben', age=25)

    >>> wrong_type = b'["chad", "twenty"]'

    >>> StructAdapter(Person).struct_validate_json(wrong_type)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int`, got `str` - at `$[1]`

Other types that duck-type as ``NamedTuple`` are also supported, such as
`os.stat_result`.

.. code-block:: python

    >>> import os

    >>> import sys

    >>> result = os.stat(sys.executable)

    >>> result
    os.stat_result(st_mode=33261, st_ino=5396073, st_dev=105, st_nlink=1, st_uid=0, st_gid=0, st_size=18440, st_atime=1760547094, st_mtime=1760547094, st_ctime=1760907672)

    >>> StructAdapter(Any).struct_dump_json(result)
    b'[33261,5396073,105,1,0,0,18440,1760547094,1760547094,1760907672]'

``dict``
--------

Dicts encode/decode as objects in JSON.

Dict subclasses (`collections.OrderedDict`, for example) are also supported for
encoding only. To decode into a ``dict`` subclass you'll need to implement a
``dec_hook`` (see :doc:`extending`).

JSON only supports key types that encode as strings or numbers (for
example `str`, `int`, `float`, `enum.Enum`, `datetime.datetime`, `uuid.UUID`,
...).

An error is raised during decoding if the keys or values don't match their
respective types (if specified).

.. code-block:: python

    >>> StructAdapter(Any).struct_dump_json({"x": 1, "y": 2})
    b'{"x":1,"y":2}'

    >>> # Decode as a Dict of str -> int
    ... StructAdapter(dict[str, int]).struct_validate_json(b'{"x":1,"y":2}')
    {"x": 1, "y": 2}

    >>> # Oops, there's a mistyped value
    ... StructAdapter(dict[str, int]).struct_validate_json(b'{"x":1,"y":"oops"}')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int`, got `str` - at `$[...]`

``TypedDict``
-------------

`typing.TypedDict` provides a way to specify different types for different
values in a ``dict``, rather than a single value type (the ``int`` in
``dict[str, int]``, for example).  At runtime these are just standard
``dict`` types, the ``TypedDict`` type is only there to provide the schema
information during decoding. Note that ``structtype`` supports both
`typing.TypedDict` and ``typing_extensions.TypedDict`` (a backport).

`typing.TypedDict` types map to objects in JSON. During decoding,
any extra fields are ignored. An error is raised during decoding if the type
doesn't match or if any required fields are missing.

When possible we recommend using `structtype.Struct` instead of ``TypedDict`` for
specifying schemas - :doc:`Structs <usage>` are faster, more ergonomic, and support
additional features. Still, you may want to use a ``TypedDict`` if you're
already using them elsewhere, or if you have downstream code that requires a
``dict`` instead of an object.

.. code-block:: python

    >>> from typing import TypedDict

    >>> class Person(TypedDict):
    ...     name: str
    ...     age: int

    >>> ben = {"name": "ben", "age": 25}

    >>> msg = StructAdapter(Any).struct_dump_json(ben)

    >>> StructAdapter(Person).struct_validate_json(msg)
    {'name': 'ben', 'age': 25}

    >>> wrong_type = b'{"name": "chad", "age": "twenty"}'

    >>> StructAdapter(Person).struct_validate_json(wrong_type)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int`, got `str` - at `$.age`

``dataclasses``
---------------

`dataclasses` map to objects in JSON.

During decoding, any extra fields are ignored. An error is raised if a field's
type doesn't match or if any required fields are missing.

If a ``__post_init__`` method is defined on the dataclass, it is called after
the object is decoded. Note that `"Init-only parameters"
<https://docs.python.org/3/library/dataclasses.html#init-only-variables>`__
(i.e. ``InitVar`` fields) are _not_ supported.

When possible we recommend using `structtype.Struct` instead of dataclasses for
specifying schemas - :doc:`Structs <usage>` are faster, more ergonomic, and support
additional features.

.. code-block:: python

    >>> from dataclasses import dataclass

    >>> @dataclass
    ... class Person:
    ...     name: str
    ...     age: int

    >>> carol = Person(name="carol", age=32)

    >>> msg = StructAdapter(Any).struct_dump_json(carol)

    >>> StructAdapter(Person).struct_validate_json(msg)
    Person(name='carol', age=32)

    >>> wrong_type = b'{"name": "doug", "age": "thirty"}'

    >>> StructAdapter(Person).struct_validate_json(wrong_type)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int`, got `str` - at `$.age`

Other types that duck-type as ``dataclasses`` are also supported, such as
`pydantic dataclasses <https://pydantic.dev/docs/validation/latest/concepts/dataclasses/>`__.

.. code-block:: python

    >>> from datetime import datetime

    >>> from pydantic.dataclasses import dataclass

    >>> @dataclass
    ... class User:
    ...     id: int
    ...     name: str = 'John Doe'
    ...     signup_ts: datetime | None = None

    >>> user = User(id='42', signup_ts='2032-06-21T12:00')

    >>> user
    User(id=42, name='John Doe', signup_ts=datetime.datetime(2032, 6, 21, 12, 0))

    >>> StructAdapter(Any).struct_dump_json(user)
    b'{"id":42,"name":"John Doe","signup_ts":"2032-06-21T12:00:00"}'

``attrs``
---------

attrs_ types map to objects in JSON.

During encoding, all attributes without a leading underscore (``"_"``) are
encoded.

During decoding, any extra fields are ignored. An error is raised if a field's
type doesn't match or if any required fields are missing.

If the ``__attrs_pre_init__`` or ``__attrs_post_init__`` methods are defined on
the class, they are called as part of the decoding process. Likewise, if a
class makes use of attrs' `validators
<https://www.attrs.org/en/stable/examples.html#validators>`__, the validators
will be called, and a `structtype.ValidationError` raised on error. Note that
attrs' `converters
<https://www.attrs.org/en/stable/examples.html#conversion>`__ are not currently
supported.

When possible we recommend using `structtype.Struct` instead of attrs_ types for
specifying schemas - :doc:`Structs <usage>` are faster, more ergonomic, and support
additional features.

.. code-block:: python

    >>> from attrs import define

    >>> @define
    ... class Person:
    ...     name: str
    ...     age: int

    >>> carol = Person(name="carol", age=32)

    >>> msg = StructAdapter(Any).struct_dump_json(carol)

    >>> StructAdapter(Person).struct_validate_json(msg)
    Person(name='carol', age=32)

    >>> wrong_type = b'{"name": "doug", "age": "thirty"}'

    >>> StructAdapter(Person).struct_validate_json(wrong_type)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int`, got `str` - at `$.age`

``Struct``
----------

Structs are the preferred way of defining structured data types in ``structtype``.
You can think of them as similar to dataclasses_/attrs_/pydantic_, but much
faster to create/compare/encode/decode. For more information, see the
:doc:`Structs <usage>` page.

By default `structtype.Struct` types map to objects in JSON. During
decoding, any unknown fields are ignored (this can be disabled, see
:ref:`forbid-unknown-fields`), and any missing optional fields have their
default values applied. An error is raised during decoding if the type doesn't
match or if any required fields are missing.

.. code-block:: python

    >>> from typing import Set, Optional
    >>> from structtype import Struct

    >>> class User(Struct):
    ...     name: str
    ...     groups: set[str] = set()
    ...     email: str | None = None

    >>> alice = User("alice", groups={"admin", "engineering"})

    >>> StructAdapter(Any).struct_dump_json(alice)
    b'{"name":"alice","groups":["admin","engineering"],"email":null}'

    >>> msg = b"""
    ... {
    ...     "name": "bob",
    ...     "email": "bob@company.com",
    ...     "unknown_field": [1, 2, 3]
    ... }
    ... """

    >>> StructAdapter(User).struct_validate_json(msg)
    User(name='bob', groups=[], email="bob@company.com")

    >>> wrong_type = b"""
    ... {
    ...     "name": "bob",
    ...     "groups": ["engineering", 123]
    ... }
    ... """

    >>> StructAdapter(User).struct_validate_json(wrong_type)
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `str`, got `int` - at `$.groups[1]`

If you pass ``array_like=True`` when defining the struct type, they're instead
treated as array types during encoding/decoding. In this case fields are
serialized in their :ref:`field order <struct-field-ordering>`. This can
further improve performance at the cost of less human readable messaging. Like
``array_like=False`` (the default) structs, extra (trailing) fields are ignored
during decoding, and any missing optional fields have their defaults applied.
Type checking also still applies.

.. code-block:: python

    >>> from structtype import Struct
    >>> class User(Struct, array_like=True):
    ...     name: str
    ...     groups: set[str] = set()
    ...     email: str | None = None

    >>> alice = User("alice", groups={"admin", "engineering"})

    >>> StructAdapter(Any).struct_dump_json(alice)
    b'["alice",["admin","engineering"],null]'

    >>> StructAdapter(User).struct_validate_json(b'["bob"]')
    User(name="bob", groups=[], email=None)

    >>> StructAdapter(User).struct_validate_json(b'["carol", ["admin"], null, ["extra", "field"]]')
    User(name="carol", groups=["admin"], email=None)

    >>> StructAdapter(Any).struct_validate_json(b'["david", ["finance", 123]]')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `str`, got `int` - at `$[1][1]`

.. _unset-type:

``UNSET``
---------

`structtype.UNSET` is a singleton object used to indicate that a field has no set
value. This is useful for cases where you need to differentiate between a
message where a field is missing and a message where the field is explicitly
``None``.

.. code-block:: python

    >>> from structtype import Struct, UnsetType, UNSET

    >>> class Example(Struct):
    ...     x: int
    ...     y: int | None | UnsetType = UNSET  # a field, defaulting to UNSET

During encoding, any field containing ``UNSET`` is omitted from the message.

.. code-block:: python

    >>> Example(1).struct_dump_json()  # y is UNSET
    b'{"x":1}'

    >>> Example(1, None).struct_dump_json()  # y is None
    b'{"x":1,"y":null}'

    >>> Example(1, 2).struct_dump_json()  # y is 2
    b'{"x":1,"y":2}'

During decoding, if a field isn't explicitly set in the message, the default
value of ``UNSET`` will be set instead. This lets downstream consumers
determine whether a field was left unset, or explicitly set to ``None``:

.. code-block:: python

    >>> Example.struct_validate_json(b'{"x": 1}')  # y defaults to UNSET
    Example(x=1, y=UNSET)

    >>> Example.struct_validate_json(b'{"x": 1, "y": null}')  # y is None
    Example(x=1, y=None)

    >>> Example.struct_validate_json(b'{"x": 1, "y": 2}')  # y is 2
    Example(x=1, y=2)

``UNSET`` fields are supported for `structtype.Struct`, `dataclasses`, and attrs_
types. It is an error to use `structtype.UNSET` or `structtype.UnsetType` anywhere
other than a field for one of these types.

Omission of ``UNSET`` fields applies to `struct_dump_json` and
`struct_dump`. `dict()` always include every field, so ``UNSET`` values appear
in their output unchanged.

``Enum`` / ``IntEnum`` / ``StrEnum``
------------------------------------

Enum types (`enum.Enum`, `enum.IntEnum`, `enum.StrEnum`, ...) encode as their
member *values* in JSON.

Any enum whose *value* is a supported type may be encoded, but only enums
composed of all string or all integer values may be decoded.

An error is raised during decoding if the value isn't the proper type, or
doesn't match any valid member.

.. code-block:: python

    >>> import enum

    >>> class Fruit(enum.Enum):
    ...     APPLE = "apple"
    ...     BANANA = "banana"

    >>> StructAdapter(Any).struct_dump_json(Fruit.APPLE)
    b'"apple"'

    >>> StructAdapter(Fruit).struct_validate_json(b'"apple"')
    <Fruit.APPLE: 'apple'>

    >>> StructAdapter(Fruit).struct_validate_json(b'"grape"')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Invalid enum value 'grape'

    >>> class JobState(enum.IntEnum):
    ...     CREATED = 0
    ...     RUNNING = 1
    ...     SUCCEEDED = 2
    ...     FAILED = 3

    >>> StructAdapter(Any).struct_dump_json(JobState.RUNNING)
    b'1'

    >>> StructAdapter(JobState).struct_validate_json(b'2')
    <JobState.SUCCEEDED: 2>

    >>> StructAdapter(JobState).struct_validate_json(b'4')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Invalid enum value 4

If the enum type includes a ``_missing_`` method (`docs
<https://docs.python.org/3/library/enum.html#enum.Enum._missing_>`__), this
method will be called to handle any missing values. It should return a valid
enum member, or ``None`` if the value is invalid. One potential use case of
this is supporting case-insensitive enums:

.. code-block:: python

    >>> import enum

    >>> class Fruit(enum.Enum):
    ...     APPLE = "apple"
    ...     BANANA = "banana"
    ...
    ...     @classmethod
    ...     def _missing_(cls, name):
    ...         """Called to handle missing enum values"""
    ...         # Normalize value to lowercase
    ...         value = name.lower()
    ...         # Return valid enum value, or None if invalid
    ...         return cls._value2member_map_.get(value)

    >>> StructAdapter(Fruit).struct_validate_json(b'"apple"')
    <Fruit.APPLE: "apple">

    >>> StructAdapter(Fruit).struct_validate_json(b'"ApPlE"')
    <Fruit.APPLE: "apple">

    >>> StructAdapter(Fruit).struct_validate_json(b'"grape"')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Invalid enum value 'grape'

``Literal``
-----------

`typing.Literal` types can be used to ensure that a decoded object is within a
set of valid values. An `enum.Enum` or `enum.IntEnum` can be used for the same
purpose, but with a `typing.Literal` the decoded values are literal `int` or
`str` instances rather than `enum` objects.

A literal can be composed of any of the following objects:

- `None`
- `bool` values (`True` and `False`)
- `int` values
- `str` values
- Nested `typing.Literal` types

An error is raised during decoding if the value isn't in the set of valid
values, or doesn't match any of their component types.

.. code-block:: python

    >>> from typing import Literal

    >>> StructAdapter(Literal[1, 2, 3]).struct_validate_json(b'1')
    1

    >>> StructAdapter(Literal["one", "two", "three"]).struct_validate_json(b'"one"')
    'one'

    >>> StructAdapter(Literal[1, 2, 3]).struct_validate_json(b'4')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Invalid enum value 4

    >>> StructAdapter(Literal[1, 2, 3]).struct_validate_json(b'"bad"')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int`, got `str`

    >>> StructAdapter(Literal[True]).struct_validate_json(b'true')
    True

    >>> StructAdapter(Literal[True, False]).struct_validate_json(b'false')
    False

``NewType``
-----------

`typing.NewType` types are treated identically to their base type. Their
support here is purely to aid static analysis tools like mypy_ or pyright_.

.. code-block:: python

    >>> from typing import NewType

    >>> UserId = NewType("UserId", int)

    >>> StructAdapter(Any).struct_dump_json(UserId(1234))
    b'1234'

    >>> StructAdapter(UserId).struct_validate_json(b'1234')
    1234

    >>> StructAdapter(UserId).struct_validate_json(b'"oops"')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int`, got `str`

Type Aliases
------------

For complex types, sometimes it can be nice to write the type once so you can
reuse it later.

.. code-block:: python

    Point = tuple[float, float]

Here ``Point`` is a "type alias" for ``tuple[float, float]`` - ``structtype``
will substitute in ``tuple[float, float]`` whenever the ``Point`` type
is used in an annotation.

``structtype`` supports the following equivalent forms:

.. code-block:: python

    # Using variable assignment
    Point = tuple[float, float]

    # Using variable assignment, annotated as a `TypeAlias`
    Point: TypeAlias = tuple[float, float]

    # Using Python 3.12's new `type` statement. This only works on Python 3.12+
    type Point = tuple[float, float]

To learn more about Type Aliases, see Python's `Type Alias docs here
<https://docs.python.org/3/library/typing.html#type-aliases>`__.

Generic Types
-------------

``structtype`` supports generic types, including `user-defined generic types`_
based on any of the following types:

- `structtype.Struct`
- `dataclasses`
- `attrs`
- `typing.TypedDict`
- `typing.NamedTuple`

Generic types may be useful for reusing common message structures.

To define a generic type:

- Define one or more type variables (`typing.TypeVar`) to parametrize your type with.
- Add `typing.Generic` as a base class when defining your type, parametrizing
  it by the relevant type variables.
- When annotating the field types, use the relevant type variables instead of
  "concrete" types anywhere you want to be generic.

For example, here we define a generic ``Paginated`` struct type for storing
extra pagination information in an API response.

.. code-block:: python

    from structtype import Struct
    from typing import Generic, TypeVar

    # A type variable for the item type
    T = TypeVar("T")

    class Paginated(Struct, Generic[T]):
        """A generic paginated API wrapper, parametrized by the item type."""
        page: int        # The current page number
        per_page: int    # Number of items per page
        total: int       # The total number of items found
        items: list[T]   # Items returned, up to `per_page` in length

This type is generic over the type of item contained in ``Paginated.items``.
This ``Paginated`` wrapper may then be used to decode a message containing a
specific item type by parametrizing it with that type. When processing a
generic type, the parametrized types are substituted for the type variables.

Here we define a ``User`` type, then use it to decode a paginated API response
containing a list of users:

.. code-block:: python

    class User(Struct):
        """A user model"""
        name: str
        groups: list[str] = []

    json_str = """
    {
        "page": 1,
        "per_page": 5,
        "total": 252,
        "items": [
            {"name": "alice", "groups": ["admin"]},
            {"name": "ben"},
            {"name": "carol", "groups": ["engineering"]},
            {"name": "dan", "groups": ["hr"]},
            {"name": "ellen", "groups": ["engineering"]}
        ]
    }
    """

    # Decode a paginated response containing a list of users
    msg = StructAdapter(Paginated[User]).struct_validate_json(json_str)
    print(msg)
    #> Paginated(
    #>     page=1, per_page=5, total=252,
    #>     items=[
    #>         User(name='alice', groups=['admin']),
    #>         User(name='ben', groups=[]),
    #>         User(name='carol', groups=['engineering']),
    #>         User(name='dan', groups=['hr']),
    #>         User(name='ellen', groups=['engineering'])
    #>     ]
    #> )

If instead we wanted to decode a paginated response of another type (say
``Team``), we could do this by parametrizing ``Paginated`` with a different
type.

.. code-block:: python

    # Decode a paginated response containing a list of teams
    StructAdapter(Paginated[Team]).struct_validate_json(some_other_message)

Any unparametrized type variables will be treated as `typing.Any` when decoding.

.. code-block:: python

    # These are equivalent.
    # The unparametrized version substitutes in `Any` for `T`
    StructAdapter(Paginated).struct_validate_json(some_other_message)
    StructAdapter(Paginated[Any]).struct_validate_json(some_other_message)

However, if an unparametrized type variable has a ``bound`` (`docs
<https://peps.python.org/pep-0484/#type-variables-with-an-upper-bound>`__),
then the bound type will be used instead.

.. code-block:: python

    from collections.abc import Sequence
    S = TypeVar("S", bound=Sequence)  # Can be any sequence type

    class Example(Struct, Generic[S]):
        value: S

    msg = b'{"value": [1, 2, 3]}'

    # These are equivalent.
    # The unparametrized version substitutes in `Sequence` for `S`
    StructAdapter(Example).struct_validate_json(some_other_message)
    StructAdapter(Example[Sequence]).struct_validate_json(some_other_message)

See the official Python docs on `generic types`_ and the `corresponding PEP
<https://peps.python.org/pep-0484/#generics>`__ for more information.

Abstract Types
--------------

``structtype`` supports several "abstract" types, decoding them as
instances of their most common concrete type.

**Decoded as lists**

- `collections.abc.Collection` / `typing.Collection`
- `collections.abc.Sequence` / `typing.Sequence`
- `collections.abc.MutableSequence` / `typing.MutableSequence`

**Decoded as sets**

- `collections.abc.Set` / `typing.AbstractSet`
- `collections.abc.MutableSet` / `typing.MutableSet`

**Decoded as dicts**

- `collections.abc.Mapping` / `typing.Mapping`
- `collections.abc.MutableMapping` / `typing.MutableMapping`

.. code-block:: python

    >>> from typing import MutableMapping

    >>> StructAdapter(MutableMapping[str, int]).struct_validate_json(b'{"x": 1}')
    {"x": 1}

    >>> StructAdapter(MutableMapping[str, int]).struct_validate_json(b'{"x": "oops"}')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int`, got `str` - at `$[...]`

``Union`` /  ``Optional``
-------------------------

Type unions are supported, with a few restrictions. These restrictions are in
place to remove any ambiguity during decoding - given an encoded value there
must always be a single type in a given `typing.Union` that can decode that
value.

Union restrictions are as follows:

- Unions may contain at most one type that encodes to an integer (`int`,
  `enum.IntEnum`)

- Unions may contain at most one type that encodes to a string (`str`,
  `enum.Enum`, `bytes`, `bytearray`, `datetime.datetime`, `datetime.date`,
  `datetime.time`, `uuid.UUID`, `decimal.Decimal`). Note that this restriction
  is fixable with some work, if this is a feature you need please `open an issue`_.

- Unions may contain at most one type that encodes to an object (`dict`,
  `typing.TypedDict`, dataclasses_, attrs_, `Struct` with ``array_like=False``)

- Unions may contain at most one type that encodes to an array (`list`,
  `tuple`, `set`, `frozenset`, `typing.NamedTuple`, `Struct` with
  ``array_like=True``).

- Unions may contain at most one *untagged* `Struct` type. Unions containing
  multiple struct types are only supported through :ref:`struct-tagged-unions`.

- Unions with custom types are unsupported beyond optionality (i.e.
  ``CustomType | None``)

.. code-block:: python

    >>> from structtype import StructAdapter

    >>> # A decoder expecting either an int, a str, or a list of strings
    ... adapter = StructAdapter(int | str | list[str])

    >>> adapter.struct_validate_json(b'1')
    1

    >>> adapter.struct_validate_json(b'"two"')
    "two"

    >>> adapter.struct_validate_json(b'["three", "four"]')
    ["three", "four"]

    >>> adapter.struct_validate_json(b'false')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `int | str | array`, got `bool`

``Raw``
-------

`structtype.Raw` is a buffer-like type containing an already-encoded message.
They have two common uses:

**1. Avoiding unnecessary encoding cost**

Wrapping an already-encoded buffer in `structtype.Raw` lets the encoder avoid
re-encoding the message, instead it will simply be copied to the output buffer.
This can be useful when part of a message already exists in an encoded format
(e.g. reading JSON bytes from a database and returning them as part of a larger
message).

.. code-block:: python

    >>> from structtype import Raw, Struct

    >>> # Create a new `Raw` object wrapping a pre-encoded message
    ... fragment = Raw(b'{"x": 1, "y": 2}')

    >>> # Compose a larger message containing the pre-encoded fragment
    ... msg = {"a": 1, "b": fragment}

    >>> # During encoding, the raw message is efficiently copied into
    ... # the output buffer, avoiding any extra encoding cost
    ... StructAdapter(Any).struct_dump_json(msg)
    b'{"a":1,"b":{"x": 1, "y": 2}}'


**2. Delaying decoding of part of a message**

Sometimes the type of a serialized value depends on the value of other fields
in a message. ``structtype`` provides an optimized version of one common pattern
(:ref:`struct-tagged-unions`), but if you need to do something more complicated
you may find using `structtype.Raw` useful here.

For example, here we demonstrate how to decode a message where the type of one
field (``point``) depends on the value of another (``dimensions``).

.. code-block:: python

    >>> from structtype import Raw, Struct

    >>> from typing import Union

    >>> class Point1D(Struct):
    ...     x: int

    >>> class Point2D(Struct):
    ...     x: int
    ...     y: int

    >>> class Point3D(Struct):
    ...     x: int
    ...     y: int
    ...     z: int

    >>> class Model(Struct):
    ...     dimensions: int
    ...     point: Raw  # use structtype.Raw to delay decoding the point field

    >>> def decode_point(msg: bytes) -> Point1D | Point2D | Point3D:
    ...     """A function for efficiently decoding the `point` field"""
    ...     # First decode the outer `Model` struct. Decoding of the `point`
    ...     # field is delayed, with the composite bytes stored as a `Raw` object
    ...     # on `point`.
    ...     model = StructAdapter(Model).struct_validate_json(msg)
    ...
    ...     # Based on the value of `dimensions`, determine which type to use
    ...     # when decoding the `point` field
    ...     if model.dimensions == 1:
    ...         point_type = Point1D
    ...     elif model.dimensions == 2:
    ...         point_type = Point2D
    ...     elif model.dimensions == 3:
    ...         point_type = Point3D
    ...     else:
    ...         raise ValueError("Too many dimensions!")
    ...
    ...     # Now that we know the type of `point`, we can finish decoding it.
    ...     # Note that `Raw` objects are buffer-like, and can be passed
    ...     # directly to ``struct_validate_json``.
    ...     return StructAdapter(point_type).struct_validate_json(model.point)

    >>> decode_point(b'{"dimensions": 2, "point": {"x": 1, "y": 2}}')
    Point2D(x=1, y=2)

    >>> decode_point(b'{"dimensions": 3, "point": {"x": 1, "y": 2, "z": 3}}')
    Point3D(x=1, y=2, z=3)


``Any``
-------

When decoding a message with `Any` type (or no type specified), encoded types
map to Python types as follows.

**JSON**

JSON_ types are decoded to Python types as follows:

- ``null``: `None`
- ``bool``: `bool`
- ``string``: `str`
- ``number``: `int` or `float` [#number_json]_
- ``array``: `list`
- ``object``: `dict`

.. [#number_json] Numbers are decoded as integers if they contain no decimal or
   exponent components (e.g. ``1`` but not ``1.0`` or ``1e10``). All other
   numbers decode as floats.



.. _type annotations: https://docs.python.org/3/library/typing.html
.. _JSON: https://json.org
.. _pydantic: https://pydantic.dev/docs/validation/latest/get-started/
.. _pendulum: https://pendulum.eustace.io/
.. _RFC8259: https://datatracker.ietf.org/doc/html/rfc8259
.. _RFC3339: https://datatracker.ietf.org/doc/html/rfc3339
.. _RFC4122: https://datatracker.ietf.org/doc/html/rfc4122
.. _ISO8601: https://en.wikipedia.org/wiki/ISO_8601
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _attrs: https://www.attrs.org/en/stable/index.html
.. _timezone-aware: https://docs.python.org/3/library/datetime.html#aware-and-naive-objects
.. _mypy: https://mypy.readthedocs.io/en/stable/
.. _pyright: https://github.com/microsoft/pyright
.. _generic types:
.. _user-defined generic types: https://docs.python.org/3/library/typing.html#user-defined-generic-types
.. _open an issue: https://github.com/tds333/structtype/issues
.. _ISO 8601 duration strings: https://en.wikipedia.org/wiki/ISO_8601#Durations
