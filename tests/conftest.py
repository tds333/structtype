import math
import os
import random
import string
import struct

import pytest

# Re-inject the ASan runtime for subprocess children. macOS strips
# DYLD_INSERT_LIBRARIES from the process environment after dyld consumes it,
# so tests that spawn subprocesses (e.g. test_raw_copy_doesnt_leak) would
# otherwise run without the sanitizer preload. STRUCTTYPE_ASAN_RUNTIME is set
# by `make test-sanitize` and survives into the parent environment.
asan_runtime = os.environ.get("STRUCTTYPE_ASAN_RUNTIME")
if asan_runtime:
    os.environ["DYLD_INSERT_LIBRARIES"] = asan_runtime


class Rand:
    """Random source, pulled out into fixture with repr so the seed is
    displayed on failing tests"""

    def __init__(self, seed=0):
        self.seed = seed or random.randint(0, 2**32 - 1)
        self.rand = random.Random(self.seed)

    def __repr__(self):
        return f"Rand({self.seed})"

    def str(self, n, m=0):
        """
        str(n) -> random string of length `n`.
        str(n, m) -> random string between lengths `n` & `m`
        """
        if m:
            n = self.rand.randint(n, m)
        return "".join(self.rand.choices(string.ascii_letters, k=n))

    def bytes(self, n):
        """random bytes of length `n`"""
        return self.rand.getrandbits(8 * n).to_bytes(n, "little")

    def float(self):
        """random finite float"""
        while True:
            dbytes = self.rand.getrandbits(64).to_bytes(8, "big")
            x = struct.unpack("!d", dbytes)[0]
            if math.isfinite(x):
                return x

    def shuffle(self, obj):
        """random shuffle"""
        self.rand.shuffle(obj)


@pytest.fixture
def rand():
    yield Rand()


@pytest.fixture(scope="session")
def package_dir(pytestconfig):
    return pytestconfig.rootpath.joinpath("src", "structtype")
