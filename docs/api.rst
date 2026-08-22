API Docs
========

Structs
-------

.. currentmodule:: structtype

.. autoclass:: Struct
    :members: struct_dump_json, struct_validate_json, struct_dump, struct_validate,
              struct_validate_self

    .. attribute:: __struct_fields__

        :type: tuple[str, ...]

        A tuple of the field names in declaration order. Available on both the
        struct type and on instances.

    .. attribute:: __struct_alias_fields__

        :type: tuple[str, ...]

        A tuple of the field names used for serialization, in declaration
        order. These are the field names after applying any ``rename``
        configuration. Available on both the struct type and on instances.

    .. attribute:: __struct_defaults__

        :type: tuple[Any, ...]

        A tuple of the default values for each field, in declaration order.
        Fields without a default use ``NODEFAULT``. Available on both the
        struct type and on instances.

    .. attribute:: __struct_config__

        :type: StructConfig

        The :class:`StructConfig` for this struct type. Available on both the
        struct type and on instances.

    .. attribute:: __match_args__

        :type: tuple[str, ...]

        A tuple of the field names used for positional pattern matching.
        Available on the struct type.

    .. attribute:: __signature__

        :type: inspect.Signature

        The computed ``__init__`` signature for the struct type. Available on
        the struct type.

    .. attribute:: __slots__

        :type: tuple[str, ...]

        The field names stored as instance slots. On the base ``Struct`` class
        this is empty; subclasses get one slot per field.

    .. method:: __iter__()

        Iterate over the struct as ``(name, value)`` pairs in declaration
        order. This enables ``dict(p)``, ``list(p)``, and ``iter(p)`` support.

    .. method:: __copy__()

        Return a shallow copy of the struct, enabling ``copy.copy(p)``.

    .. method:: __replace__(**changes)

        Create a new struct with the given fields replaced, enabling
        ``copy.replace(p, ...)``.

    .. method:: __reduce__()

        Return state information for pickling, enabling ``pickle.dump`` and
        ``copy.deepcopy`` support.

    .. method:: __rich_repr__()

        Return a list of ``(name, value)`` pairs used by IPython and Jupyter
        for a structured display of the struct.

.. autoclass:: StructMeta(name, bases, namespace, /, *, **struct_config)

.. autofunction:: structtype.fields

.. autofunction:: structtype.json_schema

.. autofunction:: structtype.json_schema_dump

.. autofunction:: structtype.json_schema_components

.. autoclass:: structtype.FieldInfo
    :members:

.. autoclass:: structtype.StructConfig

    .. attribute:: frozen

        :type: bool

    .. attribute:: eq

        :type: bool

    .. attribute:: order

        :type: bool

    .. attribute:: repr_omit_defaults

        :type: bool

    .. attribute:: array_like

        :type: bool

    .. attribute:: weakref

        :type: bool

    .. attribute:: dict

        :type: bool

    .. attribute:: cache_hash

        :type: bool

    .. attribute:: omit_defaults

        :type: bool

    .. attribute:: forbid_unknown_fields

        :type: bool

    .. attribute:: validate_on_init

        :type: bool

    .. attribute:: tag

        :type: str | int | None

    .. attribute:: tag_field

        :type: str | None

    .. note::

        The ``kw_only`` and ``rename`` struct configuration options are
        consumed at class creation time and are not exposed as attributes on
        :class:`StructConfig`. See the :class:`Struct` docstring's
        "Configuration" section for details.

.. autodata:: NODEFAULT
   :no-value:

.. data:: ALL_BUILTIN_TYPES

   :type: tuple[type, ...]

   A tuple of all builtin types. Can be used for ``struct_dump`` argument ``builtin_types``
   to pass through all builtin types unchanged. ::

       >>> from structtype import ALL_BUILTIN_TYPES
       >>> obj.struct_dump(builtin_types=ALL_BUILTIN_TYPES)

Field
-----

.. autoclass:: Field
    :members:


Factory
-------

.. autoclass:: Factory
    :members:


Serializer
----------

.. autoclass:: Serializer
    :members:

    .. attribute:: dump
        :no-index:

        :type: Callable[[Any], Any] | None

        A callable converting a custom-type value into a value composed of
        :doc:`natively supported <supported-types>` types. Used during
        encoding. Only valid for custom types; attaching a ``dump`` codec to a
        natively supported type or a union (including optional types such as
        ``Annotated[complex | None, ...]``) raises a ``TypeError`` — at
        class creation time for ``Struct``, at construction for
        ``StructAdapter``. See :doc:`extending`.

    .. attribute:: load
        :no-index:

        :type: Callable[[Any], Any] | None

        A callable converting a value composed of natively supported types
        back into a custom-type value. Called during decoding
        (``struct_validate`` / ``struct_validate_json``). In
        ``struct_validate_self`` / ``validate_on_init``, if the field value is
        not already an instance of the custom type, a ``ValidationError`` is
        raised instead — ``load`` is **not** called. Only valid for custom
        types; attaching a ``load`` codec to a natively supported type or a
        union (including optional types such as
        ``Annotated[complex | None, ...]``) raises a ``TypeError`` — at
        class creation time for ``Struct``, at construction for
        ``StructAdapter``. See :doc:`extending`.


Validator
---------

.. autoclass:: Validator
    :members:


NumericValidator
----------------

.. autoclass:: NumericValidator
    :members:


StrValidator
------------

.. autoclass:: StrValidator
    :members:


BytesValidator
--------------

.. autoclass:: BytesValidator
    :members:


CollectionValidator
-------------------

.. autoclass:: CollectionValidator
    :members:


TimezoneValidator
-----------------

.. autoclass:: TimezoneValidator
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


Exceptions
----------

.. currentmodule:: structtype


.. autoexception:: EncodeError
    :show-inheritance:

.. autoexception:: DecodeError
    :show-inheritance:

.. autoexception:: ValidationError
    :show-inheritance:
