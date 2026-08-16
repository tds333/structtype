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
