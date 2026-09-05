Examples
========

Here we provide a few examples using ``structtype`` to accomplish various tasks.

GeoJSON
-------

`GeoJSON <https://geojson.org>`__ is a popular format for encoding geographic
data. Its specification_ describes nine different types a message may take
(seven "geometry" types, plus two "feature" types). Here we provide one way of
implementing that specification using ``structtype`` to handle the parsing and
validation.

The ``loads`` and ``dumps`` methods defined below work similar to the
standard library's ``json.loads`` / ``json.dumps``, but:

- Will result in high-level `structtype.Struct` objects representing GeoJSON types
- Will error nicely if a field is missing or the wrong type
- Will fill in default values for optional fields
- Decodes and encodes *significantly faster* than the `json` module (as well as
  most other ``json`` implementations in Python).

This example makes use of `structtype.Struct` types to define the different GeoJSON
types, and :ref:`struct-tagged-unions` to differentiate between them. See the
relevant docs for more information.

The full example source can be found `here
<https://github.com/tds333/structtype/tree/main/examples/geojson>`__.

.. literalinclude:: ../examples/geojson/structtype_geojson.py
    :language: python


Here we use the ``loads`` method defined above to read some `example GeoJSON`_.

.. code-block:: ipython3

    In [1]: import structtype_geojson

    In [2]: with open("canada.json", "rb") as f:
       ...:     data = f.read()

    In [3]: canada = structtype_geojson.loads(data)

    In [4]: type(canada)  # loaded as high-level, validated object
    Out[4]: structtype_geojson.FeatureCollection

    In [5]: canada.features[0].properties
    Out[5]: {'name': 'Canada'}


.. _specification: https://datatracker.ietf.org/doc/html/rfc7946
.. _example GeoJSON: https://github.com/tds333/structtype/blob/main/examples/geojson/canada.json


.. _dynamodb-example:

DynamoDB
--------

`Amazon DynamoDB <https://aws.amazon.com/dynamodb/>`__ has no `float` type:
all numbers must be `decimal.Decimal` values. When storing structs with boto3_,
declare numeric fields as `decimal.Decimal` and pass
``builtin_types=[decimal.Decimal]`` to ``struct_dump`` — the values are kept
as `decimal.Decimal` and the result can be used directly as the ``Item``:

.. code-block:: python

    >>> import decimal

    >>> import boto3
    >>> from structtype import Struct

    >>> class Product(Struct):
    ...     name: str
    ...     price: decimal.Decimal
    ...     rating: decimal.Decimal

    >>> product = Product("widget", decimal.Decimal("9.99"), decimal.Decimal("4.5"))

    >>> table = boto3.resource("dynamodb").Table("products")
    >>> table.put_item(Item=product.struct_dump(builtin_types=[decimal.Decimal]))

Reading items back works the same way: DynamoDB returns numbers as
`decimal.Decimal`, which ``struct_validate`` accepts unchanged:

.. code-block:: python

    >>> item = table.get_item(Key={"name": "widget"})["Item"]

    >>> Product.struct_validate(item)
    Product(name='widget', price=Decimal('9.99'), rating=Decimal('4.5'))

Other types that ``struct_dump`` converts by default can be passed through the
same way — for example `bytes` fields (DynamoDB supports binary values
natively) with ``builtin_types=[decimal.Decimal, bytes]``.

.. _boto3: https://boto3.amazonaws.com
