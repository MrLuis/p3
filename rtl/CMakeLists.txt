# p3's unit library: copied to <build>/lib/pascal, where the compiler finds
# it at <bindir>/../lib/pascal, with each unit's object next to its source
# for -fseparate-units (compiled in objfpc mode, for the host).
set(P3_RTL_UNITS sysutils strutils math classes threading sequences netasync netrouter netloop nethttp p3gen json sqlite3 libpq)
set(P3_RTL_DIR ${PROJECT_BINARY_DIR}/lib/pascal)

set(P3_RTL_SOURCES)
foreach(unit ${P3_RTL_UNITS})
  add_custom_command(
    OUTPUT ${P3_RTL_DIR}/${unit}.pas
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/${unit}.pas ${P3_RTL_DIR}/${unit}.pas
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${unit}.pas)
  list(APPEND P3_RTL_SOURCES ${P3_RTL_DIR}/${unit}.pas)
endforeach()

# Include files of the units (before the objects, which depend on them).
foreach(inc json_pow5.inc)
  add_custom_command(
    OUTPUT ${P3_RTL_DIR}/${inc}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/${inc} ${P3_RTL_DIR}/${inc}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${inc})
  list(APPEND P3_RTL_SOURCES ${P3_RTL_DIR}/${inc})
endforeach()

set(P3_RTL_OBJECTS)
foreach(unit ${P3_RTL_UNITS})
  add_custom_command(
    OUTPUT ${P3_RTL_DIR}/${unit}.o
    COMMAND $<TARGET_FILE:p3> --no-project --mode=objfpc -O2 -c
            ${P3_RTL_DIR}/${unit}.pas -o ${P3_RTL_DIR}/${unit}.o
    DEPENDS p3 ${P3_RTL_SOURCES}
            $<$<TARGET_EXISTS:pascalrt-ir>:${PROJECT_BINARY_DIR}/lib/pascal/libpascalrt.ll>)
  list(APPEND P3_RTL_OBJECTS ${P3_RTL_DIR}/${unit}.o)
endforeach()

# Source generators of the library (--generator=NAME, "generators":
# ["NAME"] in p3.json): <bindir>/../lib/pascal/generators/NAME.pas.
set(P3_RTL_GENERATORS json)
foreach(gen ${P3_RTL_GENERATORS})
  add_custom_command(
    OUTPUT ${P3_RTL_DIR}/generators/${gen}.pas
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/generators/${gen}.pas
            ${P3_RTL_DIR}/generators/${gen}.pas
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generators/${gen}.pas)
  list(APPEND P3_RTL_SOURCES ${P3_RTL_DIR}/generators/${gen}.pas)
endforeach()

add_custom_target(rtl ALL DEPENDS ${P3_RTL_SOURCES} ${P3_RTL_OBJECTS})

# `cmake --install`: the unit library as it is in the build directory. Each
# unit's .p3i (not written for every unit) and .ll are by-products of
# compiling its object, for -fseparate-units. Files are named one by one, so
# that leftovers in an old build directory are not installed.
foreach(unit ${P3_RTL_UNITS})
  install(FILES ${P3_RTL_DIR}/${unit}.pas ${P3_RTL_DIR}/${unit}.o
          DESTINATION lib/pascal)
  install(FILES ${P3_RTL_DIR}/${unit}.p3i ${P3_RTL_DIR}/${unit}.ll
          DESTINATION lib/pascal OPTIONAL)
endforeach()
install(FILES ${P3_RTL_DIR}/json_pow5.inc DESTINATION lib/pascal)
foreach(gen ${P3_RTL_GENERATORS})
  install(FILES ${P3_RTL_DIR}/generators/${gen}.pas
          DESTINATION lib/pascal/generators)
endforeach()
