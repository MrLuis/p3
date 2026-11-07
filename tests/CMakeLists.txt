# Unit tests (C++), run through lit like everything else.
add_executable(pcc-unittests unit/UnitTestMain.cpp unit/DiagnosticsTest.cpp
  unit/SelfhostDiagTest.cpp)
# The selfhost diagnostics test reads selfhost/tests/ from the source tree.
target_compile_definitions(pcc-unittests PRIVATE
  PCC_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(pcc-unittests PRIVATE pccDriver)
set_target_properties(pcc-unittests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# lit configuration
find_program(LIT_EXECUTABLE NAMES lit llvm-lit lit.py
  HINTS ${LLVM_TOOLS_BINARY_DIR} $ENV{HOME}/.local/bin)
# LLVM's FileCheck, or the Python port (`pipx install filecheck`) for LLVM
# packages that don't ship it (e.g. Homebrew's llvm@19).
find_program(FILECHECK_EXECUTABLE NAMES FileCheck filecheck
  HINTS ${LLVM_TOOLS_BINARY_DIR} $ENV{HOME}/.local/bin)

configure_file(lit.site.cfg.py.in ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py @ONLY)

if(LIT_EXECUTABLE AND FILECHECK_EXECUTABLE)
  add_custom_target(check
    COMMAND ${LIT_EXECUTABLE} -sv ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS pcc pcc-unittests pascalrt
    COMMENT "Running pcc test suite"
    USES_TERMINAL)
  add_test(NAME lit COMMAND ${LIT_EXECUTABLE} -sv ${CMAKE_CURRENT_BINARY_DIR})
else()
  message(WARNING
    "'check' target disabled: lit='${LIT_EXECUTABLE}' FileCheck='${FILECHECK_EXECUTABLE}'.\n"
    "Install the missing tool(s), then re-run cmake:\n"
    "  lit:       pipx install lit   (or pip install lit)\n"
    "  FileCheck: pipx install filecheck   (or llvm-${PCC_LLVM_VERSION}-tools on Debian/Ubuntu)")
endif()

# The bootstrap of the self-hosted compiler (SELFHOST.md S9): stage 1 built
# by pcc, stages 2 and 3 by the previous stage; their IR (for the compiler
# and every .pas file) and the stage 2 and 3 executables must be identical.
# Adds the Free Pascal path when fpc is installed.
find_package(Python3 COMPONENTS Interpreter)
find_program(FPC_EXECUTABLE fpc)
# Both need the self-hosted compiler's build flags (top-level CMakeLists.txt).
if(Python3_FOUND AND PCC_SELFHOST_FLAGS)
  set(PCC_BOOTSTRAP_ARGS --modules --separate)
  if(FPC_EXECUTABLE)
    list(APPEND PCC_BOOTSTRAP_ARGS --fpc)
  endif()
  add_custom_target(check-bootstrap
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/utils/bootstrap.py
            --pcc $<TARGET_FILE:pcc> --runtime $<TARGET_FILE:pascalrt>
            --work ${PROJECT_BINARY_DIR}/bootstrap
            --flags ${PROJECT_BINARY_DIR}/selfhost/flags.txt ${PCC_BOOTSTRAP_ARGS}
    DEPENDS pcc pascalrt
    COMMENT "Bootstrapping the self-hosted compiler"
    USES_TERMINAL)
  # The stage-1 seed (S11): build/seed/pcc-seed-<version>-<triple>[.tar.gz].
  add_custom_target(seed
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/utils/seed.py make
            --pcc $<TARGET_FILE:pcc> --runtime $<TARGET_FILE:pascalrt>
            --flags ${PROJECT_BINARY_DIR}/selfhost/flags.txt
            --out ${PROJECT_BINARY_DIR}/seed
    DEPENDS pcc pascalrt
    COMMENT "Building the stage-1 seed"
    USES_TERMINAL)
endif()
