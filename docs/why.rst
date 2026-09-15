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

  It uses less memory than dataclasses or other libraries, and
  validation/serialization is very fast (implemented in C). The API is small and
  easy to learn.

- **Clean and minimal interface**

  Methods named ``validate`` validate and decode; methods named ``dump`` encode
  and serialize. Nothing more.



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

It solves a similar problem faster and with less memory, using strict, simple
validation. Some ideas overlap, but parameters and usage differ. It has fewer
features by design.


.. _msgspec: https://github.com/jcrist/msgspec

.. _tds333/structtype: https://github.com/tds333/structtype

.. _mypy: https://mypy.readthedocs.io/en/stable/
.. _pyright: https://github.com/microsoft/pyright


Relationship to std lib and std json
------------------------------------

Where it makes sense, parameters match the standard library. Instead of extra
copy/replace methods, it implements the standard protocols (``__copy__``,
``__replace__``) so builtins work.

