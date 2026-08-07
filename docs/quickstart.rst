Quickstart
==========

1. Define a subclass of ``Struct`` with types and defaults.
2. Use this class to validate (input) or dump (output).

Define a ``Struct`` subclass and annotate each field with a type. Optional
fields get default values:

.. code-block:: python

    >>> from structtype import Struct

    >>> class User(Struct):
    ...     name: str
    ...     groups: set[str] = set()
    ...     email: str | None = None

Create instances like any class:

.. code-block:: python

    >>> alice = User("alice", groups={"admin", "engineering"})
    >>> alice
    User(name='alice', groups={"admin", "engineering"}, email=None)

Serialize to JSON, and deserialize back with validation:

.. code-block:: python

    >>> alice.struct_dump_json()
    b'{"name":"alice","groups":["admin","engineering"],"email":null}'

    >>> User.struct_validate_json(
    ...     b'{"name":"alice","groups":["admin","engineering"],"email":null}'
    ... )
    User(name='alice', groups={"admin", "engineering"}, email=None)

Invalid messages raise a descriptive error:

.. code-block:: python

    >>> User.struct_validate_json(b'{"name":"bob","groups":[123]}')
    Traceback (most recent call last):
      File "<stdin>", line 1, in <module>
    structtype.ValidationError: Expected `str`, got `int` - at `$.groups[0]`

For more, see the :doc:`Structs & Usage <usage>` guide.
