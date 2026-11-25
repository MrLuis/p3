# Unit tests (C++), run through lit like everything else.
add_executable(p3-unittests unit/UnitTestMain.cpp unit/DiagnosticsTest.cpp
  unit/SelfhostDiagTest.cpp)
# The selfhost diagnostics test reads selfhost/tests/ from the source tree.
target_compile_definitions(p3-unittests PRIVATE
  P3_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(p3-unittests PRIVATE p3Driver)
set_target_properties(p3-unittests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# lit configuration
find_program(LIT_EXECUTABLE NAMES lit llvm-lit lit.py
  HINTS ${LLVM_TOOLS_BINARY_DIR} $ENV{HOME}/.local/bin)
# LLVM's FileCheck, or the Python port (`pipx install filecheck`) for LLVM
# packages that don't ship it (e.g. Homebrew's llvm@23).
find_program(FILECHECK_EXECUTABLE NAMES FileCheck filecheck
  HINTS ${LLVM_TOOLS_BINARY_DIR} $ENV{HOME}/.local/bin)

configure_file(lit.site.cfg.py.in ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py @ONLY)

if(LIT_EXECUTABLE AND FILECHECK_EXECUTABLE)
  add_custom_target(check
    COMMAND ${LIT_EXECUTABLE} -sv ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS p3 p3-unittests pascalrt rtl
    COMMENT "Running p3 test suite"
    USES_TERMINAL)
  add_test(NAME lit COMMAND ${LIT_EXECUTABLE} -sv ${CMAKE_CURRENT_BINARY_DIR})
else()
  message(WARNING
    "'check' target disabled: lit='${LIT_EXECUTABLE}' FileCheck='${FILECHECK_EXECUTABLE}'.\n"
    "Install the missing tool(s), then re-run cmake:\n"
    "  lit:       pipx install lit   (or pip install lit)\n"
    "  FileCheck: pipx install filecheck   (or llvm-${P3_LLVM_VERSION}-tools on Debian/Ubuntu)")
endif()

# The bootstrap of the self-hosted compiler (SELFHOST.md S9): stage 1 built
# by p3, stages 2 and 3 by the previous stage; their IR (for the compiler
# and every .pas file) and the stage 2 and 3 executables must be identical.
# Adds the Free Pascal path when fpc is installed.
find_package(Python3 COMPONENTS Interpreter)
find_program(FPC_EXECUTABLE fpc)
# Both need the self-hosted compiler's build flags (top-level CMakeLists.txt).
if(Python3_FOUND AND P3_SELFHOST_FLAGS)
  set(P3_BOOTSTRAP_ARGS --modules --separate)
  if(FPC_EXECUTABLE)
    list(APPEND P3_BOOTSTRAP_ARGS --fpc)
  endif()
  add_custom_target(check-bootstrap
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/utils/bootstrap.py
            --p3 $<TARGET_FILE:p3> --runtime $<TARGET_FILE:pascalrt>
            --work ${PROJECT_BINARY_DIR}/bootstrap
            --flags ${PROJECT_BINARY_DIR}/selfhost/flags.txt ${P3_BOOTSTRAP_ARGS}
    DEPENDS p3 pascalrt rtl
    COMMENT "Bootstrapping the self-hosted compiler"
    USES_TERMINAL)
  # The stage-1 seed (S11): build/seed/p3-seed-<version>-<triple>[.tar.gz].
  add_custom_target(seed
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/utils/seed.py make
            --p3 $<TARGET_FILE:p3> --runtime $<TARGET_FILE:pascalrt>
            --flags ${PROJECT_BINARY_DIR}/selfhost/flags.txt
            --out ${PROJECT_BINARY_DIR}/seed
    DEPENDS p3 pascalrt
    COMMENT "Building the stage-1 seed"
    USES_TERMINAL)
endif()
