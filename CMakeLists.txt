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
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
