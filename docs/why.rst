Why structtype?
---------------

If you're writing a networked application, you'll need some agreed upon
protocol that your clients and servers can use to communicate. JSON is a decent
choice here (though there are many other options). It's ubiquitous, and Python
has many libraries for parsing it into builtin types (``json``, ``ujson``,
``orjson``, ...).

*However, servers don't just parse JSON, they also need to do something with
it*.

``structtype`` goes above and beyond other Python JSON libraries to help with the
following:

- **Validation**

  If a field is missing from a request or has the wrong type, you probably want
  to raise a nice error message rather than just throwing a 500 error.

  ``structtype`` lets you describe your schema via type annotations, and will
  efficiently :ref:`validate <typed-decoding>` messages against this
  schema while decoding.

  It also integrates well with static analysis tools like mypy_ and pyright_,
  helping you avoid whole classes of runtime errors.

- **Application Logic**

  What your application actually does! While builtin types like dicts are
  fine for writing application logic, they aren't as ergonomic as custom
  classes (no attribute access, poor type checking, ...).

  ``structtype`` supports a :doc:`wide variety of types <supported-types>`,
  letting you decouple the objects your application logic uses from those that
  JSON natively supports.

- **Future Flexibility**

  Application needs change; you'll want to make sure your clients/servers won't
  break if the JSON schema evolves over time.

  To handle this, ``structtype`` supports :doc:`"schema evolution"
  <schema-evolution>`. Messages can be sent between clients with different
  schemas without error, allowing systems to evolve over time.

While there are other tools in this space, ``structtype`` should be an :doc:`order
of magnitude faster <benchmarks>` than other options. We also hope that it's
quick to learn and friendly to use, letting you focus less on serialization and
more on your application code.

Relationship to msgspec
-----------------------

``structtype`` is a focused fork of the excellent `msgspec`_ library. The core
C encoder/decoder code is from msgspec, with this project extracting and
streamlining the ``Struct`` type and its minimal helpers. The goals of this
fork are:

- Provide only the ``Struct`` type with all serialization methods directly on
  the class (no separate ``Encoder``/``Decoder`` objects needed for common
  use).
- Remove msgspec's msgpack and YAML protocols, keeping only JSON.
- Ship as a lightweight, zero-dependency library focused on schema validation
  and JSON serialization.

.. _msgspec: https://github.com/jcrist/msgspec

.. _tds333/structtype: https://github.com/tds333/structtype

.. _mypy: https://mypy.readthedocs.io/en/stable/
.. _pyright: https://github.com/microsoft/pyright
