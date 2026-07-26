API Docs
========

Structs
-------

.. currentmodule:: structtype

.. autoclass:: Struct
    :members: struct_dump_json, struct_validate_json, struct_dump, struct_validate,
              struct_to_dict, struct_to_tuple, struct_force_setattr,
              struct_validate_jsonln, struct_dump_jsonln

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

.. _inspect-api:

Inspect
-------

.. currentmodule:: structtype._inspect

.. autoclass:: Type
.. autoclass:: Metadata
.. autoclass:: AnyType
.. autoclass:: NoneType
.. autoclass:: BoolType
.. autoclass:: IntType
.. autoclass:: FloatType
.. autoclass:: StrType
.. autoclass:: BytesType
.. autoclass:: ByteArrayType
.. autoclass:: MemoryViewType
.. autoclass:: DateTimeType
.. autoclass:: TimeType
.. autoclass:: DateType
.. autoclass:: TimeDeltaType
.. autoclass:: UUIDType
.. autoclass:: DecimalType
.. autoclass:: RawType
.. autoclass:: EnumType
.. autoclass:: LiteralType
.. autoclass:: CustomType
.. autoclass:: UnionType
    :members:
.. autoclass:: CollectionType
.. autoclass:: ListType
.. autoclass:: SetType
.. autoclass:: FrozenSetType
.. autoclass:: DictType
.. autoclass:: TypedDictType
.. autoclass:: FrozenDictType
.. autoclass:: VarTupleType
.. autoclass:: TupleType
.. autoclass:: NamedTupleType
.. autoclass:: DataclassType
.. autoclass:: StructType


Exceptions
----------

.. currentmodule:: structtype


.. autoexception:: EncodeError
    :show-inheritance:

.. autoexception:: DecodeError
    :show-inheritance:

.. autoexception:: ValidationError
    :show-inheritance:
