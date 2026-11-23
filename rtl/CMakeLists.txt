# p3's unit library: copied to <build>/lib/pascal, where the compiler finds
# it at <bindir>/../lib/pascal, with each unit's object next to its source
# for -fseparate-units (compiled in objfpc mode, for the host).
set(P3_RTL_UNITS sysutils strutils math classes threading sequences netasync netrouter netloop nethttp p3gen)
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

set(P3_RTL_OBJECTS)
foreach(unit ${P3_RTL_UNITS})
  add_custom_command(
    OUTPUT ${P3_RTL_DIR}/${unit}.o
    COMMAND $<TARGET_FILE:p3> --no-project --mode=objfpc -O2 -c
            ${P3_RTL_DIR}/${unit}.pas -o ${P3_RTL_DIR}/${unit}.o
    DEPENDS p3 ${P3_RTL_SOURCES}
            $<$<TARGET_EXISTS:pascalrt-ir>:${PROJECT_BINARY_DIR}/lib/libpascalrt.ll>)
  list(APPEND P3_RTL_OBJECTS ${P3_RTL_DIR}/${unit}.o)
endforeach()

add_custom_target(rtl ALL DEPENDS ${P3_RTL_SOURCES} ${P3_RTL_OBJECTS})
