API Docs
========

Structs
-------

.. currentmodule:: structtype

.. autoclass:: Struct
    :members: struct_dump_json, struct_validate_json, struct_dump, struct_validate,
              struct_check_types

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

.. autoclass:: StructMeta(name, bases, namespace, /)

.. autofunction:: structtype.fields

.. autofunction:: structtype.json_schema

.. autofunction:: structtype.json_schema_dump

.. autofunction:: structtype.json_schema_components

.. autoclass:: structtype.FieldInfo
    :members: required

    .. note::

       ``FieldInfo`` is the *read* side of field metadata — you obtain it by
       calling :func:`structtype.fields`. To *set* metadata (aliases,
       descriptions, JSON schema extra, …) on a field, use
       :class:`typing.Annotated` with :class:`Field` in the annotation —
       see :doc:`annotation`.

    .. attribute:: name

        The Python field name (the name used in ``__init__`` and as a dict
        key).

    .. attribute:: alias

        The name used for serialization — equals *name* unless
        ``Field(alias=...)`` or ``StructConfig(rename=...)`` is used.

    .. attribute:: type

        The field's type annotation.

    .. attribute:: default

        The default value, or ``NODEFAULT`` for required fields.

    .. attribute:: default_factory

        A callable that produces the default value, or ``NODEFAULT`` if
        none.

    **Example:**

    .. code-block:: python

        >>> from structtype import Struct, fields, Factory
        >>> class User(Struct):
        ...     name: str
        ...     age: int = 0
        >>> for info in fields(User):
        ...     print(f"{info.name}: required={info.required}")
        name: required=True
        age: required=False

.. autoclass:: structtype.StructConfig

    A :class:`typing.TypedDict` with ``total=False`` containing 15 optional
    keys. ``StructConfig(frozen=True)`` returns a plain ``dict`` at runtime.

    .. important::

        ``isinstance(x, StructConfig)`` raises ``TypeError`` on all Python
        versions. Use ``isinstance(x, dict)`` instead, or access individual
        keys directly.

    Keys:

    .. attribute:: frozen

        :type: bool

        Make instances immutable — ``setattr``/``delattr`` raise
        ``FrozenInstanceError``. Enables ``__hash__`` (only when
        ``eq=True``, the default). See :ref:`struct-frozen`.

    .. attribute:: eq

        :type: bool

        Generate ``__eq__`` and ``__ne__``. Enabled by default.

    .. attribute:: order

        :type: bool

        Generate ``__lt__``, ``__le__``, ``__gt__``, ``__ge__``.
        Requires ``eq=True``. See :doc:`usage` (Field Ordering).

    .. attribute:: kw_only

        :type: bool

        All fields are keyword-only in ``__init__``. Enabled by default.

    .. attribute:: repr_omit_defaults

        :type: bool

        Omit fields equal to their default value in ``__repr__``.
        Described in :doc:`advanced_configuration`.

    .. attribute:: array_like

        :type: bool

        Encode as positional array (``list``/``tuple``) instead of a
        JSON object. Described in :doc:`supported-types`.

    .. attribute:: weakref

        :type: bool

        Allow ``weakref.ref`` on instances (one pointer per instance).
        Described in :doc:`advanced_configuration`.

    .. attribute:: dict

        :type: bool

        Give instances a ``__dict__`` for arbitrary runtime attributes.
        Described in :doc:`advanced_configuration`.

    .. attribute:: cache_hash

        :type: bool

        Cache the hash of frozen struct instances after the first
        ``hash()`` call. Described in :doc:`advanced_configuration`.

    .. attribute:: omit_defaults

        :type: bool

        Omit fields equal to their default value in
        ``struct_dump_json`` output. See :ref:`omit_defaults`.

    .. attribute:: forbid_unknown_fields

        :type: bool

        Raise ``ValidationError`` on unknown keys in
        ``struct_validate``/``struct_validate_json``.
        See :ref:`forbid-unknown-fields`.

    .. attribute:: check_types_on_init

        :type: bool

        Run type and constraint validation on every ``__init__`` call.
        See :ref:`struct-check-types-on-init`.

    .. attribute:: tag

        :type: bool | str | int | Callable[[str], str | int] | None

        The discriminant value used for tagged unions — either a fixed
        value or a callable that maps the class name to one. Set to
        ``False`` to explicitly disable tagging. See
        :ref:`struct-tagged-unions`.

    .. attribute:: tag_field

        :type: str | None

        The key used to store the discriminant value. Defaults to
        ``"type"`` when either *tag* or *tag_field* is set. See
        :ref:`struct-tagged-unions`.

    .. attribute:: rename

        :type: None | Literal["lower", "upper", "camel", "pascal", "kebab"] | Callable[[str], str | None] | Mapping[str, str]

        Rename fields for serialization. Accepts a preset string
        (``"lower"``, ``"upper"``, ``"camel"``, ``"pascal"``,
        ``"kebab"``), a callable ``(name) -> new_name | None``, or a
        dict mapping original names to serialized names.
        See :ref:`renaming-fields`.

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
        encoding. Only valid for custom types; attaching a ``dump=`` Serializer to a
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
        ``struct_check_types`` / ``check_types_on_init``, if the field value is
        not already an instance of the custom type, a ``ValidationError`` is
        raised instead — ``load`` is **not** called. Only valid for custom
        types; attaching a ``load=`` Serializer to a natively supported type or a
        union (including optional types such as
        ``Annotated[complex | None, ...]``) raises a ``TypeError`` — at
        class creation time for ``Struct``, at construction for
        ``StructAdapter``. See :doc:`extending`.


Constraint
----------

.. autoclass:: Constraint
    :members:


NumericConstraint
-----------------

.. autoclass:: NumericConstraint
    :members:


StrConstraint
-------------

.. autoclass:: StrConstraint
    :members:


BytesConstraint
---------------

.. autoclass:: BytesConstraint
    :members:


CollectionConstraint
--------------------

.. autoclass:: CollectionConstraint
    :members:


TimezoneConstraint
------------------

.. autoclass:: TimezoneConstraint
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
