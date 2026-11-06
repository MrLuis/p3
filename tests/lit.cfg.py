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
config.excludes = ["Inputs", "CMakeLists.txt", "lit.cfg.py", "lit.site.cfg.py"]

config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.pcc_obj_root, "tests")

# Put the LLVM tools (FileCheck, not, ...) and pcc on PATH.
for d in (config.llvm_tools_dir, config.pcc_tools_dir):
    config.environment["PATH"] = os.pathsep.join((d, config.environment["PATH"]))

config.substitutions.append(("%pcc-unittests",
                             os.path.join(config.pcc_tools_dir, "pcc-unittests")))
# Always disable colors so output is stable when run from a terminal.
config.substitutions.append(("%pcc",
                             os.path.join(config.pcc_tools_dir, "pcc") + " -fdiagnostics-color=never"))
