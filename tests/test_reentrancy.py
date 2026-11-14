"""Regression tests for the crash/hang bugs found in the 2026-11-13 audit.

See ``docs/code-audit-2026-11-13.md``. These run in subprocesses because the
pre-fix behaviour is a segfault or an infinite loop; a subprocess lets the test
fail cleanly instead of killing the test runner. The mutation tests also force
``PYTHONMALLOC=debug`` so that use-after-free is deterministic.
"""

import os
import subprocess
import sys
import textwrap

import pytest

from .utils import requires_subprocess

_ENV = {**os.environ, "PYTHONMALLOC": "debug"}


def _run(source, *, timeout=60):
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(source)],
        capture_output=True,
        check=False,
        text=True,
        timeout=timeout,
        env=_ENV,
    )


@requires_subprocess
def test_encode_list_mutated_by_callback_raises():
    """A `struct_dump` callback that clears the list being encoded must raise,
    not walk a freed `ob_item` array."""
    proc = _run(
        """
        import structtype
        from structtype._core import _json_encode

        values = []

        class Boom:
            def struct_dump(self):
                values.clear()
                return 1

        values.extend([Boom(), 2])
        try:
            _json_encode(values)
        except structtype.EncodeError:
            print("ok")
        else:
            print("no-error")
        """
    )
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.strip() == "ok"


@requires_subprocess
def test_encode_nested_list_mutated_by_callback_raises():
    """The currently-encoded container must stay alive even when a callback
    drops the parent's reference to it."""
    proc = _run(
        """
        import structtype
        from structtype._core import _json_encode

        values = []

        class Boom:
            def struct_dump(self):
                values.clear()
                return 1

        values.extend([[Boom(), 1], 2])
        try:
            _json_encode(values)
        except structtype.EncodeError:
            print("ok")
        else:
            print("no-error")
        """
    )
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.strip() == "ok"


@requires_subprocess
def test_dump_list_mutated_by_callback_raises():
    """Same hazard in the builtin-types dump path (`struct_dump`)."""
    proc = _run(
        """
        import structtype
        from structtype._core import _dump

        values = []

        class Boom:
            def struct_dump(self):
                values.clear()
                return 1

        values.extend([Boom(), 2])
        try:
            _dump(values)
        except structtype.EncodeError:
            print("ok")
        else:
            print("no-error")
        """
    )
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.strip() == "ok"


@requires_subprocess
@pytest.mark.parametrize("which", ["encoder", "decoder"])
def test_encoder_decoder_without_init_raises(which):
    """`__new__` without `__init__` used to leave NULL state behind."""
    if which == "encoder":
        source = """
        from structtype._core import JSONEncoder

        enc = JSONEncoder.__new__(JSONEncoder)
        try:
            enc.encode(object())
        except RuntimeError as exc:
            assert "not initialized" in str(exc), exc
            print("ok")
        else:
            print("no-error")
        """
    else:
        source = """
        from structtype._core import JSONDecoder

        dec = JSONDecoder.__new__(JSONDecoder)
        try:
            dec.decode(b"1")
        except RuntimeError as exc:
            assert "not initialized" in str(exc), exc
            print("ok")
        else:
            print("no-error")
        """
    proc = _run(source)
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.strip() == "ok"


@requires_subprocess
def test_decoder_reinit_during_decode_raises():
    """Re-initializing a decoder from a decode callback used to free the
    TypeNode tree the outer decode was walking."""
    proc = _run(
        """
        import datetime
        from typing import Annotated

        from structtype import Serializer
        from structtype._core import JSONDecoder

        dec = None

        def load(value):
            dec.__init__(int)
            return datetime.date.fromisoformat(value)

        dec = JSONDecoder(list[Annotated[datetime.date, Serializer(load=load)]])
        try:
            dec.decode(b'["2024-01-01","2024-01-02"]')
        except RuntimeError as exc:
            assert "re-initialize" in str(exc), exc
            print("ok")
        else:
            print("no-error")
        """
    )
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.strip() == "ok"


@requires_subprocess
def test_struct_info_reentrant_decode_raises_not_hangs():
    """Decoding a struct while its own StructInfo is being built used to spin
    forever in TypeNode_get_struct_info's wait loop."""
    proc = _run(
        """
        import structtype
        from typing import TypedDict

        def hook():
            Outer.struct_validate_json(b'{"inner": {"x": 1}}')
            return int

        class Outer(structtype.Struct):
            inner: "TD"

        class TD(TypedDict):
            x: "hook()"

        try:
            Outer.struct_validate_json(b'{"inner": {"x": 1}}')
        except RuntimeError as exc:
            assert "being built" in str(exc), exc
            print("ok")
        else:
            print("no-error")
        """,
        timeout=30,
    )
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.strip() == "ok"
