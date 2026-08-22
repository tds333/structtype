import datetime
import decimal
import enum
import sys
import typing
import uuid
from base64 import b64encode
from collections import namedtuple
from dataclasses import dataclass
from typing import (
    Annotated,
    Any,
    Dict,
    FrozenSet,
    Generic,
    List,
    Literal,
    NamedTuple,
    NewType,
    Optional,
    Set,
    Tuple,
    TypedDict,
    TypeVar,
    Union,
)

import pytest

import structtype
from structtype import (
    BytesValidator,
    CollectionValidator,
    Field,
    NumericValidator,
    StrValidator,
    TimezoneValidator,
)
from structtype._json_schema import json_schema as make_schema, json_schema_components, json_schema_dump

from .utils import py315_or_later_only, temp_module

if sys.version_info >= (3, 15):
    # This is needed for `ruff` to recognize `frozendict` name
    # and to not raise `F821`:
    from builtins import frozendict

T = TypeVar("T")


def test_any():
    assert make_schema(Any) == {}


def test_raw():
    assert make_schema(structtype.Raw) == {}


def test_msgpack_ext():
    pass


def test_custom():
    with pytest.raises(TypeError, match="Generating JSON schema for custom types"):
        assert make_schema(complex)

    schema = {"type": "string", "format": "complex"}

    assert make_schema(Annotated[complex, Field(json_schema_extra=schema)]) == schema


def test_custom_schema_hook():
    schema = {"type": "string", "format": "complex"}

    def schema_hook(cls):
        if cls is complex:
            return schema
        raise NotImplementedError

    assert make_schema(complex, schema_hook=schema_hook) == schema
    assert make_schema(
        Annotated[complex, Field(json_schema_extra={"title": "A complex field"})],
        schema_hook=schema_hook,
    ) == {**schema, "title": "A complex field"}

    with pytest.raises(TypeError, match="Generating JSON schema for custom types"):
        make_schema(slice, schema_hook=schema_hook)


def test_none():
    assert make_schema(None) == {"type": "null"}


def test_bool():
    assert make_schema(bool) == {"type": "boolean"}


def test_int():
    assert make_schema(int) == {"type": "integer"}


def test_float():
    assert make_schema(float) == {"type": "number"}


def test_string():
    assert make_schema(str) == {"type": "string"}


@pytest.mark.parametrize("typ", [bytes, bytearray, memoryview])
def test_binary(typ):
    assert make_schema(typ) == {
        "type": "string",
        "contentEncoding": "base64",
    }


@pytest.mark.parametrize(
    "annot, extra",
    [
        (None, {}),
        (TimezoneValidator(tz=True), {"format": "date-time"}),
        (TimezoneValidator(tz=False), {}),
    ],
)
def test_datetime(annot, extra):
    typ = datetime.datetime
    if annot is not None:
        typ = Annotated[typ, annot]
    assert make_schema(typ) == {"type": "string", **extra}


@pytest.mark.parametrize(
    "annot, extra",
    [
        (None, {}),
        (TimezoneValidator(tz=True), {"format": "time"}),
        (TimezoneValidator(tz=False), {"format": "partial-time"}),
    ],
)
def test_time(annot, extra):
    typ = datetime.time
    if annot is not None:
        typ = Annotated[typ, annot]
    assert make_schema(typ) == {"type": "string", **extra}


def test_date():
    assert make_schema(datetime.date) == {
        "type": "string",
        "format": "date",
    }


def test_timedelta():
    assert make_schema(datetime.timedelta) == {
        "type": "string",
        "format": "duration",
    }


def test_uuid():
    assert make_schema(uuid.UUID) == {
        "type": "string",
        "format": "uuid",
    }


def test_decimal():
    assert make_schema(decimal.Decimal) == {
        "type": "string",
        "format": "decimal",
    }


def test_newtype():
    UserId = NewType("UserId", str)
    assert make_schema(UserId) == {"type": "string"}
    assert make_schema(Annotated[UserId, StrValidator(max_length=10)]) == {
        "type": "string",
        "maxLength": 10,
    }


@pytest.mark.parametrize("typ", [list, tuple, List, Tuple])
def test_sequence_any(typ):
    assert make_schema(typ) == {"type": "array"}


@pytest.mark.parametrize("cls", [list, tuple, List, Tuple])
def test_sequence_typed(cls):
    args = (int, ...) if cls in (tuple, Tuple) else int
    typ = cls[args]
    assert make_schema(typ) == {"type": "array", "items": {"type": "integer"}}


@pytest.mark.parametrize("typ", [set, frozenset, Set, FrozenSet])
def test_set_any(typ):
    assert make_schema(typ) == {"type": "array", "uniqueItems": True}


@pytest.mark.parametrize("cls", [set, frozenset, Set, FrozenSet])
def test_set_typed(cls):
    typ = cls[int]
    assert make_schema(typ) == {
        "type": "array",
        "uniqueItems": True,
        "items": {"type": "integer"},
    }


@pytest.mark.parametrize("cls", [tuple, Tuple])
def test_tuple(cls):
    typ = cls[int, float, str]
    assert make_schema(typ) == {
        "type": "array",
        "minItems": 3,
        "maxItems": 3,
        "items": False,
        "prefixItems": [
            {"type": "integer"},
            {"type": "number"},
            {"type": "string"},
        ],
    }


@pytest.mark.parametrize("cls", [tuple, Tuple])
def test_empty_tuple(cls):
    typ = cls[()]
    assert make_schema(typ) == {
        "type": "array",
        "minItems": 0,
        "maxItems": 0,
    }


@pytest.mark.parametrize("typ", [dict, Dict])
def test_dict_any(typ):
    assert make_schema(typ) == {"type": "object"}


@pytest.mark.parametrize("cls", [dict, Dict])
def test_dict_typed(cls):
    typ = cls[str, int]
    assert make_schema(typ) == {
        "type": "object",
        "additionalProperties": {"type": "integer"},
    }


@py315_or_later_only
def test_frozendict_any():
    assert make_schema(frozendict) == {"type": "object"}


@py315_or_later_only
def test_frozendict_typed():
    typ = frozendict[str, bool]
    assert make_schema(typ) == {
        "type": "object",
        "additionalProperties": {"type": "boolean"},
    }


def test_abstract_sequence():
    # Only testing one here, the main tests are in `test_inspect`
    typ = typing.Sequence[int]
    assert make_schema(typ) == {"type": "array", "items": {"type": "integer"}}


def test_abstract_mapping():
    # Only testing one here, the main tests are in `test_inspect`
    typ = typing.MutableMapping[str, int]
    assert make_schema(typ) == {
        "type": "object",
        "additionalProperties": {"type": "integer"},
    }


def test_int_enum():
    class Example(enum.IntEnum):
        C = 1
        B = 3
        A = 2

    assert make_schema(Example) == {
        "$ref": "#/$defs/Example",
        "$defs": {"Example": {"title": "Example", "enum": [1, 2, 3]}},
    }


def test_enum():
    class Example(enum.Enum):
        """A docstring"""

        C = "x"
        B = "z"
        A = "y"

    assert make_schema(Example) == {
        "$ref": "#/$defs/Example",
        "$defs": {
            "Example": {
                "title": "Example",
                "description": "A docstring",
                "enum": ["x", "y", "z"],
            }
        },
    }


def test_int_literal():
    assert make_schema(Literal[3, 1, 2]) == {"enum": [1, 2, 3]}


def test_str_literal():
    assert make_schema(Literal["c", "a", "b"]) == {"enum": ["a", "b", "c"]}


def test_mixed_literal():
    # A Literal may mix value types; building its schema must not crash trying
    # to sort values of incomparable types (gh#1018).
    assert make_schema(Literal[1, None]) == {"enum": [None, 1]}
    assert make_schema(Literal[True, "yes"]) == {"enum": [True, "yes"]}


def test_struct_object():
    class Point(structtype.Struct, forbid_unknown_fields=True):
        x: int
        y: int

    class Polygon(structtype.Struct):
        """An example docstring"""

        vertices: list[Point]
        name: str | None = None
        metadata: dict[str, str] = {}

    assert make_schema(Polygon) == {
        "$ref": "#/$defs/Polygon",
        "$defs": {
            "Polygon": {
                "title": "Polygon",
                "description": "An example docstring",
                "type": "object",
                "properties": {
                    "vertices": {
                        "type": "array",
                        "items": {"$ref": "#/$defs/Point"},
                    },
                    "name": {
                        "anyOf": [{"type": "string"}, {"type": "null"}],
                        "default": None,
                    },
                    "metadata": {
                        "type": "object",
                        "additionalProperties": {"type": "string"},
                        "default": {},
                    },
                },
                "required": ["vertices"],
            },
            "Point": {
                "title": "Point",
                "type": "object",
                "properties": {
                    "x": {"type": "integer"},
                    "y": {"type": "integer"},
                },
                "required": ["x", "y"],
                "additionalProperties": False,
            },
        },
    }


@pytest.mark.parametrize("forbid_unknown_fields", [False, True])
def test_struct_array_like(forbid_unknown_fields):
    class Example(
        structtype.Struct, array_like=True, forbid_unknown_fields=forbid_unknown_fields
    ):
        """An example docstring"""

        a: int
        b: str
        c: list[int] = []
        d: dict[str, int] = {}

    sol = {
        "$ref": "#/$defs/Example",
        "$defs": {
            "Example": {
                "title": "Example",
                "description": "An example docstring",
                "type": "array",
                "prefixItems": [
                    {"type": "integer"},
                    {"type": "string"},
                    {"type": "array", "items": {"type": "integer"}, "default": []},
                    {
                        "type": "object",
                        "additionalProperties": {"type": "integer"},
                        "default": {},
                    },
                ],
                "minItems": 2,
            }
        },
    }
    if forbid_unknown_fields:
        sol["$defs"]["Example"]["maxItems"] = 4
    assert make_schema(Example) == sol


def test_struct_no_fields():
    class Example(structtype.Struct):
        pass

    assert make_schema(Example) == {
        "$ref": "#/$defs/Example",
        "$defs": {
            "Example": {
                "title": "Example",
                "type": "object",
                "properties": {},
                "required": [],
            }
        },
    }


def test_struct_object_tagged():
    class Point(structtype.Struct, tag=True):
        x: int
        y: int

    assert make_schema(Point) == {
        "$ref": "#/$defs/Point",
        "$defs": {
            "Point": {
                "title": "Point",
                "type": "object",
                "properties": {
                    "type": {"enum": ["Point"]},
                    "x": {"type": "integer"},
                    "y": {"type": "integer"},
                },
                "required": ["type", "x", "y"],
            }
        },
    }


def test_struct_array_tagged():
    class Point(structtype.Struct, tag=True, array_like=True):
        x: int
        y: int

    assert make_schema(Point) == {
        "$ref": "#/$defs/Point",
        "$defs": {
            "Point": {
                "title": "Point",
                "type": "array",
                "prefixItems": [
                    {"enum": ["Point"]},
                    {"type": "integer"},
                    {"type": "integer"},
                ],
                "minItems": 3,
            }
        },
    }


def test_struct_keyword_only():
    class Base(structtype.Struct, kw_only=True):
        x: int = 1
        y: int
        z: int = 2

    class Test(Base):
        a: int
        b: int = 0

    assert make_schema(Test) == {
        "$ref": "#/$defs/Test",
        "$defs": {
            "Test": {
                "title": "Test",
                "type": "object",
                "properties": {
                    "a": {"type": "integer"},
                    "b": {"type": "integer", "default": 0},
                    "x": {"type": "integer", "default": 1},
                    "y": {"type": "integer"},
                    "z": {"type": "integer", "default": 2},
                },
                "required": ["a", "y"],
            }
        },
    }


def test_struct_array_keyword_only():
    class Base(structtype.Struct, kw_only=True, array_like=True):
        x: int = 1
        y: int
        z: int = 2

    class Test(Base):
        a: int
        b: int = 0

    assert make_schema(Test) == {
        "$ref": "#/$defs/Test",
        "$defs": {
            "Test": {
                "title": "Test",
                "type": "array",
                "prefixItems": [
                    {"type": "integer"},
                    {"type": "integer", "default": 0},
                    {"type": "integer", "default": 1},
                    {"type": "integer"},
                    {"type": "integer", "default": 2},
                ],
                "minItems": 4,
            }
        },
    }


def test_typing_namedtuple():
    class Example(NamedTuple):
        """An example docstring"""

        a: str
        b: bool
        c: int = 0

    assert make_schema(Example) == {
        "$ref": "#/$defs/Example",
        "$defs": {
            "Example": {
                "title": "Example",
                "description": "An example docstring",
                "type": "array",
                "prefixItems": [
                    {"type": "string"},
                    {"type": "boolean"},
                    {"type": "integer", "default": 0},
                ],
                "minItems": 2,
                "maxItems": 3,
            }
        },
    }


def test_collections_namedtuple():
    Example = namedtuple("Example", ["a", "b", "c"], defaults=(0,))

    assert make_schema(Example) == {
        "$ref": "#/$defs/Example",
        "$defs": {
            "Example": {
                "title": "Example",
                "type": "array",
                "prefixItems": [{}, {}, {"default": 0}],
                "minItems": 2,
                "maxItems": 3,
            }
        },
    }


def test_generic_namedtuple():
    NamedTuple = pytest.importorskip("typing_extensions").NamedTuple

    class Ex(NamedTuple, Generic[T]):
        """An example docstring"""

        x: T
        y: list[T]

    assert make_schema(Ex) == {
        "$ref": "#/$defs/Ex",
        "$defs": {
            "Ex": {
                "title": "Ex",
                "description": "An example docstring",
                "type": "array",
                "prefixItems": [{}, {"type": "array"}],
                "minItems": 2,
                "maxItems": 2,
            },
        },
    }

    assert make_schema(Ex[int]) == {
        "$ref": "#/$defs/Ex_int_",
        "$defs": {
            "Ex_int_": {
                "title": "Ex[int]",
                "description": "An example docstring",
                "type": "array",
                "prefixItems": [
                    {"type": "integer"},
                    {"type": "array", "items": {"type": "integer"}},
                ],
                "minItems": 2,
                "maxItems": 2,
            },
        },
    }


@pytest.mark.parametrize("use_typing_extensions", [False, True])
def test_typeddict(use_typing_extensions):
    if use_typing_extensions:
        tex = pytest.importorskip("typing_extensions")
        cls = tex.TypedDict
    else:
        cls = TypedDict

    class Example(cls):
        """An example docstring"""

        a: str
        b: bool
        c: int

    assert make_schema(Example) == {
        "$ref": "#/$defs/Example",
        "$defs": {
            "Example": {
                "title": "Example",
                "description": "An example docstring",
                "type": "object",
                "properties": {
                    "a": {"type": "string"},
                    "b": {"type": "boolean"},
                    "c": {"type": "integer"},
                },
                "required": ["a", "b", "c"],
            }
        },
    }


@pytest.mark.parametrize("use_typing_extensions", [False, True])
def test_typeddict_optional(use_typing_extensions):
    if use_typing_extensions:
        tex = pytest.importorskip("typing_extensions")
        cls = tex.TypedDict
    else:
        cls = TypedDict

    class Base(cls):
        a: str
        b: bool

    class Example(Base, total=False):
        """An example docstring"""

        c: int

    assert make_schema(Example) == {
        "$ref": "#/$defs/Example",
        "$defs": {
            "Example": {
                "title": "Example",
                "description": "An example docstring",
                "type": "object",
                "properties": {
                    "a": {"type": "string"},
                    "b": {"type": "boolean"},
                    "c": {"type": "integer"},
                },
                "required": ["a", "b"],
            }
        },
    }


def test_generic_typeddict():
    TypedDict = pytest.importorskip("typing_extensions").TypedDict

    class Ex(TypedDict, Generic[T]):
        """An example docstring"""

        x: T
        y: list[T]

    assert make_schema(Ex) == {
        "$ref": "#/$defs/Ex",
        "$defs": {
            "Ex": {
                "title": "Ex",
                "description": "An example docstring",
                "type": "object",
                "properties": {
                    "x": {},
                    "y": {"type": "array"},
                },
                "required": ["x", "y"],
            },
        },
    }

    assert make_schema(Ex[int]) == {
        "$ref": "#/$defs/Ex_int_",
        "$defs": {
            "Ex_int_": {
                "title": "Ex[int]",
                "description": "An example docstring",
                "type": "object",
                "properties": {
                    "x": {"type": "integer"},
                    "y": {"type": "array", "items": {"type": "integer"}},
                },
                "required": ["x", "y"],
            },
        },
    }


@pytest.mark.parametrize("module", ["dataclasses", "attrs"])
def test_dataclass_or_attrs(module):
    m = pytest.importorskip(module)
    if module == "attrs":
        decorator = m.define
        factory_default = m.field(factory=dict)
    else:
        decorator = m.dataclass
        factory_default = m.field(default_factory=dict)

    @decorator
    class Point:
        x: int
        y: int

    @decorator
    class Polygon:
        """An example docstring"""

        vertices: list[Point]
        name: str | None = None
        metadata: dict[str, str] = factory_default

    assert make_schema(Polygon) == {
        "$ref": "#/$defs/Polygon",
        "$defs": {
            "Polygon": {
                "title": "Polygon",
                "description": "An example docstring",
                "type": "object",
                "properties": {
                    "vertices": {
                        "type": "array",
                        "items": {"$ref": "#/$defs/Point"},
                    },
                    "name": {
                        "anyOf": [{"type": "string"}, {"type": "null"}],
                        "default": None,
                    },
                    "metadata": {
                        "type": "object",
                        "additionalProperties": {"type": "string"},
                    },
                },
                "required": ["vertices"],
            },
            "Point": {
                "title": "Point",
                "type": "object",
                "properties": {
                    "x": {"type": "integer"},
                    "y": {"type": "integer"},
                },
                "required": ["x", "y"],
            },
        },
    }


@pytest.mark.parametrize("module", ["dataclasses", "attrs"])
def test_generic_dataclass_or_attrs(module):
    m = pytest.importorskip(module)
    decorator = m.define if module == "attrs" else m.dataclass

    @decorator
    class Ex(Generic[T]):
        """An example docstring"""

        x: T
        y: list[T]

    assert make_schema(Ex) == {
        "$ref": "#/$defs/Ex",
        "$defs": {
            "Ex": {
                "title": "Ex",
                "description": "An example docstring",
                "type": "object",
                "properties": {
                    "x": {},
                    "y": {"type": "array"},
                },
                "required": ["x", "y"],
            },
        },
    }

    assert make_schema(Ex[int]) == {
        "$ref": "#/$defs/Ex_int_",
        "$defs": {
            "Ex_int_": {
                "title": "Ex[int]",
                "description": "An example docstring",
                "type": "object",
                "properties": {
                    "x": {"type": "integer"},
                    "y": {"type": "array", "items": {"type": "integer"}},
                },
                "required": ["x", "y"],
            },
        },
    }


def test_optional_struct_null_last():
    class Example(structtype.Struct):
        x: int

    assert make_schema(Optional[Example]) == {
        "anyOf": [{"$ref": "#/$defs/Example"}, {"type": "null"}],
        "$defs": {
            "Example": {
                "title": "Example",
                "type": "object",
                "properties": {"x": {"type": "integer"}},
                "required": ["x"],
            }
        },
    }


def test_optional_union_null_last():
    assert make_schema(Union[int, None, str]) == {
        "anyOf": [{"type": "integer"}, {"type": "string"}, {"type": "null"}],
    }


@pytest.mark.parametrize("use_union_operator", [False, True])
def test_union(use_union_operator):
    class Example(structtype.Struct):
        x: int
        y: int

    if use_union_operator:
        typ = int | str | Example
    else:
        typ = Union[int, str, Example]

    assert make_schema(typ) == {
        "anyOf": [
            {"type": "integer"},
            {"type": "string"},
            {"$ref": "#/$defs/Example"},
        ],
        "$defs": {
            "Example": {
                "title": "Example",
                "type": "object",
                "properties": {"x": {"type": "integer"}, "y": {"type": "integer"}},
                "required": ["x", "y"],
            }
        },
    }


def test_struct_tagged_union():
    class Point(structtype.Struct, tag=True):
        x: int
        y: int

    class Point3D(Point):
        z: int

    assert make_schema(Point | Point3D) == {
        "anyOf": [{"$ref": "#/$defs/Point"}, {"$ref": "#/$defs/Point3D"}],
        "discriminator": {
            "mapping": {"Point": "#/$defs/Point", "Point3D": "#/$defs/Point3D"},
            "propertyName": "type",
        },
        "$defs": {
            "Point": {
                "properties": {
                    "type": {"enum": ["Point"]},
                    "x": {"type": "integer"},
                    "y": {"type": "integer"},
                },
                "required": ["type", "x", "y"],
                "title": "Point",
                "type": "object",
            },
            "Point3D": {
                "properties": {
                    "type": {"enum": ["Point3D"]},
                    "x": {"type": "integer"},
                    "y": {"type": "integer"},
                    "z": {"type": "integer"},
                },
                "required": ["type", "x", "y", "z"],
                "title": "Point3D",
                "type": "object",
            },
        },
    }
    assert make_schema(Point | Point3D) == make_schema(Union[Point, Point3D])


def test_struct_tagged_union_with_none():
    class Point(structtype.Struct, tag=True):
        x: int
        y: int

    class Point3D(Point):
        z: int

    schema = make_schema(Union[Point, Point3D, None])
    assert schema["anyOf"] == [
        {
            "anyOf": [{"$ref": "#/$defs/Point"}, {"$ref": "#/$defs/Point3D"}],
            "discriminator": {
                "mapping": {"Point": "#/$defs/Point", "Point3D": "#/$defs/Point3D"},
                "propertyName": "type",
            },
        },
        {"type": "null"},
    ]
    assert "discriminator" not in schema


def test_struct_tagged_union_mixed_types():
    class Point(structtype.Struct, tag=True):
        x: int
        y: int

    class Point3D(Point):
        z: int

    assert make_schema(Point | Point3D | int | float) == {
        "anyOf": [
            {"type": "integer"},
            {"type": "number"},
            {
                "anyOf": [{"$ref": "#/$defs/Point"}, {"$ref": "#/$defs/Point3D"}],
                "discriminator": {
                    "mapping": {"Point": "#/$defs/Point", "Point3D": "#/$defs/Point3D"},
                    "propertyName": "type",
                },
            },
        ],
        "$defs": {
            "Point": {
                "properties": {
                    "type": {"enum": ["Point"]},
                    "x": {"type": "integer"},
                    "y": {"type": "integer"},
                },
                "required": ["type", "x", "y"],
                "title": "Point",
                "type": "object",
            },
            "Point3D": {
                "properties": {
                    "type": {"enum": ["Point3D"]},
                    "x": {"type": "integer"},
                    "y": {"type": "integer"},
                    "z": {"type": "integer"},
                },
                "required": ["type", "x", "y", "z"],
                "title": "Point3D",
                "type": "object",
            },
        },
    }


def test_struct_tagged_union_with_none_and_other():
    class Point(structtype.Struct, tag=True):
        x: int
        y: int

    class Point3D(Point):
        z: int

    schema = make_schema(Union[Point, Point3D, int, None])
    assert schema["anyOf"] == [
        {"type": "integer"},
        {
            "anyOf": [{"$ref": "#/$defs/Point"}, {"$ref": "#/$defs/Point3D"}],
            "discriminator": {
                "mapping": {"Point": "#/$defs/Point", "Point3D": "#/$defs/Point3D"},
                "propertyName": "type",
            },
        },
        {"type": "null"},
    ]
    assert "discriminator" not in schema


def test_optional_union_null_member_metadata_preserved():
    NullWithMeta = Annotated[None, structtype.Field(description="explicitly unset")]

    assert make_schema(Union[int, NullWithMeta]) == {
        "anyOf": [
            {"type": "integer"},
            {"type": "null", "description": "explicitly unset"},
        ],
    }


def test_struct_array_union():
    class Point(structtype.Struct, array_like=True, tag=True):
        x: int
        y: int

    class Point3D(Point):
        z: int

    assert make_schema(Point | Point3D) == {
        "anyOf": [{"$ref": "#/$defs/Point"}, {"$ref": "#/$defs/Point3D"}],
        "$defs": {
            "Point": {
                "minItems": 3,
                "prefixItems": [
                    {"enum": ["Point"]},
                    {"type": "integer"},
                    {"type": "integer"},
                ],
                "title": "Point",
                "type": "array",
            },
            "Point3D": {
                "minItems": 4,
                "prefixItems": [
                    {"enum": ["Point3D"]},
                    {"type": "integer"},
                    {"type": "integer"},
                    {"type": "integer"},
                ],
                "title": "Point3D",
                "type": "array",
            },
        },
    }


def test_struct_unset_fields():
    class Ex(structtype.Struct):
        x: int | structtype.UnsetType = structtype.UNSET

    assert make_schema(Ex) == {
        "$ref": "#/$defs/Ex",
        "$defs": {
            "Ex": {
                "properties": {"x": {"type": "integer"}},
                "required": [],
                "title": "Ex",
                "type": "object",
            }
        },
    }


def test_generic_struct():
    class Ex(structtype.Struct, Generic[T]):
        """An example docstring"""

        x: T
        y: list[T]

    assert make_schema(Ex) == {
        "$ref": "#/$defs/Ex",
        "$defs": {
            "Ex": {
                "title": "Ex",
                "description": "An example docstring",
                "type": "object",
                "properties": {
                    "x": {},
                    "y": {"type": "array"},
                },
                "required": ["x", "y"],
            },
        },
    }

    assert make_schema(Ex[int]) == {
        "$ref": "#/$defs/Ex_int_",
        "$defs": {
            "Ex_int_": {
                "title": "Ex[int]",
                "description": "An example docstring",
                "type": "object",
                "properties": {
                    "x": {"type": "integer"},
                    "y": {"type": "array", "items": {"type": "integer"}},
                },
                "required": ["x", "y"],
            },
        },
    }


def test_generic_struct_tagged_union():
    class Point(structtype.Struct, Generic[T], tag=True):
        x: T
        y: T

    class Point3D(Point[T]):
        z: T

    sol = {
        "anyOf": [{"$ref": "#/$defs/Point_int_"}, {"$ref": "#/$defs/Point3D_int_"}],
        "discriminator": {
            "mapping": {
                "Point": "#/$defs/Point_int_",
                "Point3D": "#/$defs/Point3D_int_",
            },
            "propertyName": "type",
        },
        "$defs": {
            "Point_int_": {
                "properties": {
                    "type": {"enum": ["Point"]},
                    "x": {"type": "integer"},
                    "y": {"type": "integer"},
                },
                "required": ["type", "x", "y"],
                "title": "Point[int]",
                "type": "object",
            },
            "Point3D_int_": {
                "properties": {
                    "type": {"enum": ["Point3D"]},
                    "x": {"type": "integer"},
                    "y": {"type": "integer"},
                    "z": {"type": "integer"},
                },
                "required": ["type", "x", "y", "z"],
                "title": "Point3D[int]",
                "type": "object",
            },
        },
    }
    res = make_schema(Point[int] | Point3D[int])
    assert res == sol


@pytest.mark.parametrize(
    "field, constraint",
    [
        ("ge", "minimum"),
        ("gt", "exclusiveMinimum"),
        ("le", "maximum"),
        ("lt", "exclusiveMaximum"),
        ("multiple_of", "multipleOf"),
    ],
)
def test_numeric_metadata(field, constraint):
    typ = Annotated[int, NumericValidator(**{field: 2})]
    assert make_schema(typ) == {"type": "integer", constraint: 2}


@pytest.mark.parametrize(
    "field, val, constraint",
    [
        ("pattern", "[a-z]*", "pattern"),
        ("min_length", 0, "minLength"),
        ("max_length", 3, "maxLength"),
    ],
)
def test_string_metadata(field, val, constraint):
    typ = Annotated[str, StrValidator(**{field: val})]
    assert make_schema(typ) == {"type": "string", constraint: val}


@pytest.mark.parametrize(
    "field, val, constraint",
    [
        ("pattern", "[a-z]*", "pattern"),
        ("min_length", 0, "minLength"),
        ("max_length", 3, "maxLength"),
    ],
)
def test_dict_key_metadata(field, val, constraint):
    typ = Annotated[str, StrValidator(**{field: val})]
    assert make_schema(dict[typ, int]) == {
        "type": "object",
        "additionalProperties": {"type": "integer"},
        "propertyNames": {constraint: val},
    }


@pytest.mark.parametrize("typ", [bytes, bytearray, memoryview])
@pytest.mark.parametrize(
    "field, n, constraint",
    [("min_length", 2, "minLength"), ("max_length", 7, "maxLength")],
)
def test_binary_metadata(typ, field, n, constraint):
    n2 = len(b64encode(b"x" * n))
    typ = Annotated[typ, BytesValidator(**{field: n})]
    assert make_schema(typ) == {
        "type": "string",
        constraint: n2,
        "contentEncoding": "base64",
    }


@pytest.mark.parametrize("typ", [list, tuple])
@pytest.mark.parametrize(
    "field, constraint",
    [("min_length", "minItems"), ("max_length", "maxItems")],
)
def test_array_metadata(typ, field, constraint):
    typ = Annotated[typ, CollectionValidator(**{field: 2})]
    assert make_schema(typ) == {"type": "array", constraint: 2}


@pytest.mark.parametrize("typ", [set, frozenset])
@pytest.mark.parametrize(
    "field, constraint",
    [("min_length", "minItems"), ("max_length", "maxItems")],
)
def test_set_metadata(typ, field, constraint):
    typ = Annotated[typ[int], CollectionValidator(**{field: 2})]
    assert make_schema(typ) == {
        "type": "array",
        "uniqueItems": True,
        "items": {"type": "integer"},
        constraint: 2,
    }


@pytest.mark.parametrize(
    "field, constraint",
    [("min_length", "minProperties"), ("max_length", "maxProperties")],
)
def test_object_metadata(field, constraint):
    typ = Annotated[dict, CollectionValidator(**{field: 2})]
    assert make_schema(typ) == {"type": "object", constraint: 2}


def test_generic_metadata():
    typ = Annotated[
        int,
        Field(
            title="the title",
            description="the description",
            examples=[1, 2, 3],
            json_schema_extra={"title": "an override", "default": 1},
        ),
    ]
    assert make_schema(typ) == {
        "type": "integer",
        "title": "an override",
        "description": "the description",
        "examples": [1, 2, 3],
        "default": 1,
    }


def test_deprecated_metadata():
    typ = Annotated[str, Field(deprecated=True)]
    assert make_schema(typ) == {"type": "string", "deprecated": True}

    typ = Annotated[str, Field(deprecated=False)]
    assert make_schema(typ) == {"type": "string", "deprecated": False}

    typ = Annotated[str, Field()]
    assert make_schema(typ) == {"type": "string"}


def test_component_names_collide():
    s1 = """
    import structtype

    class Ex(structtype.Struct):
        x: int
        y: int
    """

    s2 = """
    import structtype

    class Ex(structtype.Struct):
        a: str
        b: str
    """

    with temp_module(s1) as m1, temp_module(s2) as m2:
        (r1, r2), components = json_schema_components([m1.Ex, m2.Ex])

    assert r1 == {"$ref": f"#/$defs/{m1.__name__}.Ex"}
    assert r2 == {"$ref": f"#/$defs/{m2.__name__}.Ex"}
    assert components == {
        f"{m1.__name__}.Ex": {
            "properties": {"x": {"type": "integer"}, "y": {"type": "integer"}},
            "required": ["x", "y"],
            "title": "Ex",
            "type": "object",
        },
        f"{m2.__name__}.Ex": {
            "properties": {"a": {"type": "string"}, "b": {"type": "string"}},
            "required": ["a", "b"],
            "title": "Ex",
            "type": "object",
        },
    }


def test_schema_components_collects_subtypes():
    class ExEnum(enum.Enum):
        A = 1

    class ExStruct(structtype.Struct):
        b: set[frozenset[ExEnum]] | int

    class ExDict(TypedDict):
        c: tuple[ExStruct, ...]

    class ExTuple(NamedTuple):
        d: list[ExDict]

    @dataclass
    class ExDataclass:
        e: list[ExTuple]

    (s,), components = json_schema_components([dict[str, ExDataclass]])

    r1 = {"$ref": "#/$defs/ExEnum"}
    r2 = {"$ref": "#/$defs/ExStruct"}
    r3 = {"$ref": "#/$defs/ExDict"}
    r4 = {"$ref": "#/$defs/ExTuple"}
    r5 = {"$ref": "#/$defs/ExDataclass"}

    assert s == {"type": "object", "additionalProperties": r5}
    assert components == {
        "ExEnum": {"enum": [1], "title": "ExEnum"},
        "ExStruct": {
            "type": "object",
            "title": "ExStruct",
            "properties": {
                "b": {
                    "anyOf": [
                        {
                            "items": {
                                "items": r1,
                                "type": "array",
                                "uniqueItems": True,
                            },
                            "type": "array",
                            "uniqueItems": True,
                        },
                        {"type": "integer"},
                    ]
                }
            },
            "required": ["b"],
        },
        "ExDict": {
            "title": "ExDict",
            "type": "object",
            "properties": {"c": {"items": r2, "type": "array"}},
            "required": ["c"],
        },
        "ExTuple": {
            "title": "ExTuple",
            "type": "array",
            "prefixItems": [{"items": r3, "type": "array"}],
            "maxItems": 1,
            "minItems": 1,
        },
        "ExDataclass": {
            "title": "ExDataclass",
            "type": "object",
            "properties": {"e": {"items": r4, "type": "array"}},
            "required": ["e"],
        },
    }


def test_ref_template():
    class Ex1(structtype.Struct):
        a: int

    class Ex2(structtype.Struct):
        b: Ex1

    (s1, s2), components = json_schema_components(
        [Ex1, Ex2], ref_template="#/definitions/{name}"
    )

    assert s1 == {"$ref": "#/definitions/Ex1"}
    assert s2 == {"$ref": "#/definitions/Ex2"}

    assert components == {
        "Ex1": {
            "title": "Ex1",
            "type": "object",
            "properties": {"a": {"type": "integer"}},
            "required": ["a"],
        },
        "Ex2": {
            "title": "Ex2",
            "type": "object",
            "properties": {"b": s1},
            "required": ["b"],
        },
    }


def test_multiline_docstring():
    class Example(structtype.Struct):
        """
            indented first line

        last line.
        """

        pass

    assert make_schema(Example) == {
        "$ref": "#/$defs/Example",
        "$defs": {
            "Example": {
                "description": "    indented first line\n\nlast line.",
                "title": "Example",
                "type": "object",
                "properties": {},
                "required": [],
            }
        },
    }


# Pydantic v2 schema tests
def test_pydantic_simple():
    pydantic = pytest.importorskip("pydantic")

    class Point(pydantic.BaseModel):
        x: int
        y: int

    assert make_schema(Point) == {
        "$ref": "#/$defs/Point",
        "$defs": {
            "Point": {
                "title": "Point",
                "type": "object",
                "properties": {
                    "x": {"type": "integer"},
                    "y": {"type": "integer"},
                },
                "required": ["x", "y"],
            },
        },
    }


def test_pydantic_with_defaults():
    pydantic = pytest.importorskip("pydantic")

    class User(pydantic.BaseModel):
        name: str
        age: int = 0
        active: bool = True
        tags: list[str] = pydantic.Field(default_factory=list)

    assert make_schema(User) == {
        "$ref": "#/$defs/User",
        "$defs": {
            "User": {
                "title": "User",
                "type": "object",
                "properties": {
                    "name": {"type": "string"},
                    "age": {"type": "integer", "default": 0},
                    "active": {"type": "boolean", "default": True},
                    "tags": {
                        "type": "array",
                        "items": {"type": "string"},
                    },
                },
                "required": ["name"],
            },
        },
    }


def test_pydantic_nested():
    pydantic = pytest.importorskip("pydantic")

    class Point(pydantic.BaseModel):
        x: float
        y: float

    class Polygon(pydantic.BaseModel):
        """An example docstring"""
        vertices: list[Point]
        name: str | None = None

    assert make_schema(Polygon) == {
        "$ref": "#/$defs/Polygon",
        "$defs": {
            "Polygon": {
                "title": "Polygon",
                "description": "An example docstring",
                "type": "object",
                "properties": {
                    "vertices": {
                        "type": "array",
                        "items": {"$ref": "#/$defs/Point"},
                    },
                    "name": {
                        "anyOf": [{"type": "string"}, {"type": "null"}],
                        "default": None,
                    },
                },
                "required": ["vertices"],
            },
            "Point": {
                "title": "Point",
                "type": "object",
                "properties": {
                    "x": {"type": "number"},
                    "y": {"type": "number"},
                },
                "required": ["x", "y"],
            },
        },
    }


def test_pydantic_with_alias():
    pydantic = pytest.importorskip("pydantic")

    class Example(pydantic.BaseModel):
        first_name: str = pydantic.Field(alias="firstName")
        last_name: str = pydantic.Field(serialization_alias="lastName")
        email: str

    assert make_schema(Example) == {
        "$ref": "#/$defs/Example",
        "$defs": {
            "Example": {
                "title": "Example",
                "type": "object",
                "properties": {
                    "firstName": {"type": "string"},
                    "lastName": {"type": "string"},
                    "email": {"type": "string"},
                },
                "required": ["firstName", "lastName", "email"],
            },
        },
    }


# ------------------------------------------------------------------
# Coverage: json_schema_dump body (_json_schema.py:119)
# ------------------------------------------------------------------
def test_json_schema_dump():
    class _DumpTarget(structtype.Struct):
        name: str
        value: int

    s = json_schema_dump(_DumpTarget)
    assert isinstance(s, bytes)
    assert b"_DumpTarget" in s


# ------------------------------------------------------------------
# Coverage: enum auto-docstring suppression (_json_schema.py:240)
# ------------------------------------------------------------------
class _ColorNoDoc(enum.Enum):
    RED = 1
    BLUE = 2


class _ColorWithDoc(enum.Enum):
    "Custom color enum."
    RED = 1
    BLUE = 2


def test_enum_no_doc_no_description():
    s = make_schema(_ColorNoDoc)
    assert "description" not in s


def test_enum_with_doc_has_description():
    s = make_schema(_ColorWithDoc)
    assert s["$defs"]["_ColorWithDoc"].get("description") == "Custom color enum."
