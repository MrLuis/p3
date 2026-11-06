# Unit tests (C++), run through lit like everything else.
add_executable(pcc-unittests unit/UnitTestMain.cpp unit/DiagnosticsTest.cpp)
target_link_libraries(pcc-unittests PRIVATE pccDriver)
set_target_properties(pcc-unittests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# lit configuration
find_program(LIT_EXECUTABLE NAMES lit llvm-lit lit.py
  HINTS ${LLVM_TOOLS_BINARY_DIR} $ENV{HOME}/.local/bin)
find_program(FILECHECK_EXECUTABLE FileCheck HINTS ${LLVM_TOOLS_BINARY_DIR})

configure_file(lit.site.cfg.py.in ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py @ONLY)

if(LIT_EXECUTABLE AND FILECHECK_EXECUTABLE)
  add_custom_target(check
    COMMAND ${LIT_EXECUTABLE} -sv ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS pcc pcc-unittests pascalrt
    COMMENT "Running pcc test suite"
    USES_TERMINAL)
  add_test(NAME lit COMMAND ${LIT_EXECUTABLE} -sv ${CMAKE_CURRENT_BINARY_DIR})
else()
  message(WARNING "lit or FileCheck not found: 'check' target disabled. "
                  "Install lit (pip install lit) and llvm-${PCC_LLVM_VERSION}-tools.")
endif()
