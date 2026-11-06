cmake_minimum_required(VERSION 3.20)
project(pcc VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# LLVM (pinned major version; override with -DLLVM_DIR=...)
# ---------------------------------------------------------------------------
set(PCC_LLVM_VERSION 19)
find_package(LLVM REQUIRED CONFIG
  HINTS /usr/lib/llvm-${PCC_LLVM_VERSION}/lib/cmake/llvm
        /opt/homebrew/opt/llvm@${PCC_LLVM_VERSION}/lib/cmake/llvm
        /usr/local/opt/llvm@${PCC_LLVM_VERSION}/lib/cmake/llvm)
if(NOT LLVM_VERSION_MAJOR EQUAL PCC_LLVM_VERSION)
  message(FATAL_ERROR "pcc requires LLVM ${PCC_LLVM_VERSION}.x, found "
                      "${LLVM_PACKAGE_VERSION} (${LLVM_DIR}). Pass -DLLVM_DIR=...")
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()

option(PCC_LINK_LLVM_DYLIB "Link against the shared libLLVM if available" ON)
if(PCC_LINK_LLVM_DYLIB AND TARGET LLVM)
  set(PCC_LLVM_LIBS LLVM)
else()
  llvm_map_components_to_libnames(PCC_LLVM_LIBS
    core support target targetparser passes mc native
    ${LLVM_TARGETS_TO_BUILD})
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_subdirectory(src)
add_subdirectory(runtime)

# ---------------------------------------------------------------------------
# The self-hosted compiler (SELFHOST.md S11) links with libLLVM through the
# LLVM-C API. Its LLVM-specific parts live in the build directory:
#   selfhost/llvmtargets.inc  the target initializers of this LLVM
#   selfhost/flags.txt        the options to build selfhost/pcc.pas with pcc
# ---------------------------------------------------------------------------
set(PCC_SELFHOST_DIR ${PROJECT_BINARY_DIR}/selfhost)
file(MAKE_DIRECTORY ${PCC_SELFHOST_DIR})
find_package(Python3 COMPONENTS Interpreter)
list(GET LLVM_INCLUDE_DIRS 0 PCC_LLVM_INCLUDE_DIR)
if(Python3_FOUND)
  execute_process(COMMAND ${Python3_EXECUTABLE}
                          ${PROJECT_SOURCE_DIR}/utils/gen-llvm-targets.py
                          ${PCC_LLVM_INCLUDE_DIR} ${PCC_SELFHOST_DIR}/llvmtargets.inc)
endif()
find_program(PCC_LLVM_CONFIG NAMES llvm-config-${PCC_LLVM_VERSION} llvm-config
             HINTS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
execute_process(COMMAND ${PCC_LLVM_CONFIG} --libdir
                OUTPUT_VARIABLE PCC_LLVM_LIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PCC_LLVM_CONFIG} --link-shared --libs
                OUTPUT_VARIABLE PCC_LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
set(PCC_SELFHOST_FLAGS "-I ${PCC_SELFHOST_DIR} -L${PCC_LLVM_LIBDIR} ${PCC_LLVM_LIBS}")
file(WRITE ${PCC_SELFHOST_DIR}/flags.txt "${PCC_SELFHOST_FLAGS}\n")
message(STATUS "Self-hosted compiler build flags: ${PCC_SELFHOST_FLAGS}")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Fuzzing (see tools/fuzz/pcc-fuzz.cpp)
# ---------------------------------------------------------------------------
option(PCC_ENABLE_FUZZING "Build the libFuzzer target pcc-fuzz" OFF)
if(PCC_ENABLE_FUZZING)
  add_executable(pcc-fuzz tools/fuzz/pcc-fuzz.cpp)
  target_link_libraries(pcc-fuzz PRIVATE pccDriver)
  target_link_options(pcc-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  set_target_properties(pcc-fuzz PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()
