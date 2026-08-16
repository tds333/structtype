"""Verify the shipped type stubs work with real type checkers.

Runs ``ty``, ``pyrefly``, and ``mypy`` over the consumer-style fixtures in
``tests/typecheck/`` and asserts each reports no errors. If a tool is not
available (no ``uvx``, or it cannot be fetched/run), the test is skipped so a
minimal CI environment without network does not fail.
"""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

FIXTURE_DIR = Path(__file__).parent / "typecheck"

CHECKERS = {
    "ty": ["ty", "check", str(FIXTURE_DIR)],
    "pyrefly": ["pyrefly", "check", str(FIXTURE_DIR)],
    "mypy": ["mypy", "--python-executable", sys.executable, str(FIXTURE_DIR)],
}


@pytest.mark.parametrize("name", sorted(CHECKERS))
def test_typecheck(name):
    if shutil.which("uvx") is None:
        pytest.skip("uvx not available")

    tool = CHECKERS[name][0]
    probe = subprocess.run(["uvx", tool, "--version"], capture_output=True, check=False)
    if probe.returncode != 0:
        pytest.skip(
            f"{tool} unavailable: {probe.stderr.decode(errors='replace').strip()}"
        )

    result = subprocess.run(
        ["uvx", *CHECKERS[name]],
        capture_output=True,
        text=True,
        timeout=300,
        check=False,
    )
    assert result.returncode == 0, (
        f"{tool} reported errors over {FIXTURE_DIR}:\n{result.stdout}\n{result.stderr}"
    )
