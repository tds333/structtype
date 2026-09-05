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
    :caption: Python 3.15t (free-threaded), structtype 0.10.1.dev, msgspec 0.21.1, pydantic 2.13.5

    Dump (struct → dict)
    -------------------------------------------------------
      structtype           1302.1 μs   (1.00x)
      msgspec              1337.5 μs   (1.03x)
      pydantic             6021.8 μs   (4.62x)

    Load (dict → struct)
    -------------------------------------------------------
      structtype           1329.3 μs   (1.00x)
      msgspec              1401.8 μs   (1.05x)
      pydantic             7581.7 μs   (5.70x)

    Dump JSON (struct → bytes)
    -------------------------------------------------------
      structtype            948.5 μs   (1.00x)
      msgspec              1098.1 μs   (1.16x)
      pydantic             5133.1 μs   (5.41x)

    Load JSON (bytes → struct)
    -------------------------------------------------------
      structtype           2245.6 μs   (1.00x)
      msgspec              2466.1 μs   (1.10x)
      pydantic             9552.4 μs   (4.25x)

    Dump JSON (tagged union)
    -------------------------------------------------------
      structtype              1.8 μs   (1.00x)
      msgspec                 1.9 μs   (1.07x)
      pydantic               28.8 μs   (16.26x)

    Load JSON (tagged union)
    -------------------------------------------------------
      structtype              4.4 μs   (1.00x)
      msgspec                 4.6 μs   (1.04x)
      pydantic               28.4 μs   (6.46x)

For flat data, ``structtype`` is consistently the fastest: ``msgspec`` runs
3–16% behind across all operations, while ``pydantic`` is 4.5–5.7x slower. The
tagged union benchmark tells the same story: ``msgspec`` runs 4–7% behind,
with ``pydantic`` 6–16x behind across all measurement types.

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
    :caption: Python 3.15t (free-threaded), structtype 0.10.1.dev, pydantic 2.13.5

    Load (dict -> object)
    -------------------------------------------------------
      structtype            384.5 μs   (1.00x)
      pydantic             1109.5 μs   (2.89x)

    Dump (object -> dict)
    -------------------------------------------------------
      structtype            192.7 μs   (1.00x)
      pydantic              937.8 μs   (4.87x)

    Load JSON (bytes -> object)
    -------------------------------------------------------
      structtype            528.8 μs   (1.00x)
      pydantic             1299.5 μs   (2.46x)

    Dump JSON (object -> bytes)
    -------------------------------------------------------
      structtype            162.3 μs   (1.00x)
      pydantic              941.6 μs   (5.80x)

    Init (no validation)
    -------------------------------------------------------
      structtype            102.2 μs   (1.00x)
      pydantic             1085.2 μs   (10.62x)

    Init (with validation)
    -------------------------------------------------------
      structtype            352.7 μs   (1.00x)
      pydantic             1086.5 μs   (3.08x)

With every field doing validation or Serializer conversion work, structtype is
~2.5–5.8x faster than pydantic on load/dump operations. The largest gap is
construction: structtype's default (no init validation) is ~10.6x faster, and
even with ``check_types_on_init=True`` it is still ~3.1x faster than pydantic's
always-on construction-time validation.


.. _struct-benchmark:

Dataclass like libs
-------------------

Here we benchmark common `structtype.Struct` operations, comparing their
performance against other similar libraries. The cases compared are:

- Standard Python classes
- dataclasses_
- structtype_ (0.10.1.dev)
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
  classes/``attrs``/``dataclasses``, and 15x faster than ``pydantic``.
- For equality comparison, structtype Structs are roughly 2.5x to 48x faster than
  the alternatives.
- For order comparison, structtype Structs are roughly 4x to 60x faster than the
  alternatives.

.. code-block:: text
    :caption: Python 3.15t (free-threaded), structtype 0.10.1.dev, attrs 26.1.0, msgspec 0.21.1, pydantic 2.13.5

    +----------------------+-------------+-------------+---------------+------------+
    |                      | import (μs) | create (μs) | equality (μs) | order (μs) |
    +======================+=============+=============+===============+============+
    | **structtype**       | 21.50       | 0.08        | 0.02          | 0.03       |
    +----------------------+-------------+-------------+---------------+------------+
    | **msgspec**          | 16.96       | 0.08        | 0.02          | 0.03       |
    +----------------------+-------------+-------------+---------------+------------+
    | **standard classes** | 15.48       | 0.32        | 0.06          | 0.15       |
    +----------------------+-------------+-------------+---------------+------------+
    | **attrs**            | 448.59      | 0.30        | 0.05          | 1.86       |
    +----------------------+-------------+-------------+---------------+------------+
    | **dataclasses**      | 386.61      | 0.29        | 0.05          | 0.12       |
    +----------------------+-------------+-------------+---------------+------------+
    | **pydantic**         | 301.63      | 1.19        | 0.96          | N/A        |
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
    :caption: Python 3.15t (free-threaded), structtype 0.10.1.dev

    +-----------------------------------+--------------+-------------------+
    |                                   | GC time (ms) | Memory Used (MiB) |
    +===================================+==============+===================+
    | **standard class**                | 52.26        | 219.29            |
    +-----------------------------------+--------------+-------------------+
    | **standard class with __slots__** | 42.77        | 135.37            |
    +-----------------------------------+--------------+-------------------+
    | **structtype struct**             | 26.80        | 135.37            |
    +-----------------------------------+--------------+-------------------+

.. _structtype: https://structtype.dev
.. _msgspec: https://jcristharif.com/msgspec/
.. _attrs: https://www.attrs.org/en/stable/
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _pydantic: https://pydantic.dev/docs/validation/latest/get-started/
