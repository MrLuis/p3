# -*- Python -*-
# lit configuration for the p3 test suite.
#
# Substitutions available in RUN lines:
#   %p3            the compiler under test, ignoring project files (p3.json)
#   %p3-project    the compiler under test, reading project files
#   %p3-unittests  the C++ unit test binary
#   %cmake         the CMake that configured the build
#   %build-dir     the build directory
#   FileCheck, not  from the LLVM tools directory
import os
import re
import sys

import lit.formats

config.name = "p3"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".pas", ".test"]
config.excludes = ["Inputs", "CMakeLists.txt", "lit.cfg.py", "lit.site.cfg.py", "README.md"]

config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.p3_obj_root, "tests")

# Put the LLVM tools (FileCheck, not, ...) and p3 on PATH.
for d in (config.llvm_tools_dir, config.p3_tools_dir):
    config.environment["PATH"] = os.pathsep.join((d, config.environment["PATH"]))

# RUN lines say `FileCheck`; point that at whichever implementation CMake
# found (LLVM's, or the Python port `filecheck`).
config.substitutions.append((r"\bFileCheck\b", config.filecheck))
# Windows as the host: programs end their lines with CR LF and the expected
# files with LF, so diff (the command, not names such as selfhost-diff.py) is
# utils/lit-diff-crlf.py; and X.expected is X.windows.expected where a test
# has one (what differs on Windows by design, as tests/windows does it).
if sys.platform == "win32":
    config.available_features.add("system-windows")
    utils_dir = os.path.join(os.path.dirname(__file__), "..", "utils")
    crlf_diff = os.path.abspath(os.path.join(utils_dir, "lit-diff-crlf.py"))
    config.substitutions.append(
        (r"(?<![-\w./])diff ",
         '"%s" "%s" ' % (sys.executable.replace("\\", "/"), crlf_diff.replace("\\", "/"))))
    for root, _, files in os.walk(os.path.dirname(__file__)):
        for f in files:
            if f.endswith(".windows.expected"):
                base = f[: -len(".windows.expected")]
                config.substitutions.append(
                    (r"/%s\.expected\b" % re.escape(base), "/%s.windows.expected" % base))

# %selfhost-flags: the options that build selfhost/p3.pas (its LLVM-C
# backend's include file and libLLVM; before %p3, a prefix of it).
# Tests using it say REQUIRES: selfhost-build.
#
# Every cmake run regenerates lit.site.cfg.py with selfhost_flags, so it is
# missing only when lit runs on a build directory last configured before
# S11 (whose %p3 is then stale too). It is empty when cmake could not set
# up the self-hosted build (no Python 3 or llvm-config; cmake warned why).
# Either way the selfhost tests are skipped, with a warning so a green run
# does not hide that they did not run. A test that uses %selfhost-flags
# without the REQUIRES line gets an option p3 rejects by name, not an
# empty string and puzzling link errors.
selfhost_flags = getattr(config, "selfhost_flags", None)
if selfhost_flags is None:
    lit_config.warning("build directory predates the self-hosted compiler's "
                       "build flags: the selfhost tests are skipped; re-run "
                       "cmake to run them")
elif not selfhost_flags:
    lit_config.warning("cmake could not set up the self-hosted compiler's "
                       "build (see its warning): the selfhost tests are "
                       "skipped")
else:
    config.available_features.add("selfhost-build")
if not selfhost_flags:
    selfhost_flags = "--missing-selfhost-flags-rerun-cmake"
config.substitutions.append(("%selfhost-flags", selfhost_flags))
# %cmake: unset in a build directory configured before it was added.
config.substitutions.append(("%cmake", getattr(config, "cmake", "cmake")))
config.substitutions.append(("%build-dir", config.p3_obj_root))
# %p3-runtime-lib: this build's runtime, for compilers not installed next
# to it (before %p3, which is a prefix of it).
config.substitutions.append(
    ("%p3-runtime-lib", os.path.join(config.p3_obj_root, "lib", "libpascalrt.a")))
# %p3-runtime-ir: this build's runtime as LLVM IR, which p3 inlines from
# at -O1 and up (before %p3-runtime-lib and %p3, prefixes of it). CMake
# builds it when this LLVM has clang and llvm-link; tests that need it say
# REQUIRES: runtime-ir.
runtime_ir = os.path.join(config.p3_obj_root, "lib", "pascal", "libpascalrt.ll")
if os.path.exists(runtime_ir):
    config.available_features.add("runtime-ir")
config.substitutions.insert(0, ("%p3-runtime-ir", runtime_ir))
config.substitutions.append(("%p3-unittests",
                             os.path.join(config.p3_tools_dir, "p3-unittests")))
# Always disable colors so output is stable when run from a terminal.
# P3_UNDER_TEST=<path> runs the suite with another compiler as %p3, e.g.
# the self-hosted one (SELFHOST.md): lit -sv build/tests/sema
p3_under_test = os.environ.get("P3_UNDER_TEST") or \
    os.path.join(config.p3_tools_dir, "p3")
# %p3 ignores project files, so a p3.json in a directory above the tree
# cannot change the tests; the project tests use %p3-project.
config.p3_under_test = p3_under_test + " -fdiagnostics-color=never --no-project"
config.substitutions.append(("%p3-project", p3_under_test + " -fdiagnostics-color=never"))
config.substitutions.append(("%p3", p3_under_test + " -fdiagnostics-color=never --no-project"))
# The tools and runtime p3 uses, if set.
for var in ("P3_RUNTIME_LIB", "P3_RUNTIME_IR", "P3_CC", "P3_UNIT_DIR"):
    if os.environ.get(var):
        config.environment[var] = os.environ[var]
# This build's unit library, whichever compiler is under test (a self-hosted
# one built by a test does not sit next to it).
config.environment.setdefault("P3_UNIT_DIR", os.path.join(config.p3_obj_root, "lib", "pascal"))

# Windows: the runtime built for MinGW-w64 x86-64 (P3_CROSS_TARGETS), so
# that p3 links Windows programs (REQUIRES: windows-runtime); Wine, which
# runs them (tests/windows).
if os.path.exists(os.path.join(config.p3_obj_root, "lib", "x86_64-w64-mingw32",
                               "msvcrt", "libpascalrt.a")):
    config.available_features.add("windows-runtime")
import shutil
config.wine = shutil.which("wine")
if config.wine:
    config.available_features.add("wine")

# On macOS the LLVM tools directory put on PATH above usually holds a
# Homebrew clang, which p3's driver may pick for linking. Its config
# file pins an SDK (-isysroot) that need not match the installed Command
# Line Tools, and it ignores SDKROOT, so links fail (`library not found for
# -lm`). Link with the system C compiler instead, unless P3_CC is set.
if sys.platform == "darwin" and not os.environ.get("P3_CC"):
    if os.access("/usr/bin/cc", os.X_OK):
        config.environment["P3_CC"] = "/usr/bin/cc"
    else:
        lit_config.warning("no /usr/bin/cc to link with; set P3_CC")
