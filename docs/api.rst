API Docs
========

Structs
-------

.. currentmodule:: structtype

.. autoclass:: Struct
    :members: struct_dump_json, struct_validate_json, struct_dump, struct_validate,
              struct_validate_self

.. autoclass:: StructMeta(name, bases, namespace, /, *, **struct_config)

.. autofunction:: structtype.fields

.. autofunction:: structtype.json_schema

.. autofunction:: structtype.json_schema_dump

.. autofunction:: structtype.json_schema_components

.. autoclass:: structtype.FieldInfo
    :members:

.. autoclass:: structtype.StructConfig

.. autodata:: NODEFAULT
   :no-value:

Field
-----

.. autoclass:: Field
    :members:


Raw
---

.. currentmodule:: structtype

.. autoclass:: Raw
    :members:

Unset
-----

.. autodata:: UNSET
   :no-value:

.. autoclass:: UnsetType


StructAdapter
-------------

.. currentmodule:: structtype

.. autoclass:: StructAdapter
    :members:

StrAdapter
----------

.. autoclass:: StrAdapter
    :members:


Exceptions
----------

.. currentmodule:: structtype


.. autoexception:: EncodeError
    :show-inheritance:

.. autoexception:: DecodeError
    :show-inheritance:

.. autoexception:: ValidationError
    :show-inheritance:
