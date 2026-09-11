"""Encode/decode/validation tests for ``frozendict`` (Python 3.15+).

``frozendict`` was previously only exercised through JSON-schema and type-info
tests; these cover the actual serializer/validator code paths.
"""

import sys
from typing import Annotated

import pytest

from structtype import (
    CollectionConstraint,
    Struct,
    StructAdapter,
    ValidationError,
)

from .utils import py315_or_later_only

pytestmark = py315_or_later_only

if sys.version_info >= (3, 15):
    from builtins import frozendict
else:  # pragma: no cover
    frozendict = None


def test_frozendict_roundtrip_json():
    class Msg(Struct):
        fd: frozendict

    src = Msg(frozendict({"a": 1, "b": 2}))
    buf = src.struct_dump_json()
    assert buf == b'{"fd":{"a":1,"b":2}}'

    out = Msg.struct_validate_json(buf)
    assert out == src
    assert isinstance(out.fd, frozendict)


def test_frozendict_empty():
    class Msg(Struct):
        fd: frozendict

    assert Msg(frozendict()).struct_dump_json() == b'{"fd":{}}'
    assert Msg.struct_validate_json(b'{"fd":{}}').fd == frozendict()


def test_frozendict_typed():
    class Msg(Struct):
        fd: frozendict[str, int]

    out = Msg.struct_validate({"fd": {"a": 1}})
    assert out.fd == frozendict({"a": 1})
    assert isinstance(out.fd, frozendict)

    with pytest.raises(ValidationError):
        Msg.struct_validate({"fd": {"a": "not-an-int"}})


def test_frozendict_nested():
    class Msg(Struct):
        fd: frozendict[str, frozendict[str, int]]

    out = Msg.struct_validate({"fd": {"a": {"b": 1}}})
    assert out.fd == frozendict({"a": frozendict({"b": 1})})
    assert Msg.struct_validate_json(out.struct_dump_json()).fd == out.fd


def test_frozendict_struct_dump_builtin():
    class Msg(Struct):
        fd: frozendict

    assert Msg(frozendict({"a": 1})).struct_dump() == {"fd": {"a": 1}}


def test_frozendict_constraint():
    class Msg(Struct):
        fd: Annotated[frozendict, CollectionConstraint(min_length=1, max_length=2)]

    Msg(frozendict({"a": 1})).struct_check_types()
    with pytest.raises(ValidationError):
        Msg.struct_validate({"fd": {}})
    with pytest.raises(ValidationError):
        Msg.struct_validate({"fd": {"a": 1, "b": 2, "c": 3}})


def test_frozendict_adapter():
    ta = StructAdapter(frozendict[str, int])
    assert ta.struct_validate_json(b'{"a":1}') == frozendict({"a": 1})
    assert ta.struct_dump_json(frozendict({"a": 1})) == b'{"a":1}'
    assert isinstance(ta.struct_validate({"a": 1}), frozendict)
