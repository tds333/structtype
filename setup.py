import os
import platform
import sys

from setuptools import setup
from setuptools.extension import Extension

# Check for 32-bit windows builds, which currently aren't supported. We can't
# rely on `platform.architecture` here since users can still run 32-bit python
# builds on 64 bit architectures.
if sys.platform == "win32" and sys.maxsize == (2**31 - 1):
    import textwrap

    error = """
    ====================================================================
    `structtype` currently doesn't support 32-bit Python windows builds.
    ====================================================================
    """
    print(textwrap.dedent(error))
    exit(1)


SANITIZE = os.environ.get("STRUCTTYPE_SANITIZE", False)
COVERAGE = os.environ.get("STRUCTTYPE_COVERAGE", False)
DEBUG = os.environ.get("STRUCTTYPE_DEBUG", SANITIZE or COVERAGE)

extra_compile_args = []
extra_link_args = []
if SANITIZE:
    extra_compile_args.extend(["-fsanitize=address", "-fsanitize=undefined"])
    extra_link_args.extend(["-lasan", "-lubsan"])
if COVERAGE:
    extra_compile_args.append("--coverage")
    extra_link_args.append("-lgcov")
if DEBUG:
    extra_compile_args.extend(["-O0", "-g", "-UNDEBUG"])
elif sys.platform != "win32":
    extra_compile_args.extend(["-g0"])
    if sys.platform == "darwin" and platform.machine().lower() == "arm64":
        extra_compile_args.extend(["-flto=thin"])
        extra_link_args.extend(["-flto=thin"])

# from https://py-free-threading.github.io/faq/#im-trying-to-build-a-library-on-windows-but-msvc-says-c-atomic-support-is-not-enabled
if sys.platform == "win32":
    extra_compile_args.extend(
        [
            "/std:c11",
            "/experimental:c11atomics",
        ]
    )

libraries = []
if sys.platform != "win32":
    libraries.append("m")

ext_modules = [
    Extension(
        "structtype._core",
        [os.path.join("src", "structtype", "_core.c")],
        libraries=libraries,
        extra_compile_args=extra_compile_args,
        extra_link_args=extra_link_args,
    )
]

setup(
    ext_modules=ext_modules,
)
