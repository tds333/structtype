import json
import pathlib
import sys
from typing import Annotated

import pytest

import structtype
from structtype import ALL_BUILTIN_TYPES, Serializer, Struct, _inspect as mi
from structtype._json_schema import json_schema as make_schema

PURE_PATH_CLASSES = [
    pathlib.Path,
    pathlib.PurePath,
    pathlib.PurePosixPath,
    pathlib.PureWindowsPath,
]
if sys.platform == "win32":
    PURE_PATH_CLASSES.append(pathlib.WindowsPath)
else:
    PURE_PATH_CLASSES.append(pathlib.PosixPath)


def _struct(annotation):
    return type(
        "P_" + getattr(annotation, "__name__", "Union"),
        (Struct,),
        {"__annotations__": {"v": annotation}},
    )


@pytest.mark.parametrize("annotation", PURE_PATH_CLASSES, ids=lambda c: c.__name__)
def test_roundtrip_json(annotation):
    cls = _struct(annotation)
    value = annotation("a/b")
    obj = cls(value)
    out = cls.struct_validate_json(obj.struct_dump_json())
    assert isinstance(out.v, annotation)
    assert out.v == value


@pytest.mark.parametrize("annotation", PURE_PATH_CLASSES, ids=lambda c: c.__name__)
def test_roundtrip_struct_dump(annotation):
    cls = _struct(annotation)
    value = annotation("a/b")
    dumped = cls(value).struct_dump()
    out = cls.struct_validate(dumped)
    assert isinstance(out.v, annotation)
    assert out.v == value


@pytest.mark.parametrize("annotation", PURE_PATH_CLASSES, ids=lambda c: c.__name__)
def test_struct_adapter(annotation):
    adapter = structtype.StructAdapter(annotation)
    value = annotation("a/b")
    out = adapter.struct_validate_json(adapter.struct_dump_json(value))
    assert isinstance(out, annotation)
    assert out == value


def test_decode_constructs_declared_windows_flavour():
    cls = _struct(pathlib.PureWindowsPath)
    out = cls.struct_validate_json(json.dumps({"v": "a/b"}).encode())
    assert type(out.v) is pathlib.PureWindowsPath
    assert out.v == pathlib.PureWindowsPath("a/b")


def test_decode_constructs_declared_pureposix():
    cls = _struct(pathlib.PurePosixPath)
    out = cls.struct_validate({"v": "/tmp/a"})
    assert type(out.v) is pathlib.PurePosixPath


def test_path_field_rejects_non_path():
    cls = _struct(pathlib.Path)
    cls(pathlib.Path("a")).struct_check_types()
    with pytest.raises(structtype.ValidationError):
        cls(pathlib.PurePosixPath("a")).struct_check_types()
    with pytest.raises(structtype.ValidationError):
        cls.struct_validate({"v": pathlib.PurePosixPath("a")})


def test_purepath_field_accepts_every_flavour():
    cls = _struct(pathlib.PurePath)
    for value in (
        pathlib.Path("a"),
        pathlib.PurePosixPath("a"),
        pathlib.PureWindowsPath("a"),
    ):
        assert cls.struct_validate({"v": value}).v == value


class _FspathOnly(pathlib.PurePosixPath):
    def __str__(self):
        return "STR:" + super().__str__()

    def __fspath__(self):
        return "a/b"


class _BytesFspath(pathlib.PurePosixPath):
    def __fspath__(self):
        return b"a/b"


def test_serialization_uses_fspath_not_str():
    cls = _struct(pathlib.PurePath)
    obj = cls(_FspathOnly("a/b"))
    assert json.loads(obj.struct_dump_json())["v"] == "a/b"
    assert obj.struct_dump() == {"v": "a/b"}


def test_fspath_returning_bytes_errors():
    cls = _struct(pathlib.PurePath)
    obj = cls(_BytesFspath("a/b"))
    with pytest.raises(TypeError):
        obj.struct_dump_json()
    with pytest.raises(TypeError):
        obj.struct_dump()


def test_builtin_types_preserves_pure_path():
    cls = _struct(pathlib.PurePosixPath)
    value = pathlib.PurePosixPath("a/b")
    obj = cls(value)
    assert obj.struct_dump(builtin_types=[pathlib.PurePath])["v"] == value
    assert obj.struct_dump(builtin_types=ALL_BUILTIN_TYPES)["v"] == value


def test_type_info_pure_path_subclasses():
    for annotation in PURE_PATH_CLASSES:
        assert mi.type_info(annotation) == mi.PathType()


def test_schema_pure_path_subclasses():
    for annotation in PURE_PATH_CLASSES:
        assert make_schema(annotation) == {"type": "string", "format": "path"}


def test_serializer_on_pure_path_subclass():
    ann = Annotated[
        pathlib.PureWindowsPath,
        Serializer(dump=str, load=pathlib.PureWindowsPath),
    ]
    cls = type("CodecPureWin", (Struct,), {"__annotations__": {"v": ann}})
    obj = cls(pathlib.PureWindowsPath("a/b"))
    assert cls.struct_validate_json(
        obj.struct_dump_json()
    ).v == pathlib.PureWindowsPath("a/b")


def test_union_of_distinct_path_types_rejected():
    with pytest.raises(TypeError, match="more than one path type"):
        cls = _struct(pathlib.Path | pathlib.PureWindowsPath)
        cls.struct_validate({"v": pathlib.Path("a")})
