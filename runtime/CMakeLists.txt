# libpascalrt: the C runtime linked into every Pascal executable.
# Lands in <build>/lib so the compiler finds it at <bindir>/../lib.
add_library(pascalrt STATIC io.c sys.c ansi.c format.c async.c)
target_include_directories(pascalrt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# async.c asks for POSIX with _XOPEN_SOURCE, which hides the BSD extensions
# it uses (MAP_ANONYMOUS, _SC_NPROCESSORS_ONLN); _DARWIN_C_SOURCE brings them
# back on macOS.
if(APPLE)
  target_compile_options(pascalrt PRIVATE -D_DARWIN_C_SOURCE)
endif()
# DWARF 4 like p3's own output: GNU ld mis-merges mixed DWARF 4/5 objects.
target_compile_options(pascalrt PRIVATE $<$<CONFIG:Debug>:-gdwarf-4>
  -ffunction-sections -fdata-sections)
# The runtime is part of every program, whatever p3's -O level: optimize it
# in every configuration (optimization.md item 24).
if(MSVC)
  target_compile_options(pascalrt PRIVATE /O2)
else()
  target_compile_options(pascalrt PRIVATE -O2)
endif()
set_target_properties(pascalrt PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON
  ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

# libpascalrt.ll: the runtime as LLVM IR, which p3 links into optimized
# programs so that small runtime functions are inlined (optimization.md,
# item 3). Needs this LLVM's own clang and llvm-link; without them p3
# works as before, calling the library.
find_program(P3_RT_CLANG NAMES clang-${P3_LLVM_VERSION} clang
             HINTS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
find_program(P3_RT_LLVM_LINK NAMES llvm-link-${P3_LLVM_VERSION} llvm-link
             HINTS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
find_package(Python3 COMPONENTS Interpreter)
set(P3_RT_IR_FLAGS -O2 -g0 -std=gnu11 -fPIC -fno-strict-aliasing
    -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables)
set(P3_RT_IR_PROBLEM "")
if(NOT P3_RT_CLANG OR NOT P3_RT_LLVM_LINK)
  set(P3_RT_IR_PROBLEM
      "it needs clang and llvm-link in ${LLVM_TOOLS_BINARY_DIR}")
elseif(NOT Python3_FOUND)
  set(P3_RT_IR_PROBLEM "it needs Python 3")
else()
  # macOS: LLVM's clang (Homebrew's) looks for the SDK its configuration
  # file names, which need not be the installed one, and then does not find
  # the C headers. Name the installed SDK, as the system compiler does.
  if(APPLE)
    set(P3_RT_SYSROOT "${CMAKE_OSX_SYSROOT}")
    if(NOT P3_RT_SYSROOT OR NOT IS_DIRECTORY "${P3_RT_SYSROOT}")
      execute_process(COMMAND xcrun --show-sdk-path
                      OUTPUT_VARIABLE P3_RT_SYSROOT
                      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(P3_RT_SYSROOT AND IS_DIRECTORY "${P3_RT_SYSROOT}")
      list(APPEND P3_RT_IR_FLAGS -isysroot ${P3_RT_SYSROOT})
    endif()
  endif()
  # Can this clang compile the runtime at all? If not, go without the IR
  # instead of failing the build.
  execute_process(
    COMMAND ${P3_RT_CLANG} ${P3_RT_IR_FLAGS}
            -I ${CMAKE_CURRENT_SOURCE_DIR}/include -S -emit-llvm
            ${CMAKE_CURRENT_SOURCE_DIR}/sys.c -o ${CMAKE_CURRENT_BINARY_DIR}/probe.ll
    RESULT_VARIABLE P3_RT_PROBE_RESULT
    OUTPUT_QUIET ERROR_VARIABLE P3_RT_PROBE_ERROR)
  if(NOT P3_RT_PROBE_RESULT EQUAL 0)
    string(REGEX MATCH "[^\n]*error[^\n]*" P3_RT_PROBE_LINE "${P3_RT_PROBE_ERROR}")
    if(NOT P3_RT_PROBE_LINE)
      set(P3_RT_PROBE_LINE "exit status ${P3_RT_PROBE_RESULT}")
    endif()
    set(P3_RT_IR_PROBLEM
        "${P3_RT_CLANG} cannot compile the runtime (${P3_RT_PROBE_LINE})")
  endif()
endif()
if(NOT P3_RT_IR_PROBLEM)
  set(P3_RT_IR_DIR ${CMAKE_CURRENT_BINARY_DIR}/ir)
  file(MAKE_DIRECTORY ${P3_RT_IR_DIR})
  set(P3_RT_IR_PARTS "")
  foreach(src io sys ansi format)
    add_custom_command(
      OUTPUT ${P3_RT_IR_DIR}/${src}.ll
      COMMAND ${P3_RT_CLANG} ${P3_RT_IR_FLAGS}
              -I ${CMAKE_CURRENT_SOURCE_DIR}/include -S -emit-llvm
              ${CMAKE_CURRENT_SOURCE_DIR}/${src}.c -o ${P3_RT_IR_DIR}/${src}.ll
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}.c
              ${CMAKE_CURRENT_SOURCE_DIR}/include/pascalrt.h
      VERBATIM)
    list(APPEND P3_RT_IR_PARTS ${P3_RT_IR_DIR}/${src}.ll)
  endforeach()
  add_custom_command(
    OUTPUT ${PROJECT_BINARY_DIR}/lib/libpascalrt.ll
    COMMAND ${P3_RT_LLVM_LINK} -S ${P3_RT_IR_PARTS} -o ${P3_RT_IR_DIR}/linked.ll
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/utils/gen-runtime-ir.py
            ${P3_RT_IR_DIR}/linked.ll ${PROJECT_BINARY_DIR}/lib/libpascalrt.ll
    DEPENDS ${P3_RT_IR_PARTS} ${PROJECT_SOURCE_DIR}/utils/gen-runtime-ir.py
    VERBATIM)
  add_custom_target(pascalrt-ir ALL DEPENDS ${PROJECT_BINARY_DIR}/lib/libpascalrt.ll)
  add_dependencies(pascalrt pascalrt-ir)
else()
  # A file of an earlier configuration would be out of date.
  file(REMOVE ${PROJECT_BINARY_DIR}/lib/libpascalrt.ll)
  message(WARNING "libpascalrt.ll is not built: ${P3_RT_IR_PROBLEM}. p3 works "
                  "without it, but does not inline runtime functions.")
endif()
