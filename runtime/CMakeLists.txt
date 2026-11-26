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
# Windows as the host (MinGW-w64): the socket pollers (wepoll) and MinGW's
# own printf and strtod, as in the Windows runtimes built for other hosts
# below; the C library is the toolchain's.
if(WIN32)
  target_sources(pascalrt PRIVATE wepoll/wepoll.c)
  target_compile_definitions(pascalrt PRIVATE __USE_MINGW_ANSI_STDIO=1)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/wepoll/LICENSE
          DESTINATION share/doc/p3 RENAME LICENSE.wepoll)
endif()
# DWARF 4 like p3's own output: GNU ld mis-merges mixed DWARF 4/5 objects.
target_compile_options(pascalrt PRIVATE $<$<CONFIG:Debug>:-gdwarf-4>
  -ffunction-sections -fdata-sections)
# Build the runtime for a chosen CPU/features too (optimization2 item 9):
# -DP3_RUNTIME_CPU=native (or x86-64-v2, ...) and -DP3_RUNTIME_MATTR=...
# Otherwise the generic baseline stays in place.
set(P3_RUNTIME_CPU "" CACHE STRING "CPU for the runtime library and IR")
set(P3_RUNTIME_MATTR "" CACHE STRING "target features for the runtime library and IR")
if(P3_RUNTIME_CPU)
  target_compile_options(pascalrt PRIVATE -mcpu=${P3_RUNTIME_CPU})
endif()
if(P3_RUNTIME_MATTR)
  target_compile_options(pascalrt PRIVATE -mattr=${P3_RUNTIME_MATTR})
endif()
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
install(TARGETS pascalrt ARCHIVE DESTINATION lib)

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
if(P3_RUNTIME_CPU)
  list(APPEND P3_RT_IR_FLAGS -mcpu=${P3_RUNTIME_CPU})
endif()
if(WIN32)
  list(APPEND P3_RT_IR_FLAGS -D__USE_MINGW_ANSI_STDIO=1)   # as the library
endif()
if(P3_RUNTIME_MATTR)
  list(APPEND P3_RT_IR_FLAGS -mattr=${P3_RUNTIME_MATTR})
endif()
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
  file(MAKE_DIRECTORY ${P3_RT_IR_DIR} ${PROJECT_BINARY_DIR}/lib/pascal)
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
    OUTPUT ${PROJECT_BINARY_DIR}/lib/pascal/libpascalrt.ll
    COMMAND ${P3_RT_LLVM_LINK} -S ${P3_RT_IR_PARTS} -o ${P3_RT_IR_DIR}/linked.ll
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/utils/gen-runtime-ir.py
            ${P3_RT_IR_DIR}/linked.ll ${PROJECT_BINARY_DIR}/lib/pascal/libpascalrt.ll
    DEPENDS ${P3_RT_IR_PARTS} ${PROJECT_SOURCE_DIR}/utils/gen-runtime-ir.py
    VERBATIM)
  add_custom_target(pascalrt-ir ALL DEPENDS ${PROJECT_BINARY_DIR}/lib/pascal/libpascalrt.ll)
  install(FILES ${PROJECT_BINARY_DIR}/lib/pascal/libpascalrt.ll DESTINATION lib/pascal)
  add_dependencies(pascalrt pascalrt-ir)
else()
  # A file of an earlier configuration would be out of date.
  file(REMOVE ${PROJECT_BINARY_DIR}/lib/pascal/libpascalrt.ll)
  message(WARNING "libpascalrt.ll is not built: ${P3_RT_IR_PROBLEM}. p3 works "
                  "without it, but does not inline runtime functions.")
endif()

# The runtime for other targets, so that p3 links executables for them too:
# <build>/lib/<triple>/libpascalrt.a, built with the target's own C
# toolchain (<triple>-gcc and <triple>-ar: MinGW-w64's for Windows). By
# default every target whose compiler is installed.
set(P3_CROSS_TARGET_CANDIDATES x86_64-w64-mingw32)
set(P3_CROSS_TARGETS "" CACHE STRING
    "other targets to build the runtime for (default: those whose <triple>-gcc is found; OFF for none)")
set(P3_CROSS_LIST "")
if(P3_CROSS_TARGETS STREQUAL "")
  foreach(triple ${P3_CROSS_TARGET_CANDIDATES})
    find_program(P3_CROSS_CC_${triple} ${triple}-gcc)
    if(P3_CROSS_CC_${triple})
      list(APPEND P3_CROSS_LIST ${triple})
    endif()
  endforeach()
elseif(NOT P3_CROSS_TARGETS STREQUAL "OFF")
  set(P3_CROSS_LIST ${P3_CROSS_TARGETS})
endif()
foreach(triple ${P3_CROSS_LIST})
  find_program(P3_CROSS_CC_${triple} ${triple}-gcc REQUIRED)
  find_program(P3_CROSS_AR_${triple} ${triple}-ar REQUIRED)
  set(srcs io sys ansi format async)
  # MinGW-w64: one runtime per C library, as the headers see it
  # (__MSVCRT_VERSION__): lib/<triple>/msvcrt and lib/<triple>/ucrt. p3
  # links the one that matches the user's toolchain (or P3_WINDOWS_CRT),
  # whichever C library the toolchain that builds them defaults to.
  if(triple MATCHES "mingw|windows")
    list(APPEND srcs wepoll/wepoll)   # the socket pollers (async.c)
    set(variants msvcrt ucrt)
  else()
    set(variants "")
  endif()
  set(libs "")
  # Both with MinGW-w64's own printf and strtod family
  # (__USE_MINGW_ANSI_STDIO), which the headers choose for msvcrt anyway:
  # UCRT's printf rounds an exact tie away from zero ("%.0f" of 0.5 is 1 on
  # Windows), where p3 rounds to even, as MinGW's and other systems' do.
  foreach(crt IN LISTS variants)
    if(crt STREQUAL "msvcrt")
      set(crtflags -D__MSVCRT_VERSION__=0x700 -D__USE_MINGW_ANSI_STDIO=1)
    elseif(crt STREQUAL "ucrt")
      set(crtflags -D__MSVCRT_VERSION__=0xE00 -D__USE_MINGW_ANSI_STDIO=1)
    else()
      set(crtflags "")
    endif()
    set(dir ${CMAKE_CURRENT_BINARY_DIR}/${triple}/${crt})
    set(libdir ${PROJECT_BINARY_DIR}/lib/${triple})
    if(crt)
      set(libdir ${libdir}/${crt})
    endif()
    file(MAKE_DIRECTORY ${dir} ${libdir})
    set(objs "")
    foreach(src ${srcs})
      get_filename_component(obj ${src} NAME)
      add_custom_command(
        OUTPUT ${dir}/${obj}.o
        COMMAND ${P3_CROSS_CC_${triple}} -std=gnu11 -O2 -ffunction-sections
                -fdata-sections ${crtflags} -I ${CMAKE_CURRENT_SOURCE_DIR}/include
                -c ${CMAKE_CURRENT_SOURCE_DIR}/${src}.c -o ${dir}/${obj}.o
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}.c
                ${CMAKE_CURRENT_SOURCE_DIR}/include/pascalrt.h
                ${CMAKE_CURRENT_SOURCE_DIR}/wepoll/wepoll.h
        VERBATIM)
      list(APPEND objs ${dir}/${obj}.o)
    endforeach()
    set(lib ${libdir}/libpascalrt.a)
    add_custom_command(
      OUTPUT ${lib}
      COMMAND ${CMAKE_COMMAND} -E rm -f ${lib}
      COMMAND ${P3_CROSS_AR_${triple}} rcs ${lib} ${objs}
      DEPENDS ${objs}
      VERBATIM)
    file(RELATIVE_PATH rel ${PROJECT_BINARY_DIR} ${libdir})
    install(FILES ${lib} DESTINATION ${rel})
    list(APPEND libs ${lib})
  endforeach()
  add_custom_target(pascalrt-${triple} ALL DEPENDS ${libs})
  if(triple MATCHES "mingw|windows")
    # wepoll's license goes with the library that contains it.
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/wepoll/LICENSE
            DESTINATION share/doc/p3 RENAME LICENSE.wepoll)
  endif()
  message(STATUS "p3 runtime for ${triple}: ${P3_CROSS_CC_${triple}}")
endforeach()
