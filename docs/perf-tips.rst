Performance Tips
================

Here we present a few tips and tricks for squeezing maximum performance out of
``structtype``. They're presented in order from "sane, definitely a good idea" to
"fast, but you may not want to do this".


Avoid Encoding Default Values
-----------------------------

By default, ``structtype`` encodes all fields in a ``Struct`` type, including optional
fields (those configured with a default value). If the default values are known
on the decoding end (making serializing them redundant), it may be beneficial
to omit default values from the encoded message. This can be done by
configuring ``omit_defaults=True`` as part of the ``Struct`` definition.
Omitting defaults reduces the size of the encoded message, and often also improves
encoding and decoding performance (since there's less work to do).

For more information, see :ref:`omit_defaults`.


.. _avoid-decoding-unused-fields:

Avoid Decoding Unused Fields
----------------------------

When decoding large inputs, sometimes you're only interested in a few specific
fields. Since decoding large objects is inherently allocation heavy, it may be
beneficial to define a smaller `structtype.Struct` type that only has the fields
you require.

For example, say you're interested in decoding some JSON from the `Twitter API
<https://developer.twitter.com/en/docs/twitter-api/v1/data-dictionary/object-model/tweet>`__.
A ``Tweet`` object has many nested fields on it - perhaps you only care about
the tweet text, the user name, and the number of favorites. By defining struct
types with only those fields, ``structtype`` can avoid doing unnecessary work
decoding fields that are never used.

.. code-block:: python

    >>> from structtype import Struct, Field

    >>> class User(Struct):
    ...     name: str

    >>> class Tweet(Struct):
    ...     user: User
    ...     full_text: str
    ...     favorite_count: int


We can then use these types to decode the `example tweet json
<https://developer.twitter.com/en/docs/twitter-api/v1/data-dictionary/object-model/example-payloads>`__:

.. code-block:: python

    >>> tweet = Tweet.struct_validate_json(example_json)

    >>> tweet.user.name
    'Twitter Dev'

    >>> tweet.favorite_count
    70

Of course there are downsides to defining smaller "view" types, but if decoding
performance is a bottleneck in your workflow, you may benefit from this
technique.

Use ``array_like=True``
-----------------------

One touted benefit of JSON_ is that it's "self-describing"
protocols. JSON objects serialize their field names along with their values. If
both ends of a connection already know the field names though, serializing them
may be an unnecessary cost. If you need higher performance (at the cost of more
inscrutable message encoding), you can set ``array_like=True`` on a struct
definition. Structs with this option enabled are encoded/decoded like array
types, removing the field names from the encoded message. This can provide on
average another ~2x speedup for decoding (and ~1.5x speedup for encoding).

.. code-block:: python

    >>> from structtype import Struct, StructConfig

    >>> class Example(Struct):
    ...     struct_config = StructConfig(array_like=True)
    ...     my_first_field: str
    ...     my_second_field: int

    >>> x = Example("some string", 2)

    >>> msg = x.struct_dump_json()

    >>> msg
    b'["some string",2]'

    >>> Example.struct_validate_json(msg)
    Example(my_first_field="some string", my_second_field=2)


Think about type conversion
---------------------------

When converting raw data into Python types, the internal machinery
will treat different datastructures differently.
Some type conversions are faster than others.

For example, this model:

.. code-block:: python

  class Example(Struct):
      items: frozendict[str, int]  # requires Python3.15+ to be used

  msg = b'{"items": {"pen": 1, "book": 2}}'
  Example.struct_validate_json(msg)  # need to convert anyway
  # Example(items=frozendict({"pen": 1, "book": 2}))

As ``frozendict`` does not allow adding items one at a time,
``structtype`` will first parse ``items`` as a regular :class:`dict`,
and will then convert it to ``frozendict``, which results in the
total decoding operation having ``O(n*2)`` time complexity.
Which might be slow on big dictionaries and consume more memory.
Regular :class:`dict` would be more efficient to use.

The same can be said for :class:`tuple` vs :class:`list`:

.. code-block:: python

    >>> from structtype import Struct

    >>> class Example(Struct):
    ...     items: tuple[str, ...]

    >>> msg = b'{"items": ["pen", "book"]}'

    >>> Example.struct_validate_json(msg)
    Example(items=("pen", "book"))

We would first create a ``list`` object and then convert
it to variable-sized ``tuple``,
using double the memory and ``O(n*2)`` time to do that.
This is true for all conversions, that require an intermediate representation.

To achieve the best performance,
use collection types that can be constructed natively:
``list``, ``set``, ``frozenset``, fixed-sized ``tuple``, and ``dict``.


.. _JSON: https://json.org
