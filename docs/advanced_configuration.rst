Configuration
=============

Metaclasses
-----------

You can define project-wide :class:`structtype.Struct` policies at class-creation
time by extending the :class:`structtype.StructMeta` metaclass.

In the following example, we flip the default value of ``kw_only`` to ``True``
in all subclasses of ``KwOnlyStruct``.

.. code-block:: python

    >>> from structtype import Struct, StructMeta

    >>> class KwOnlyStructMeta(StructMeta):
    ...     def __new__(mcls, name, bases, namespace, **struct_config):
    ...         struct_config.setdefault("kw_only", True)
    ...         return super().__new__(mcls, name, bases, namespace, **struct_config)

    >>> class KwOnlyStruct(Struct, metaclass=KwOnlyStructMeta): ...

    >>> class Example(KwOnlyStruct):
    ...     a: str = ""
    ...     b: int

    >>> Example()
    Traceback (most recent call last):
      File "<python-input-3>", line 1, in <module>
        Example()
        ~~~~~~~^^
    TypeError: Missing required argument 'b'

    >>> Example(b=123)
    Example(a='', b=123)

You can also mix :class:`structtype.StructMeta` with other metaclasses. One common
use case is combining it with :class:`abc.ABCMeta` to define abstract base
Structs.

.. code-block:: python

    >>> from abc import ABCMeta, abstractmethod

    >>> from structtype import Struct, StructMeta

    >>> class EventMeta(StructMeta, ABCMeta): ...

    >>> class Event(Struct, metaclass=EventMeta):
    ...     id: int
    ...     @abstractmethod
    ...     def kind(self) -> str: ...
    ...
    >>> Event(id=1)
    Traceback (most recent call last):
      File "<python-input-4>", line 1, in <module>
        Event(id=1)
        ~~~~~^^^^^^
    TypeError: Can't instantiate abstract class Event without an implementation for abstract method 'kind'

    >>> class UserCreated(Event):
    ...     username: str
    ...     def kind(self) -> str:
    ...         return "user_created"
    ...
    >>> UserCreated(id=1, username="alice")
    UserCreated(id=1, username='alice')

Here :class:`structtype.Struct` participates fully in the ABC machinery:
abstract base Structs (like ``Event``) cannot be instantiated, and
:func:`isinstance` and :func:`issubclass` checks behave the same as for normal
ABCs.

.. important::

    - Classes with a :class:`structtype.StructMeta`-derived metaclass do not
      *technically* need to inherit from :class:`structtype.Struct`, but it is
      recommended to do so for static typing support in IDEs and other tools.
    - Mixing :class:`structtype.StructMeta` with arbitrary metaclasses
      is not supported. Only combinations involving :class:`abc.ABCMeta`
      (or its subclasses) are guaranteed to work. Prefer using
      :meth:`object.__init_subclass__` on a :class:`structtype.Struct` base class
      instead of additional custom metaclasses.

Inspecting Configuration
------------------------

Every struct type and instance exposes its configuration through the read-only
``__struct_config__`` attribute, which returns a :class:`structtype.StructConfig`
object:

.. code-block:: python

    >>> from structtype import Struct

    >>> class User(Struct, frozen=True, tag="user"):
    ...     name: str
    ...     groups: set[str] = set()
    ...

    >>> User.__struct_config__.frozen
    True
    >>> User.__struct_config__.tag
    'user'
    >>> User("alice").__struct_config__.tag
    'user'

The exposed attributes mirror the struct configuration parameters of the same
name described in the :class:`structtype.Struct` docstring, with the exception
of ``kw_only`` and ``rename``, which are consumed at class creation time and
are not available on the object. See the :doc:`api` reference for the full
list of attributes.


.. _struct-replace:

``__replace__``
---------------

Struct types generate a ``__replace__`` method that returns a new struct
instance with some fields replaced. This is similar to `dataclasses.replace`.

.. code-block:: python

    >>> from structtype import Struct, Field

    >>> class Point(Struct):
    ...     x: float
    ...     y: float

    >>> p = Point(1.0, 2.0)
    >>> p.__replace__(x=3.0)
    Point(x=3.0, y=2.0)

The ``__replace__`` method works on frozen structs as well, since it creates a
new instance rather than modifying the existing one.

.. _struct-validate-on-init:

``validate_on_init``
--------------------

By default, struct instances are not type-checked on creation. Type
validation only occurs when decoding a message (e.g. via
``struct_validate_json``). Setting ``validate_on_init=True`` on a struct
definition enables type and constraint checking during ``__init__``:

