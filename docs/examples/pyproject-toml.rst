pyproject.toml Schema
=====================

This example demonstrates how to define a schema for ``pyproject.toml``
configuration files using ``structtype``.

The source code is available at
``examples/pyproject-toml/pyproject.py``.

It showcases:

- Using the ``kebab`` rename option to map Python ``snake_case`` field names
  to the ``kebab-case`` keys used in ``pyproject.toml``.
- ``omit_defaults`` to reduce output size.
- ``forbid_unknown_fields`` for strict schema validation.
- Nested struct types and optional fields.

.. literalinclude:: ../../examples/pyproject-toml/pyproject.py
    :language: python
    :linenos:
