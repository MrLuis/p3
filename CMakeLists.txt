cmake_minimum_required(VERSION 3.20)
project(p3 VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Installed binaries should be optimized; use -DCMAKE_BUILD_TYPE=Debug (or
# RelWithDebInfo) for development (optimization.md item 24).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# LLVM (pinned major version; override with -DLLVM_DIR=...)
# ---------------------------------------------------------------------------
set(P3_LLVM_VERSION 23)
find_package(LLVM REQUIRED CONFIG
  HINTS /usr/lib/llvm-${P3_LLVM_VERSION}/lib/cmake/llvm
        /opt/homebrew/opt/llvm@${P3_LLVM_VERSION}/lib/cmake/llvm
        /usr/local/opt/llvm@${P3_LLVM_VERSION}/lib/cmake/llvm)
if(NOT LLVM_VERSION_MAJOR EQUAL P3_LLVM_VERSION)
  message(FATAL_ERROR "p3 requires LLVM ${P3_LLVM_VERSION}.x, found "
                      "${LLVM_PACKAGE_VERSION} (${LLVM_DIR}). Pass -DLLVM_DIR=...")
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
# The PGO profile runtime is searched in LLVM's libraries directory.
cmake_path(GET LLVM_DIR PARENT_PATH P3_LLVM_LIBDIR_TWO)
cmake_path(GET P3_LLVM_LIBDIR_TWO PARENT_PATH P3_LLVM_LIBDIR_TWO)
set(P3_LLVM_LIBDIR "")
foreach(_D IN LISTS LLVM_LIBRARY_DIRS LLVM_DIR)
  cmake_path(GET _D PARENT_PATH _P1)
  cmake_path(GET _P1 PARENT_PATH _P1)
  if(EXISTS ${_P1}/clang)
    set(P3_LLVM_LIBDIR ${_P1})
    break()
  endif()
endforeach()
if(P3_LLVM_LIBDIR STREQUAL "")
  execute_process(COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --libdir OUTPUT_VARIABLE P3_LLVM_LIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
add_compile_definitions(P3_LLVM_LIBDIR="${P3_LLVM_LIBDIR}")
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()

option(P3_LINK_LLVM_DYLIB "Link against the shared libLLVM if available" ON)
option(P3_SELFHOST_NO_CHECKS
       "Build the self-hosted compiler with range and overflow checks off" OFF)
if(P3_LINK_LLVM_DYLIB AND TARGET LLVM)
  set(P3_LLVM_LIBS LLVM)
else()
  llvm_map_components_to_libnames(P3_LLVM_LIBS
    core support target targetparser passes mc native irreader linker
    ${LLVM_TARGETS_TO_BUILD})
endif()

# For a downloadable package with LLVM linked statically: LLVM's libraries
# name Z3, which p3 uses nothing of but which the linker keeps when it is
# named by path (Homebrew's), and a shared zstd that the user's system may
# not have. Link zstd statically and leave Z3 out; a link error says if a
# later LLVM needs it.
option(P3_LLVM_STATIC_DEPS
       "With P3_LINK_LLVM_DYLIB=OFF, link LLVM's zstd (and on Windows zlib) statically and leave out Z3" OFF)
if(P3_LLVM_STATIC_DEPS)
  if(NOT P3_LLVM_LIBS STREQUAL "LLVM" AND TARGET LLVMSupport)
    get_target_property(_p3_deps LLVMSupport INTERFACE_LINK_LIBRARIES)
    list(FILTER _p3_deps EXCLUDE REGEX "[zZ]3")
    if(TARGET zstd::libzstd_static)
      list(TRANSFORM _p3_deps REPLACE "^zstd::libzstd_shared$" "zstd::libzstd_static")
    elseif("zstd::libzstd_shared" IN_LIST _p3_deps)
      message(FATAL_ERROR "P3_LLVM_STATIC_DEPS: LLVM links a shared zstd, and no static one was found")
    endif()
    # Windows has no zlib of its own: ZLIB::ZLIB names MSYS2's import
    # library (libz-1.dll), so link its static library instead.
    if(WIN32 AND "ZLIB::ZLIB" IN_LIST _p3_deps)
      find_library(P3_ZLIB_STATIC NAMES libz.a REQUIRED)
      list(TRANSFORM _p3_deps REPLACE "^ZLIB::ZLIB$" "${P3_ZLIB_STATIC}")
    endif()
    set_target_properties(LLVMSupport PROPERTIES INTERFACE_LINK_LIBRARIES "${_p3_deps}")
  else()
    message(FATAL_ERROR "P3_LLVM_STATIC_DEPS needs P3_LINK_LLVM_DYLIB=OFF")
  endif()
endif()

# Windows packages (docs/build.md): p3 links Windows programs with lld
# inside it, from MinGW-w64's libraries in <prefix>/lib/mingw-w64, and
# needs no C toolchain. lld's static libraries are linked by file (an LLVM
# distribution's lld targets may name the shared libLLVM, which would put a
# second LLVM in p3), with the LLVM components they use, but not
# WindowsManifest: it needs libxml2, and src/driver/WindowsManifestStub.cpp
# stands in for it.
option(P3_EMBED_LLD
       "Link Windows programs with lld inside p3 (MinGW hosts; LLVM linked statically)" OFF)
set(P3_LLD_LIBS "")
if(P3_EMBED_LLD)
  if(P3_LLVM_LIBS STREQUAL "LLVM")
    message(FATAL_ERROR "P3_EMBED_LLD needs P3_LINK_LLVM_DYLIB=OFF")
  endif()
  foreach(lib lldMinGW lldCOFF lldCommon)
    find_library(P3_${lib}_LIBRARY NAMES lib${lib}.a HINTS ${LLVM_LIBRARY_DIR}
                 NO_DEFAULT_PATH REQUIRED)
    list(APPEND P3_LLD_LIBS ${P3_${lib}_LIBRARY})
  endforeach()
  find_path(P3_LLD_INCLUDE_DIR lld/Common/Driver.h HINTS ${LLVM_INCLUDE_DIRS}
            NO_DEFAULT_PATH REQUIRED)
  include_directories(SYSTEM ${P3_LLD_INCLUDE_DIR})
  llvm_map_components_to_libnames(P3_LLD_LLVM_LIBS
    binaryformat bitwriter codegen core debuginfocodeview debuginfodwarf dtlto
    debuginfomsf debuginfopdb demangle libdriver lto mc object option passes
    support target targetparser windowsdriver
    ${LLVM_TARGETS_TO_BUILD})
  list(APPEND P3_LLD_LIBS ${P3_LLD_LLVM_LIBS})
  message(STATUS "p3 links Windows programs with lld inside it")
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(src)
add_subdirectory(runtime)
add_subdirectory(rtl)

# ---------------------------------------------------------------------------
# The self-hosted compiler (SELFHOST.md S11) links with libLLVM through the
# LLVM-C API. Its LLVM-specific parts live in the build directory:
#   selfhost/llvmtargets.inc  the target initializers of this LLVM
#   selfhost/flags.txt        the options to build selfhost/p3.pas with p3
# Both are needed; if either cannot be made (no Python 3, no llvm-config),
# P3_SELFHOST_FLAGS stays empty, flags.txt is not written, and the
# selfhost tests and bootstrap targets are left out with a warning. The
# C++ p3 builds and is tested as usual.
# ---------------------------------------------------------------------------
set(P3_SELFHOST_DIR ${PROJECT_BINARY_DIR}/selfhost)
file(MAKE_DIRECTORY ${P3_SELFHOST_DIR})
set(P3_SELFHOST_FLAGS "")
set(P3_SELFHOST_PROBLEM "")
find_package(Python3 COMPONENTS Interpreter)
list(GET LLVM_INCLUDE_DIRS 0 P3_LLVM_INCLUDE_DIR)
if(Python3_FOUND)
  execute_process(COMMAND ${Python3_EXECUTABLE}
                          ${PROJECT_SOURCE_DIR}/utils/gen-llvm-targets.py
                          ${P3_LLVM_INCLUDE_DIR} ${P3_SELFHOST_DIR}/llvmtargets.inc
                  RESULT_VARIABLE P3_GEN_TARGETS_RESULT)
  if(NOT P3_GEN_TARGETS_RESULT EQUAL 0)
    set(P3_SELFHOST_PROBLEM "utils/gen-llvm-targets.py failed (${P3_GEN_TARGETS_RESULT})")
  endif()
else()
  set(P3_SELFHOST_PROBLEM "no Python 3 to generate selfhost/llvmtargets.inc")
endif()
find_program(P3_LLVM_CONFIG NAMES llvm-config-${P3_LLVM_VERSION} llvm-config
             HINTS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
if(NOT P3_SELFHOST_PROBLEM AND NOT P3_LLVM_CONFIG)
  set(P3_SELFHOST_PROBLEM "no llvm-config in ${LLVM_TOOLS_BINARY_DIR}")
endif()
if(NOT P3_SELFHOST_PROBLEM)
  execute_process(COMMAND ${P3_LLVM_CONFIG} --libdir
                  OUTPUT_VARIABLE P3_SELFHOST_LLVM_LIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE P3_LIBDIR_RESULT)
  execute_process(COMMAND ${P3_LLVM_CONFIG} --link-shared --libs
                  OUTPUT_VARIABLE P3_LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE P3_LIBS_RESULT)
  if(NOT P3_LIBDIR_RESULT EQUAL 0 OR NOT P3_LIBS_RESULT EQUAL 0
     OR P3_SELFHOST_LLVM_LIBDIR STREQUAL "" OR P3_LLVM_LIBS STREQUAL "")
    set(P3_SELFHOST_PROBLEM
        "${P3_LLVM_CONFIG} gave no libLLVM to link (--libdir, --link-shared --libs)")
  endif()
endif()
if(P3_SELFHOST_PROBLEM)
  file(REMOVE ${P3_SELFHOST_DIR}/flags.txt)
  message(WARNING "The self-hosted compiler cannot be built: ${P3_SELFHOST_PROBLEM}. "
                  "Its tests and the check-bootstrap and seed targets are left out.")
else()
  set(P3_SELFHOST_FLAGS "-I ${P3_SELFHOST_DIR} -L${P3_SELFHOST_LLVM_LIBDIR} ${P3_LLVM_LIBS}")
  file(WRITE ${P3_SELFHOST_DIR}/flags.txt "${P3_SELFHOST_FLAGS}\n")
  message(STATUS "Self-hosted compiler build flags: ${P3_SELFHOST_FLAGS}")
endif()

# `ninja p3-self`: the self-hosted compiler, built by p3 at -O2 as
# <build>/bin/p3-self, next to p3, so that it finds the same runtime and
# unit library (<bindir>/../lib). examples/ builds its projects with it.
#
# On macOS, inlining the runtime IR into the self-hosted compiler produces a
# p3-self that crashes while compiling routines with string results (a
# PAGEOFF12 relocation in the large, inlined code generator is not applied by
# the system linker). Keep the compiler binary on the plain runtime calls.
set(P3_SELFHOST_RUNTIME_FLAGS "")
if(APPLE)
  set(P3_SELFHOST_RUNTIME_FLAGS -fno-inline-runtime)
endif()
set(P3_SELFHOST_CHECK_FLAGS "")
if(P3_SELFHOST_NO_CHECKS)
  set(P3_SELFHOST_CHECK_FLAGS -fno-range-checks -fno-overflow-checks)
endif()
if(P3_SELFHOST_FLAGS)
  file(GLOB P3_SELFHOST_SOURCES CONFIGURE_DEPENDS
       ${PROJECT_SOURCE_DIR}/selfhost/*.pas ${PROJECT_SOURCE_DIR}/selfhost/*.inc)
  separate_arguments(P3_SELFHOST_FLAG_LIST NATIVE_COMMAND "${P3_SELFHOST_FLAGS}")
  add_custom_command(
    OUTPUT ${PROJECT_BINARY_DIR}/bin/p3-self
    COMMAND $<TARGET_FILE:p3> --no-project --mode=tp -O2 ${P3_SELFHOST_RUNTIME_FLAGS}
            ${P3_SELFHOST_CHECK_FLAGS} ${P3_SELFHOST_FLAG_LIST}
            -I ${PROJECT_SOURCE_DIR}/selfhost ${PROJECT_SOURCE_DIR}/selfhost/p3.pas
            -o ${PROJECT_BINARY_DIR}/bin/p3-self
    DEPENDS p3 pascalrt rtl ${P3_SELFHOST_SOURCES}
    COMMENT "Building the self-hosted compiler (bin/p3-self)"
    VERBATIM)
  add_custom_target(p3-self DEPENDS ${PROJECT_BINARY_DIR}/bin/p3-self)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Fuzzing (see tools/fuzz/p3-fuzz.cpp)
# ---------------------------------------------------------------------------
option(P3_ENABLE_FUZZING "Build the libFuzzer target p3-fuzz" OFF)
if(P3_ENABLE_FUZZING)
  add_executable(p3-fuzz tools/fuzz/p3-fuzz.cpp)
  target_link_libraries(p3-fuzz PRIVATE p3Driver)
  target_link_options(p3-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  set_target_properties(p3-fuzz PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()
