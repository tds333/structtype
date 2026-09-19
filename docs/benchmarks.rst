Benchmarks
==========

.. note::

    Benchmarks are *hard*.

    Repeatedly calling the same function in a tight loop will lead to the
    instruction cache staying hot and branches being highly predictable. That's
    not representative of real world access patterns. It's also hard to write a
    nonbiased benchmark. I wrote structtype, naturally whatever benchmark I
    publish it's going to perform well in.

    Even so, people like to see benchmarks. I've tried to be as nonbiased as I
    can be, and the results hopefully indicate a few tradeoffs you make when
    you choose different serialization formats. I encourage you to write your
    own benchmarks before making these decisions.

    In all cases benchmarks were run on my local development machine (~2020
    x86 Linux desktop) using CPython 3.15t (free-threaded).


.. _library-comparison:

Library Comparison
------------------

This benchmark compares ``structtype`` against ``msgspec`` and ``pydantic``
across two common data shapes:

**E-commerce data** — flat structs with lists and optional fields (``Item`` /
``Order``):

.. code-block:: python

    from structtype import Struct, StructConfig

    class Item(Struct):
        name: str
        price: float
        tags: list[str] = []
        metadata: dict[str, str] | None = None

    class Order(Struct):
        struct_config = StructConfig(kw_only=True)
        id: int
        customer: str
        items: list[Item]
        created_at: str
        status: str = "pending"

A set of 500 randomized orders is used to measure:

- ``struct → dict`` serialization
- ``dict → struct`` validation
- ``struct → JSON`` encoding
- ``JSON → struct`` decoding

**Tagged union data** — recursively nested ``File`` / ``Dir`` tree:

.. code-block:: python

    from structtype import Struct, StructConfig

    class File(Struct):
        struct_config = StructConfig(tag="file", kw_only=True)
        name: str
        size: int

    class Dir(Struct):
        struct_config = StructConfig(tag="dir", kw_only=True)
        name: str
        contents: list[File | Dir]

A single deeply nested tree (depth 4, branching up to 5) is used to measure
JSON encode/decode performance specifically.

The full benchmark source can be found
`here <https://github.com/tds333/structtype/blob/main/benchmarks/bench_libs.py>`__.

.. code-block:: text
    :caption: Historical snapshot: Python 3.15.0rc2 (free-threaded), structtype 0.13.1.dev9, msgspec 0.21.1, pydantic 2.13.5

    Dump (struct → dict)
    -------------------------------------------------------
      structtype           1299.2 μs   (1.00x)
      msgspec              1335.2 μs   (1.03x)
      pydantic             5811.3 μs   (4.47x)

    Load (dict → struct)
    -------------------------------------------------------
      structtype           1445.8 μs   (1.01x)
      msgspec              1424.8 μs   (1.00x)
      pydantic             6823.5 μs   (4.79x)

    Dump JSON (struct → bytes)
    -------------------------------------------------------
      structtype            927.8 μs   (1.00x)
      msgspec              1067.0 μs   (1.15x)
      pydantic             4682.7 μs   (5.05x)

    Load JSON (bytes → struct)
    -------------------------------------------------------
      structtype           2184.4 μs   (1.02x)
      msgspec              2148.9 μs   (1.00x)
      pydantic             8849.7 μs   (4.12x)

    Dump JSON (tagged union)
    -------------------------------------------------------
      structtype              1.5 μs   (1.00x)
      msgspec                 1.8 μs   (1.20x)
      pydantic               27.5 μs   (18.33x)

    Load JSON (tagged union)
    -------------------------------------------------------
      structtype              4.4 μs   (1.02x)
      msgspec                 4.3 μs   (1.00x)
      pydantic               26.3 μs   (6.12x)

For flat data the two libraries trade places: ``structtype`` is fastest at
encoding — 3% ahead of ``msgspec`` for ``struct → dict`` and 15% for ``struct →
JSON`` — while ``msgspec`` edges ahead on decoding (under 1% for ``dict →
struct`` and ~2% for ``JSON → struct``). ``pydantic`` is 4.1–5.1x slower
throughout. The tagged union benchmark shows the same split: ``structtype`` is
20% faster to encode and ``msgspec`` 2% faster to decode, with ``pydantic``
6.1–18.3x behind.

Why is it faster than pydantic? 

``structtype`` is highly optimized in C. Pydantic's core is in Rust, but it is
still slower than pure C. Pydantic also has more features, at the cost of
performance.


Constraints & Serializers
-------------------------