.. code-block:: python

    >>> from structtype import Struct

    >>> class Point(Struct, validate_on_init=True):
    ...     x: float
    ...     y: float

    >>> Point(1, 2)  # valid
    Point(x=1.0, y=2.0)

    >>> Point("bad", 2)  # invalid
    Traceback (most recent call last):
      ...
    structtype.ValidationError: Expected `float`, got `str`

While convenient, this adds overhead to every ``__init__`` call. Prefer static
type checking with mypy_/pyright_ when possible, and use ``struct_validate_self`` for
explicit runtime validation.

``repr_omit_defaults``
----------------------

When set to ``True``, the generated ``__repr__`` for a struct will omit fields
whose values match their default:

.. code-block:: python

    >>> from structtype import Struct

    >>> class User(Struct, repr_omit_defaults=True):
    ...     name: str
    ...     groups: set[str] = set()
    ...     email: str | None = None

    >>> User("alice")  # "groups" and "email" omitted from repr
    User(name='alice')

    >>> User("bob", email="bob@company.com")
    User(name='bob', email='bob@company.com')

This can make ``__repr__`` output more readable for structs with many optional
fields.

``cache_hash``
--------------

If a struct is frozen (``frozen=True``), it will compute a hash value when
first hashed and cache it for subsequent calls. This can improve performance
when the same frozen struct instances are used repeatedly as dictionary keys
or set members. Enable with ``cache_hash=True``:

.. code-block:: python

    >>> from structtype import Struct

    >>> class Point(Struct, frozen=True, cache_hash=True):
    ...     x: float
    ...     y: float

    >>> p = Point(1.0, 2.0)
    >>> hash(p)  # computes and caches the hash
    >>> hash(p)  # uses cached value

Note that cached hashes are never invalidated. If you create a frozen struct
with mutable fields (e.g. a ``list``), the hash will be computed from the
field values at the time of first hashing. Mutating those fields after that
will result in an inconsistent hash.

``weakref``
-----------

By default, struct instances do not support :mod:`weakref`\ erences. To enable
weak reference support, set ``weakref=True``:

.. code-block:: python

    >>> from structtype import Struct
    >>> import weakref

    >>> class Node(Struct, weakref=True):
    ...     value: int

    >>> n = Node(42)
    >>> r = weakref.ref(n)
    >>> r() is n
    True

Enabling weak references adds a small overhead to every struct instance and
increases memory usage by one pointer per instance.

.. _struct-fields-function:

The ``fields()`` Function
-------------------------

The ``fields()`` function returns introspection information about a struct's
fields as a tuple of ``FieldInfo`` objects:

.. code-block:: python

    >>> from structtype import Struct, fields

    >>> class Point(Struct):
    ...     x: float
    ...     y: float

    >>> fields(Point)
    (FieldInfo(name='x', alias='x', type=float, default=NODEFAULT, default_factory=NODEFAULT),
     FieldInfo(name='y', alias='y', type=float, default=NODEFAULT, default_factory=NODEFAULT))

    >>> fields(Point(1.0, 2.0))  # also works on instances
    (FieldInfo(...), FieldInfo(...))

Each ``FieldInfo`` has the following attributes:

- ``name``: the Python field name.
- ``alias``: the name used for serialization (affected by ``rename``/``alias``).
- ``type``: the field's type annotation.
- ``default``: the default value, or ``NODEFAULT`` if required.
- ``default_factory``: the default factory callable, or ``NODEFAULT`` if none.
- ``required``: a property returning ``True`` if the field has no default value.

``dict``
--------

By default, struct instances use ``__slots__`` for memory efficiency and do not
have a ``__dict__`` attribute. Setting ``dict=True`` enables ``__dict__``
support, allowing arbitrary attributes to be set on instances at runtime:

.. code-block:: python

    >>> from structtype import Struct

    >>> class Node(Struct, dict=True):
    ...     value: int

    >>> n = Node(42)
    >>> n.extra = "metadata"  # arbitrary attribute via __dict__
    >>> n.__dict__
    {'extra': 'metadata'}

This is useful when you need to attach extra runtime state to struct instances,
but it increases memory usage per instance.


.. _mypy: https://mypy.readthedocs.io/en/stable/
.. _pyright: https://github.com/microsoft/pyright
.. _reference counting: https://en.wikipedia.org/wiki/Reference_counting
.. _cyclic garbage collector: https://github.com/python/cpython/blob/main/InternalDocs/garbage_collector.md
