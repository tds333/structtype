Extending
=========

To encode and decode types other than those :doc:`natively supported
<supported-types>`, ``structtype`` provides two mechanisms:

- **Custom-type protocol** — the type itself declares ``struct_dump`` /
  ``struct_validate`` methods. Pydantic's ``model_dump`` / ``model_validate``
  are also recognized. This is the primary mechanism: it works for any custom
  type and nests automatically.
- **Per-field codecs** — an ``Annotated[X, Field(dump=..., validate=...)]``
  attaches codecs to a single field. This is the escape hatch for types that
  can't declare methods, such as builtins or third-party types you don't
  control.

Custom type protocol
--------------------

Types are extended by implementing two methods:

- ``struct_dump`` — an instance method converting the object into a value
  composed of :doc:`natively supported <supported-types>` types.
- ``struct_validate`` — a classmethod converting a value composed of natively
  supported types back into an instance of the custom type.

Both are detected by duck-typing, so no base class or registration is
required. During encoding, the object's ``struct_dump`` result is serialized
normally; during decoding, the decoded value is passed to ``struct_validate``.

.. note::

    Since no custom-type information is carried in the message itself,
    decoding requires :ref:`typed decoding <typed-decoding>` — the custom type
    must appear in the schema (e.g. as a field annotation).

Here we define a ``MyType`` that serializes as a pair of floats, similar to
the pair-of-floats representation of a ``complex``:

.. code-block:: python

    from structtype import Struct
    from typing import Any

    class MyType:
        def __init__(self, real: float, imag: float) -> None:
            self.real = real
            self.imag = imag

        def struct_dump(self) -> Any:
            # Return a value composed of natively supported types
            return (self.real, self.imag)

        @classmethod
        def struct_validate(cls, obj: Any) -> "MyType":
            # ``obj`` is composed of natively supported types
            return cls(obj[0], obj[1])

    class Message(Struct):
        value: MyType

    msg = Message(MyType(1.0, 2.0))

    # Encode and decode a message using the protocol
    buf = msg.struct_dump_json()
    msg2 = Message.struct_validate_json(buf)
    assert msg2.value.real == 1.0 and msg2.value.imag == 2.0

The protocol works wherever the type appears — nested inside containers,
inside other structs, or behind optional types:

.. code-block:: python

    class Container(Struct):
        values: list[MyType]
        maybe: MyType | None = None

    c = Container([MyType(1.0, 2.0)])
    assert c.struct_dump_json() == b'{"values":[[1.0,2.0]],"maybe":null}'

    c2 = Container.struct_validate_json(b'{"values":[[3.0,4.0]],"maybe":null}')
    assert c2.values[0].real == 3.0 and c2.values[0].imag == 4.0

Pydantic models
~~~~~~~~~~~~~~~

`pydantic` models are supported out of the box. They use pydantic's native
``model_dump`` / ``model_validate`` methods, which ``structtype`` recognizes
automatically:

.. code-block:: python

    from pydantic import BaseModel

    class User(BaseModel):
        name: str
        age: int = 0

    class Message(Struct):
        user: User

    msg = Message(User(name="Alice", age=30))
    assert msg.struct_dump_json() == b'{"user":{"name":"Alice","age":30}}'

    msg2 = Message.struct_validate_json(b'{"user":{"name":"Bob"}}')
    assert msg2.user.name == "Bob"

Annotation escape hatch
-----------------------

Some types can't implement the protocol. This includes builtins like
`complex` and third-party types you don't control. For these, attach codecs
to the field itself with :class:`Field`:

.. code-block:: python

    from typing import Annotated
    from structtype import Struct, Field

    def dump(c: complex) -> tuple[float, float]:
        # Convert the value into natively supported types
        return (c.real, c.imag)

    def validate(obj) -> complex:
        # Convert natively supported types back into the value
        return complex(obj[0], obj[1])

    class Message(Struct):
        value: Annotated[complex, Field(dump=dump, validate=validate)]

    msg = Message(complex(1.0, 2.0))
    assert msg.struct_dump_json() == b'{"value":[1.0,2.0]}'

    msg2 = Message.struct_validate_json(b'{"value":[1.0,2.0]}')
    assert msg2.value == complex(1.0, 2.0)

``dump`` and ``validate`` may be provided independently:

- A ``dump``-only codec controls encoding; decoding falls back to the
  protocol (if the type implements one), or fails with a ``ValidationError``.
- A ``validate``-only codec controls decoding; encoding falls back to the
  protocol, or fails with a ``TypeError``.

Codecs are per-field. Different fields may use different codecs for the same
type, and a field's codec applies wherever the annotated type appears within
that field — including nested inside lists, dicts, and tuples:

.. code-block:: python

    class Message(Struct):
        a: Annotated[complex, Field(dump=dump_pair)]        # -> [real, imag]
        b: Annotated[complex, Field(dump=dump_real)]        # -> real
        values: list[Annotated[complex, Field(dump=dump_pair)]]

Codecs may only be attached to a *custom* type. ``structtype`` validates
this when the class is created, raising a ``TypeError`` when:

