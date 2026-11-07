# -*- Python -*-
# lit configuration for the pcc test suite.
#
# Substitutions available in RUN lines:
#   %pcc            the compiler under test
#   %pcc-unittests  the C++ unit test binary
#   FileCheck, not  from the LLVM tools directory
import os
import sys

import lit.formats

config.name = "pcc"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".pas", ".test"]
config.excludes = ["Inputs", "CMakeLists.txt", "lit.cfg.py", "lit.site.cfg.py", "README.md"]

config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.pcc_obj_root, "tests")

# Put the LLVM tools (FileCheck, not, ...) and pcc on PATH.
for d in (config.llvm_tools_dir, config.pcc_tools_dir):
    config.environment["PATH"] = os.pathsep.join((d, config.environment["PATH"]))

# RUN lines say `FileCheck`; point that at whichever implementation CMake
# found (LLVM's, or the Python port `filecheck`).
config.substitutions.append((r"\bFileCheck\b", config.filecheck))

# %selfhost-flags: the options that build selfhost/pcc.pas (its LLVM-C
# backend's include file and libLLVM; before %pcc, a prefix of it).
# Tests using it say REQUIRES: selfhost-build.
#
# Every cmake run regenerates lit.site.cfg.py with selfhost_flags, so it is
# missing only when lit runs on a build directory last configured before
# S11 (whose %pcc is then stale too). It is empty when cmake could not set
# up the self-hosted build (no Python 3 or llvm-config; cmake warned why).
# Either way the selfhost tests are skipped, with a warning so a green run
# does not hide that they did not run. A test that uses %selfhost-flags
# without the REQUIRES line gets an option pcc rejects by name, not an
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
# %pcc-runtime-lib: this build's runtime, for compilers not installed next
# to it (before %pcc, which is a prefix of it).
config.substitutions.append(
    ("%pcc-runtime-lib", os.path.join(config.pcc_obj_root, "lib", "libpascalrt.a")))
config.substitutions.append(("%pcc-unittests",
                             os.path.join(config.pcc_tools_dir, "pcc-unittests")))
# Always disable colors so output is stable when run from a terminal.
# PCC_UNDER_TEST=<path> runs the suite with another compiler as %pcc, e.g.
# the self-hosted one (SELFHOST.md): lit -sv build/tests/sema
pcc_under_test = os.environ.get("PCC_UNDER_TEST") or \
    os.path.join(config.pcc_tools_dir, "pcc")
config.substitutions.append(("%pcc", pcc_under_test + " -fdiagnostics-color=never"))
# The tools and runtime pcc uses, if set.
for var in ("PCC_RUNTIME_LIB", "PCC_CC"):
    if os.environ.get(var):
        config.environment[var] = os.environ[var]

# On macOS the LLVM tools directory put on PATH above usually holds a
# Homebrew clang-19, which pcc's driver tries first for linking. Its config
# file pins an SDK (-isysroot) that need not match the installed Command
# Line Tools, and it ignores SDKROOT, so links fail (`library not found for
# -lm`). Link with the system C compiler instead, unless PCC_CC is set.
if sys.platform == "darwin" and not os.environ.get("PCC_CC"):
    if os.access("/usr/bin/cc", os.X_OK):
        config.environment["PCC_CC"] = "/usr/bin/cc"
    else:
        lit_config.warning("no /usr/bin/cc to link with; set PCC_CC")