Here we benchmark `structtype.Struct` types that make heavy use of
:doc:`Constraint <annotation>` and :doc:`Serializer <extending>` annotations
on every field, comparing against an equivalent ``pydantic`` schema using
``Field(gt=, le=, min_length=, max_length=, pattern=)`` plus
``@field_validator`` / ``@field_serializer``.

Each field carries either a constraint check or a custom-type Serializer:

- numeric constraints (``NumericConstraint`` ↔ ``Field(gt=, le=)``)
- string constraints (``StrConstraint`` ↔ ``Field(min_length=, pattern=)``)
- collection constraints (``CollectionConstraint`` ↔ ``Field(min_length=)``)
- a custom ``PostalCode`` type converted via ``Serializer(dump=, load=)``
  (↔ pydantic ``@field_serializer`` / ``@field_validator``)

Operations measured, all over 500 orders:

- ``Load`` — ``struct_validate`` vs ``model_validate`` (validators on input)
- ``Dump`` — ``struct_dump`` vs ``model_dump`` (dump serializers)
- ``Load JSON`` / ``Dump JSON`` — the same via JSON serialization
- ``Init (no validation)`` — constructor only. structtype does **not**
  validate on ``__init__`` by default; pydantic always does.
- ``Init (with validation)`` — the same with ``check_types_on_init=True`` on the
  structtype side, so both libraries validate on construction.

The full benchmark source can be found `here
<https://github.com/tds333/structtype/blob/main/benchmarks/bench_validators.py>`__.
Run it with ``make bench-validators``.

.. code-block:: text
    :caption: Historical snapshot: Python 3.15.0rc2 (free-threaded), structtype 0.13.1.dev9, pydantic 2.13.5

    Load (dict -> object)
    -------------------------------------------------------
      structtype            371.2 μs   (1.00x)
      pydantic             1002.9 μs   (2.70x)

    Dump (object -> dict)
    -------------------------------------------------------
      structtype            166.6 μs   (1.00x)
      pydantic              858.8 μs   (5.15x)

    Load JSON (bytes -> object)
    -------------------------------------------------------
      structtype            506.5 μs   (1.00x)
      pydantic             1152.2 μs   (2.27x)

    Dump JSON (object -> bytes)
    -------------------------------------------------------
      structtype            143.2 μs   (1.00x)
      pydantic              856.2 μs   (5.98x)

    Init (no validation)
    -------------------------------------------------------
      structtype             92.4 μs   (1.00x)
      pydantic              991.7 μs   (10.73x)

    Init (with validation)
    -------------------------------------------------------
      structtype            334.6 μs   (1.00x)
      pydantic              991.2 μs   (2.96x)

With every field doing validation or Serializer conversion work, structtype is
~2.3–6.0x faster than pydantic on load/dump operations. The largest gap is
construction: structtype's default (no init validation) is ~10.7x faster, and
even with ``check_types_on_init=True`` it is still ~3.0x faster than pydantic's
always-on construction-time validation.


.. _struct-benchmark:

Dataclass like libs
-------------------

Here we benchmark common `structtype.Struct` operations, comparing their
performance against other similar libraries. The cases compared are:

- Standard Python classes
- dataclasses_
- structtype_ (0.13.1.dev9)
- msgspec_ (0.21.1)
- attrs_ (26.1.0)
- pydantic_ (2.13.5)

For each library, the following operations are benchmarked:

- Time to define a new class. Many libraries that abstract away class
  boilerplate add overhead when defining classes, slowing import times for
  libraries that make use of these classes.
- Time to create an instance of that class.
- Time to compare two instances for equality (``==`` / ``!=``).
- Time to compare two instances for order (``<`` / ``>`` / ``<=`` / ``>=``)

The full benchmark source can be found `here
<https://github.com/tds333/structtype/blob/main/benchmarks/bench_structs.py>`__.

- Standard Python classes are the fastest to import (any library can only add
  overhead here). Still, ``structtype`` isn't *that* much slower, especially
  compared to other options.
- Structs are optimized to be cheap to create, and that shows for the creation
  benchmark. They're roughly 4x faster than standard
  classes/``attrs``/``dataclasses``, and 17x faster than ``pydantic``.
- For equality comparison, structtype Structs are roughly 4x to 93x faster than
  the alternatives.
- For order comparison, structtype Structs are roughly 4x to 59x faster than the
  alternatives.

