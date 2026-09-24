import subprocess
import sys

import structtype

from .utils import requires_subprocess


def run_isolated_type_error(setup, call, match):
    source = "\n".join(
        [
            "import structtype",
            setup,
            "try:",
            f"    {call}",
            "except TypeError as exc:",
            f"    assert {match!r} in str(exc), str(exc)",
            "else:",
            '    raise AssertionError("expected TypeError")',
            "print('ok')",
        ]
    )
    return subprocess.run(
        [sys.executable, "-c", source],
        capture_output=True,
        text=True,
        check=False,
    )


def external_source(fields, defaults):
    return f"""
class External:
    __struct_fields__ = {fields}
    __struct_defaults__ = {defaults}

    def __init__(self, **kwargs):
        self.__dict__.update(kwargs)
"""


def run_isolated(setup, expression):
    source = "\n".join(
        [
            "import structtype",
            setup,
            f"result = {expression}",
            "print(repr(result))",
        ]
    )
    return subprocess.run(
        [sys.executable, "-c", source],
        capture_output=True,
        text=True,
        check=False,
    )


@requires_subprocess
def test_set_subclass_iterator_does_not_overflow():
    result = run_isolated(
        """
class BadSet(set):
    def __iter__(self):
        for i in range(100000):
            yield i
""",
        "structtype.StructAdapter(BadSet).struct_dump(BadSet([1]))",
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.startswith("[")


@requires_subprocess
def test_external_fields_must_be_iterable():
    for call in (
        "struct_dump(External())",
        "struct_dump_json(External())",
        "struct_validate({})",
        "struct_validate_json(b'{}')",
    ):
        result = run_isolated_type_error(
            external_source("1", "()"),
            f"structtype.StructAdapter(External).{call}",
            "__struct_fields__ must be an iterable of str",
        )
        assert result.returncode == 0, result.stderr


def external_attributes(setup):
    source = "\n".join(
        [
            "import structtype",
            setup,
            "value = structtype.StructAdapter(External).struct_validate({'x': 1})",
            "print(value.x, value.y)",
        ]
    )
    return subprocess.run(
        [sys.executable, "-c", source],
        capture_output=True,
        text=True,
        check=False,
    )


@requires_subprocess
def test_external_metadata_may_be_a_list():
    result = external_attributes(external_source("['x', 'y']", "[0]"))
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "1 0"


@requires_subprocess
def test_external_metadata_may_be_a_generator():
    result = external_attributes(
        external_source(
            "(name for name in ('x', 'y'))",
            "(value for value in (0,))",
        )
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "1 0"


@requires_subprocess
def test_external_field_names_must_be_strings():
    for method in ("struct_dump", "struct_dump_json"):
        result = run_isolated_type_error(
            external_source("(1,)", "()"),
            f"structtype.StructAdapter(External).{method}(External())",
            "__struct_fields__[0] must be a str",
        )
        assert result.returncode == 0, result.stderr


@requires_subprocess
def test_external_defaults_must_be_iterable():
    for method, payload in (
        ("struct_validate", "{}"),
        ("struct_validate_json", "b'{}'"),
    ):
        result = run_isolated_type_error(
            external_source("('x',)", "1"),
            f"structtype.StructAdapter(External).{method}({payload})",
            "__struct_defaults__ must be an iterable",
        )
        assert result.returncode == 0, result.stderr


@requires_subprocess
def test_external_defaults_cannot_exceed_fields():
    for method, payload in (
        ("struct_validate", "{}"),
        ("struct_validate_json", "b'{}'"),
    ):
        result = run_isolated_type_error(
            external_source("('x',)", "(0, 0)"),
            f"structtype.StructAdapter(External).{method}({payload})",
            "__struct_defaults__ has 2 entries but __struct_fields__ has 1",
        )
        assert result.returncode == 0, result.stderr


def test_valid_external_struct_protocol_still_roundtrips():
    class External:
        __struct_fields__ = ("x", "y")
        __struct_defaults__ = (0,)

        def __init__(self, x, y=0):
            self.x = x
            self.y = y

        def __eq__(self, other):
            return self.x == other.x and self.y == other.y

    adapter = structtype.StructAdapter(External)
    value = adapter.struct_validate({"x": 1})
    assert value == External(1)
    assert adapter.struct_dump(value) == {"x": 1, "y": 0}
    assert adapter.struct_dump_json(value) == b'{"x":1,"y":0}'
