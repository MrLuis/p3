# -*- Python -*-
# lit configuration for the pcc test suite.
#
# Substitutions available in RUN lines:
#   %pcc            the compiler under test
#   %pcc-unittests  the C++ unit test binary
#   FileCheck, not  from the LLVM tools directory
import os

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
# The tools and runtime pcc uses, if set; the self-hosted compiler also
# reads PCC_CLANG (selfhost/pcdriver.pas).
for var in ("PCC_RUNTIME_LIB", "PCC_CC", "PCC_CLANG"):
    if os.environ.get(var):
        config.environment[var] = os.environ[var]
