Schema Evolution
================

``structtype`` includes support for "schema evolution", meaning that:

- Messages serialized with an older version of a schema will be deserializable
  using a newer version of the schema.
- Messages serialized with a newer version of the schema will be deserializable
  using an older version of the schema.

This can be useful if, for example, you have clients and servers with
mismatched versions.

For schema evolution to work smoothly, you need to follow a few guidelines:

1. Any new fields on a `structtype.Struct` must specify default values.
2. Structs with ``array_like=True`` must not reorder fields, and any new fields
   must be appended to the end (and have defaults).
3. Don't change the type annotations for existing messages or fields.

For example, suppose we had a `structtype.Struct` type representing a user:

.. code-block:: python

    >>> from structtype import Struct

    >>> class User(Struct):
    ...     """A struct representing a user"""
    ...     name: str
    ...     groups: set[str] = set()
    ...     email: str | None = None

Then suppose we wanted to add a new ``phone`` field to this struct in a way
that wouldn't break clients/servers still using the prior definition. To
accomplish this, we add ``phone`` as an _optional_ field (defaulting to
``None``), at the end of the struct.

.. code-block:: python

    >>> class User2(Struct):
    ...     """An updated version of the User struct, now with a phone number"""
    ...     name: str
    ...     groups: set[str] = set()
    ...     email: str | None = None
    ...     phone : str | None = None

Messages serialized using both the old and new schemas can still be exchanged
without error. If an old message is deserialized using the new schema, the
missing fields all have default values that will be used. Likewise, if a new
message is deserialized with the old schema the unknown new fields will be
efficiently skipped without decoding.

.. code-block:: python

    >>> new_msg = User2("bob", groups={"finance"}, phone="512-867-5309").struct_dump_json()

    >>> User.struct_validate_json(new_msg)  # newer msg with older schema
    User(name='bob', groups={'finance'}, email=None)

    >>> old_msg = User("alice", groups={"admin", "engineering"}).struct_dump_json()

    >>> User2.struct_validate_json(old_msg)  # older msg with newer schema
    User2(name="alice", groups={"admin", "engineering"}, email=None, phone=None)
