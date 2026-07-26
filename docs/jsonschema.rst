JSON Schema
===========

``structtype`` provides a few utilities for generating `JSON Schema`_
specifications from structtype-compatible :doc:`types <supported-types>` and
:doc:`constraints <constraints>`.

- `json_schema()`: a function that generates a complete JSON Schema as a Python dict.
- `json_schema_dump()`: a convenience wrapper that returns the schema as JSON bytes.


The generated schemas are compatible with `JSON Schema`_ 2020-12 and OpenAPI_
3.1.


Example
-------


.. code-block:: python

    from structtype import json_schema, json_schema_dump
    from structtype import Struct, Field
    from typing import Annotated


    class Dimensions(Struct):
        length: Annotated[float, Field(gt=0)]
        width: Annotated[float, Field(gt=0)]
        height: Annotated[float, Field(gt=0)]


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
`json_schema_components()`. This is more efficient than calling `json_schema()` in
a loop, and returns separate definitions and the root-level schema as a tuple:

.. code-block:: python

    >>> from structtype import json_schema, json_schema_dump, json_schema_components

    >>> schemas, root = json_schema_components([Dimensions, Product])

    >>> root
    {'$ref': '#/$defs/Dimensions', '$defs': {'Dimensions': ..., 'Product': ...}}

    >>> # Encode any of these to JSON via json_schema_dump
    ... json_bytes = json_schema_dump(Product)

Both functions accept the following keyword arguments:

- ``schema_hook``: A callable that can modify the generated schema for any
  type. It receives the original type and should return a dict of schema
  properties to merge.
- ``ref_template``: A string template for ``$ref`` paths. Defaults to
  ``"#/$defs/{name}"``.

Customizing the Schema
----------------------

You can enrich the generated JSON Schema using several ``Field`` parameters.
These appear in the schema output as additional metadata.

``title`` and ``description``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

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

Provide example values that will appear in the generated schema:

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, Field

    >>> class Product(Struct):
    ...     name: Annotated[str, Field(examples=["Widget", "Gadget"])]

``json_schema_extra``
~~~~~~~~~~~~~~~~~~~~~

Add arbitrary extra properties to the generated schema for a field:

.. code-block:: python

    >>> from typing import Annotated
    >>> from structtype import Struct, Field

    >>> class Product(Struct):
    ...     sku: Annotated[str, Field(
    ...         json_schema_extra={"deprecated": True}
    ...     )]

.. _JSON Schema: https://json-schema.org/
.. _OpenAPI: https://www.openapis.org/
