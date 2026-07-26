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
    x86 Linux desktop) using CPython 3.14.


.. _library-comparison:

Library Comparison
------------------

This benchmark compares ``structtype`` against ``msgspec`` and ``pydantic``
across two common data shapes:

**E-commerce data** — flat structs with lists and optional fields (``Item`` /
``Order``):

.. code-block:: python

    class Item(Struct):
        name: str
        price: float
        tags: list[str] = []
        metadata: dict[str, str] | None = None

    class Order(Struct, kw_only=True):
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

    class File(Struct, tag="file", kw_only=True):
        name: str
        size: int

    class Dir(Struct, tag="dir", kw_only=True):
        name: str
        contents: list[File | Dir]

A single deeply nested tree (depth 4, branching up to 5) is used to measure
JSON encode/decode performance specifically.

The full benchmark source can be found
`here <https://github.com/tds333/structtype/blob/main/benchmarks/bench_libs.py>`__.

.. code-block:: text
    :caption: Python 3.14, structtype 0.1.1, msgspec 0.21.1, pydantic 2.13.4

    Dump (struct → dict)
    -------------------------------------------------------
      structtype           1257.3 μs   (1.00x)
      msgspec              1302.6 μs   (1.04x)
      pydantic             4247.2 μs   (3.38x)

    Load (dict → struct)
    -------------------------------------------------------
      structtype           1237.3 μs   (1.00x)
      msgspec              1333.2 μs   (1.08x)
      pydantic             6483.4 μs   (5.24x)

    Dump JSON (struct → bytes)
    -------------------------------------------------------
      structtype           1025.1 μs   (1.00x)
      msgspec              1039.8 μs   (1.01x)
      pydantic             4136.8 μs   (4.04x)

    Load JSON (bytes → struct)
    -------------------------------------------------------
      structtype           2330.7 μs   (1.00x)
      msgspec              2445.7 μs   (1.05x)
      pydantic             8064.7 μs   (3.46x)

    Dump JSON (tagged union)
    -------------------------------------------------------
      structtype              1.6 μs   (1.05x)
      msgspec                 1.6 μs   (1.00x)
      pydantic               21.1 μs   (13.61x)

    Load JSON (tagged union)
    -------------------------------------------------------
      structtype              4.3 μs   (1.02x)
      msgspec                 4.2 μs   (1.00x)
      pydantic               22.4 μs   (5.32x)

For flat data, ``structtype`` and ``msgspec`` perform within ~5% of each other
across all operations, while ``pydantic`` is 3–5x slower. The tagged union
benchmark tells the same story: both libraries are essentially tied,
with ``pydantic`` 5–14x behind across all measurement types.


.. _struct-benchmark:

Structs
-------

Here we benchmark common `structtype.Struct` operations, comparing their
performance against other similar libraries. The cases compared are:

- Standard Python classes
- dataclasses_
- structtype_ (0.18.5)
- attrs_ (23.1.0)
- pydantic_ (2.5.2)

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
- For equality comparison, structtype Structs are roughly 4x to 30x faster than
  the alternatives.
- For order comparison, structtype Structs are roughly 5x to 60x faster than the
  alternatives.

.. _struct-gc-benchmark:

Garbage Collection
------------------

`structtype.Struct` instances implement several optimizations for reducing garbage
collection (GC) pressure and decreasing memory usage. Here we benchmark structs
(with and without :ref:`gc=False <struct-gc>`) against standard Python
classes (with and without `__slots__
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

- `structtype.Struct` instances with ``gc=False`` have the lowest memory usage
  (lack of GC reduces memory by 16 bytes per instance). They also have the
  lowest GC pause (75x faster than standard classes!) since the entire
  composing dict can be skipped during GC traversal.

.. _structtype: https://structtype.dev
.. _msgspec: https://jcristharif.com/msgspec/
.. _attrs: https://www.attrs.org/en/stable/
.. _dataclasses: https://docs.python.org/3/library/dataclasses.html
.. _pydantic: https://pydantic.dev/docs/validation/latest/get-started/
