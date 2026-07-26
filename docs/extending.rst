Extending
=========

To allow encoding/decoding types other than those :doc:`natively supported
<supported-types>`, ``structtype`` provides a few callbacks. These may be
passed to ``struct_dump_json`` / ``struct_validate_json`` as keyword arguments,
or to the ``Encoder`` / ``Decoder`` classes for reuse.

- ``enc_hook``, for transforming custom types into values
  that ``structtype``:doc:`natively supports <supported-types>`.
- ``dec_hook``, for converting natively supported types back into
  a custom type when using :ref:`typed decoding <typed-decoding>`.


These should have the following signatures:

.. code-block:: python

    def enc_hook(obj: Any) -> Any:
        """Given an object that structtype doesn't know how to serialize by
        default, convert it into an object that it does know how to
        serialize"""
        pass

    def dec_hook(type: Type, obj: Any) -> Any:
        """Given a type in a schema, convert ``obj`` (composed of natively
        supported objects) into an object of type ``type``.

        Any `TypeError` or `ValueError` exceptions raised by this method will
        be considered "user facing" and converted into a `ValidationError` with
        additional context. All other exceptions will be raised directly.
        """
        pass

These can be composed together to form complex behaviors as needed.
However, most use cases follow one of these patterns:

- Mapping a custom type to/from natively supported types via ``enc_hook`` and
  ``dec_hook`` callbacks.

Both methods are illustrated below.

Mapping to/from native types
----------------------------

This method uses messages composed only of natively supported types. During
encoding, custom types are mapped to natively supported types, which are then
serialized. This process is then reversed during decoding.

.. code-block::

    custom type -> native types -> encoded message -> native types -> custom type

This means that :ref:`typed decoding <typed-decoding>` is required to roundtrip
a message, since no custom type info is sent as part of the message.

This method works best for types that are similar to a natively supported type
(e.g. a `collections.deque` is similar to a `list`).  This can be accomplished
by defining two callback functions:

- ``enc_hook`` in ``Encoder``, for transforming custom types into values
  that ``structtype`` already knows how to serialize.
- ``dec_hook`` in ``Decoder``, for converting natively supported types back
  into a custom type when using :ref:`typed decoding <typed-decoding>`.

Here we define ``enc_hook`` and ``dec_hook`` callbacks to convert `complex`
objects to/from objects, which are then natively handled by ``structtype``.

.. code-block:: python

    from structtype import Struct
    from typing import Any, Type

    def enc_hook(obj: Any) -> Any:
        if isinstance(obj, complex):
            # convert the complex to a tuple of real, imag
            return (obj.real, obj.imag)
        else:
            # Raise a NotImplementedError for other types
            raise NotImplementedError(f"Objects of type {type(obj)} are not supported")


    def dec_hook(type: Type, obj: Any) -> Any:
        # `type` here is the value of the custom type annotation being decoded.
        if type is complex:
            # Convert ``obj`` (which should be a ``tuple``) to a complex
            real, imag = obj
            return complex(real, imag)
        else:
            # Raise a NotImplementedError for other types
            raise NotImplementedError(f"Objects of type {type} are not supported")


    # Define a message that contains a complex type
    class MyMessage(Struct):
        field_1: str
        field_2: complex

    # An example message
    msg = MyMessage("some string", complex(1, 2))

    # Encode and decode the message using struct methods with custom hooks
    buf = msg.struct_dump_json(enc_hook=enc_hook)
    msg2 = MyMessage.struct_validate_json(buf, dec_hook=dec_hook)
    assert msg == msg2  # True