.. code-block:: text
    :caption: Historical snapshot: Python 3.15.0rc2 (free-threaded), structtype 0.13.1.dev9, attrs 26.1.0, msgspec 0.21.1, pydantic 2.13.5

    +----------------------+-------------+-------------+---------------+------------+
    |                      | import (μs) | create (μs) | equality (μs) | order (μs) |
    +======================+=============+=============+===============+============+
    | **structtype**       | 19.17       | 0.07        | 0.01          | 0.03       |
    +----------------------+-------------+-------------+---------------+------------+
    | **msgspec**          | 15.04       | 0.07        | 0.01          | 0.03       |
    +----------------------+-------------+-------------+---------------+------------+
    | **standard classes** | 13.65       | 0.30        | 0.05          | 0.13       |
    +----------------------+-------------+-------------+---------------+------------+
    | **attrs**            | 395.30      | 0.27        | 0.04          | 1.78       |
    +----------------------+-------------+-------------+---------------+------------+
    | **dataclasses**      | 379.02      | 0.29        | 0.05          | 0.13       |
    +----------------------+-------------+-------------+---------------+------------+
    | **pydantic**         | 294.69      | 1.16        | 0.93          | N/A        |
    +----------------------+-------------+-------------+---------------+------------+

.. _struct-gc-benchmark:

Garbage Collection
------------------

`structtype.Struct` instances implement several optimizations for reducing garbage
collection (GC) pressure and decreasing memory usage. Here we benchmark structs
against standard Python classes (with and without `__slots__
<https://docs.python.org/3/reference/datamodel.html#slots>`__).

For each option we create a large dictionary containing many simple instances
of the benchmarked type, then measure:

- The amount of time it takes to do a full garbage collection (gc) pass
- The total amount of memory used by this data structure

The full benchmark source can be found `here
<https://github.com/tds333/structtype/blob/main/benchmarks/bench_gc.py>`__.

- Standard Python classes are the most memory hungry (since all data is stored
  in an instance dict). They also result in the largest GC pause, as the GC has
  to traverse the entire outer dict, each class instance, and each instance
  dict. All that pointer chasing has a cost.

- Standard classes with ``__slots__`` are less memory hungry, but still results
  in an equivalent GC pauses.

- `structtype.Struct` instances have the same memory layout as a class with
  ``__slots__`` (and thus have the same memory usage), but due to deferred GC
  tracking a full GC pass completes in a fraction of the time.

.. code-block:: text
    :caption: Historical snapshot: Python 3.15.0rc2 (free-threaded), structtype 0.13.1.dev9

    +-----------------------------------+--------------+-------------------+
    |                                   | GC time (ms) | Memory Used (MiB) |
    +===================================+==============+===================+
    | **standard class**                | 48.97        | 219.29            |
    +-----------------------------------+--------------+-------------------+
    | **standard class with __slots__** | 41.78        | 135.37            |
    +-----------------------------------+--------------+-------------------+
    | **structtype struct**             | 22.84        | 135.37            |
    +-----------------------------------+--------------+-------------------+

.. _string-benchmark:

String-heavy workloads
----------------------

The JSON codec scans strings a machine word at a time to locate characters that
need escaping (SIMD where available, a portable SWAR fallback otherwise). This
benchmark round-trips a struct with a single 100,000 character ``str`` field in
three flavours — pure ASCII, non-ASCII, and escape-heavy (embedded newlines and
quotes) — and reports nanoseconds per UTF-8 byte, compared against msgspec and
pydantic.

The full benchmark source can be found `here
<https://github.com/tds333/structtype/blob/main/benchmarks/bench_strings.py>`__.

.. code-block:: text
    :caption: Historical snapshot: Python 3.15.0rc2 (free-threaded), structtype 0.13.1.dev9, msgspec 0.21.1, pydantic 2.13.5

    long str struct dump_json (100000 chars, ns/byte)
      case       structtype      msgspec     pydantic  vs pydantic
      ascii           0.067        0.283        0.492        7.34x
      nonascii        0.068        0.259        1.302       19.15x
      escape          0.126        0.277        0.577        4.58x

    long str struct validate_json (100000 chars, ns/byte)
      case       structtype      msgspec     pydantic  vs pydantic
      ascii           0.080        0.278        0.446        5.58x
      nonascii        0.655        0.882        1.630        2.49x
      escape          0.272        0.395        0.679        2.50x

.. _structtype: https://structtype.dev
.. _msgspec: https://jcristharif.com/msgspec/
.. _attrs: https://www.attrs.org/en/stable/
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _pydantic: https://pydantic.dev/docs/validation/latest/get-started/
