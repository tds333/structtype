Why structtype?
===============

To have a validated data structure class which supports fast JSON serialization.

``structtype`` goes above and beyond other Python JSON libraries to help with the
following:

- **Validation**

  Struct validates external input and matches the specified constraints and types.
  ``structtype`` lets you describe your schema via type annotations, and will
  efficiently :ref:`validate <typed-decoding>` messages against this
  schema while decoding.
  It also integrates well with static analysis tools like mypy_ and pyright_,
  helping you avoid whole classes of runtime errors.

- **Serialization**

  It dumps to native types and to JSON.

- **Optimized**

  It consumes less memory than othere libraries or dataclasses.
  Validation and serialization is very fast. (in C)
  It is easy to learn without to much features.


Relationship to msgspec
-----------------------

``structtype`` is a focused fork of the excellent `msgspec`_ library. The core
C encoder/decoder code is from msgspec, with this project extracting and
streamlining the ``Struct`` type and its minimal helpers. The goals of this
fork are:

- Provide only the ``Struct`` type with all serialization methods directly on
  the class (no separate ``Encoder``/ ``Decoder`` objects needed for common
  use).
- Remove msgspec's msgpack and YAML protocols, keeping only JSON.
- Ship as a lightweight, zero-dependency library focused on schema validation
  and JSON serialization.


Relationship to pydantic
------------------------

It does similar things but faster using less memory and with strict simple
validation. Some ideas are the same but parameters and usage differs.
It has less features by intend.


.. _msgspec: https://github.com/jcrist/msgspec

.. _tds333/structtype: https://github.com/tds333/structtype

.. _mypy: https://mypy.readthedocs.io/en/stable/
.. _pyright: https://github.com/microsoft/pyright
