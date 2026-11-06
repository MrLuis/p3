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

config.substitutions.append(("%pcc-unittests",
                             os.path.join(config.pcc_tools_dir, "pcc-unittests")))
# Always disable colors so output is stable when run from a terminal.
# PCC_UNDER_TEST=<path> runs the suite with another compiler as %pcc, e.g.
# the self-hosted one (SELFHOST.md): lit -sv build/tests/sema
pcc_under_test = os.environ.get("PCC_UNDER_TEST") or \
    os.path.join(config.pcc_tools_dir, "pcc")
config.substitutions.append(("%pcc", pcc_under_test + " -fdiagnostics-color=never"))
# %selfhost-cc: utils/selfhost-cc.py, a pcc front for the self-hosted
# compiler until it has its own driver (S8). It reads SELFHOST_PCC (set in
# RUN lines or the environment), LLVM_BIN and PCC_RUNTIME_LIB (defaults:
# the LLVM tools found by CMake and this build's runtime).
config.substitutions.append(
    ("%selfhost-cc",
     "python3 " + os.path.join(config.pcc_src_root, "utils", "selfhost-cc.py")))
config.environment["LLVM_BIN"] = config.llvm_tools_dir
config.environment["PCC_RUNTIME_LIB"] = os.path.join(config.pcc_obj_root, "lib",
                                                     "libpascalrt.a")
for var in ("SELFHOST_PCC", "LLVM_BIN", "PCC_RUNTIME_LIB"):
    if os.environ.get(var):
        config.environment[var] = os.environ[var]
if os.path.exists(os.path.join(config.environment["LLVM_BIN"], "clang")):
    config.available_features.add("selfhost-clang")
