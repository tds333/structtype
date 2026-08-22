JSON Schema
===========

``structtype`` provides a few utilities for generating `JSON Schema`_
specifications from structtype-compatible :doc:`types <supported-types>` and
:doc:`Field Annotations <annotation>`.

- `json_schema()`: a function that generates a complete JSON Schema as a Python dict.
- `json_schema_dump()`: a convenience wrapper that returns the schema as JSON bytes.
- `json_schema_components()`: generates schemas for multiple types at once, returning
  per-type schemas and shared component definitions. Usable in other frameworks.


The generated schemas are compatible with `JSON Schema`_ 2020-12 and OpenAPI_
3.1.


Example
-------


.. code-block:: python

    from structtype import json_schema, json_schema_dump
    from structtype import Struct, NumericValidator
    from typing import Annotated


    class Dimensions(Struct):
        length: Annotated[float, NumericValidator(gt=0)]
        width: Annotated[float, NumericValidator(gt=0)]
        height: Annotated[float, NumericValidator(gt=0)]


    class Product(Struct):
        id: int
        name: str
        tags: set[str] = set()
        dimensions: Dimensions | None = None


    # Generate a schema as a Python dict
    result = json_schema(Product)

    # Or get it directly as JSON bytes
    json_bytes = json_schema_dump(Product)


.. code-block:: json

    {
      "$ref": "#/$defs/Product",
      "$defs": {
        "Product": {
          "title": "Product",
          "type": "object",
          "properties": {
            "id": {"type": "integer"},
            "name": {"type": "string"},
            "tags": {
              "type": "array",
              "items": {"type": "string"},
              "uniqueItems": true,
              "default": []
            },
            "dimensions": {
              "anyOf": [
                {"$ref": "#/$defs/Dimensions"},
                {"type": "null"}
              ],
              "default": null
            }
          },
          "required": ["id", "name"]
        },
        "Dimensions": {
          "title": "Dimensions",
          "type": "object",
          "properties": {
            "length": {"type": "number", "exclusiveMinimum": 0},
            "width": {"type": "number", "exclusiveMinimum": 0},
            "height": {"type": "number", "exclusiveMinimum": 0}
          },
          "required": ["length", "width", "height"]
        }
      }
    }


Schema Components
-----------------

If you need to generate schemas for multiple related types, use
`json_schema_components()`. This is more efficient than calling `json_schema()`
in a loop, and returns separate per-type schemas together with shared component
definitions:

.. code-block:: python

    >>> from structtype import json_schema_components

    >>> schemas, components = json_schema_components([Dimensions, Product])

    >>> len(schemas)
    2

    >>> # schemas[0] corresponds to the Dimensions type
    ... schemas[0]
    {'$ref': '#/$defs/Dimensions', ...}

    >>> # schemas[1] corresponds to the Product type
    ... schemas[1]
    {'$ref': '#/$defs/Product', ...}

    >>> # components is a dict mapping type names to their sub-schemas
    ... list(components.keys())
    ['Dimensions', 'Product']

    >>> # For a single type, json_schema_dump is a convenience shortcut
    ... from structtype import json_schema_dump
    ... json_schema_dump(Product)
    b'...'

Both functions accept the following keyword arguments:

- ``schema_hook``: A callable that can modify the generated schema for any
  type. It receives the original type and should return a dict of schema
  properties to merge.
- ``ref_template``: A string template for ``$ref`` paths. Defaults to
  ``"#/$defs/{name}"``.

Customizing the Schema
----------------------

You can enrich the generated JSON Schema using several ``Field`` parameters
(``title``, ``description``, ``examples``, ``deprecated``, ``json_schema_extra``)
and validation via ``NumericValidator``.
These are covered on the :doc:`Field Annotations <annotation>` page.

.. _JSON Schema: https://json-schema.org/
.. _OpenAPI: https://www.openapis.org/