- the type is natively supported — ``Annotated[int, Field(dump=...)]``,
- the type is a union — including optional types such as
  ``Annotated[complex | None, Field(dump=...)]``
  (``Annotated[int | str, Field(dump=...)]``),
- two different ``dump`` codecs apply within a single field, e.g.
  ``tuple[Annotated[complex, Field(dump=a)], Annotated[complex, Field(dump=b)]]``.

Codecs are only supported on :class:`Struct` fields.
:class:`StructAdapter` rejects annotations containing a ``Field`` codec —
use the protocol methods on the type there, or a :class:`Struct`:

.. code-block:: python

    from structtype import StructAdapter

    # Raises TypeError — use a `struct_dump`/`struct_validate` protocol method
    # on the type, or a `Struct` instead.
    StructAdapter(Annotated[complex, Field(dump=dump, validate=validate)])

Recipes
-------

The escape hatch can be defined once as an annotated alias and reused across a
project. These are the common Python stdlib types that ``structtype`` doesn't
:doc:`natively support <supported-types>`, with a ``Field`` codec for each:

.. code-block:: python

    import fractions
    import ipaddress
    import pathlib
    import re
    import types
    from collections import deque
    from typing import Annotated
    from structtype import Field

    Complex = Annotated[complex, Field(
        dump=lambda c: (c.real, c.imag),
        validate=lambda o: complex(o[0], o[1]))]

    Fraction = Annotated[fractions.Fraction, Field(
        dump=lambda f: (f.numerator, f.denominator),
        validate=lambda o: fractions.Fraction(o[0], o[1]))]

    Deque = Annotated[deque, Field(
        dump=lambda d: list(d),
        validate=lambda o: deque(o))]

    Path = Annotated[pathlib.Path, Field(
        dump=lambda p: str(p),
        validate=lambda o: pathlib.Path(o))]

    Pattern = Annotated[re.Pattern, Field(
        dump=lambda p: p.pattern,
        validate=lambda o: re.compile(o))]

    Range = Annotated[range, Field(
        dump=lambda r: (r.start, r.stop, r.step),
        validate=lambda o: range(o[0], o[1], o[2]))]

    SimpleNamespace = Annotated[types.SimpleNamespace, Field(
        dump=lambda n: vars(n),
        validate=lambda o: types.SimpleNamespace(**o))]

    IPv4Address = Annotated[ipaddress.IPv4Address, Field(
        dump=str, validate=ipaddress.IPv4Address)]

These aliases nest (``list[Complex]``, ``dict[str, Fraction]``), and the
``dump`` / ``validate`` callables must map to :doc:`natively supported
<supported-types>` values. Codec aliases are supported on :class:`Struct`
fields only — :class:`StructAdapter` rejects them (see above). For types you
control, prefer the ``struct_dump`` / ``struct_validate`` protocol methods.
Single-argument string-constructible types such as ``IPv4Address`` may also use
``Annotated[T, Field(dump=str, validate=T)]`` codecs.

Pydantic custom types
~~~~~~~~~~~~~~~~~~~~~

pydantic lets you define *custom types* via the ``__get_pydantic_core_schema__``
protocol. ``structtype`` has no runtime dependency on pydantic, so these are
bridged from the user side with a :class:`Field` codec and
``pydantic.TypeAdapter``:

.. code-block:: python

    from typing import Annotated
    from pydantic import TypeAdapter
    from structtype import Field

    class Zip:  # a pydantic custom type whose core schema defines validation
        def __init__(self, code: str):
            self.code = code

    # `validate` always works — it's pydantic's own validation/coercion.
    # Provide a small `dump` when the type has no serializer.
    PostalCode = Annotated[Zip, Field(
        dump=lambda p: p.code,
        validate=TypeAdapter(Zip).validate_python)]

    # ...or, if the custom type defines a serializer in its core schema,
    # both sides can use TypeAdapter directly:
    PostalCode = Annotated[Zip, Field(
        dump=TypeAdapter(Zip).dump_python,
        validate=TypeAdapter(Zip).validate_python)]

.. note::

    Many pydantic custom types define *validation-only* schemas (no
    serializer), and ``TypeAdapter.dump_python`` fails on those — so prefer
    providing a small ``dump`` callable yourself.

A reusable helper:

.. code-block:: python

    def pydantic_type(t, dump=None):
        ta = TypeAdapter(t)
        return Annotated[t, Field(dump=dump or ta.dump_python,
                                  validate=ta.validate_python)]

Like the stdlib recipes above, this applies to :class:`Struct` fields only
(``StructAdapter`` rejects codec'd annotations). pydantic ``BaseModel`` types
themselves are handled automatically via the ``model_dump`` /
``model_validate`` protocol described above.

Migrating from hooks
--------------------

The previous ``enc_hook`` / ``dec_hook`` callbacks were removed. Replace them
with a protocol method or a ``Field`` codec:

- For types you control, implement ``struct_dump`` and ``struct_validate``
  directly on the type.
- For types that can't declare methods, attach a
  ``Field(dump=..., validate=...)`` codec to the field's annotation.
- The hooks were passed as keyword arguments to ``struct_dump`` /
  ``struct_dump_json`` / ``struct_validate`` / ``struct_validate_json``.
  Those arguments no longer exist; passing them raises a ``TypeError``.
