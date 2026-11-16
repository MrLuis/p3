# pcc's unit library: copied to <build>/lib/pascal, where the compiler finds
# it at <bindir>/../lib/pascal, with each unit's object next to its source
# for -fseparate-units (compiled in objfpc mode, for the host).
set(PCC_RTL_UNITS sysutils strutils math classes threading)
set(PCC_RTL_DIR ${PROJECT_BINARY_DIR}/lib/pascal)

set(PCC_RTL_SOURCES)
foreach(unit ${PCC_RTL_UNITS})
  add_custom_command(
    OUTPUT ${PCC_RTL_DIR}/${unit}.pas
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/${unit}.pas ${PCC_RTL_DIR}/${unit}.pas
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${unit}.pas)
  list(APPEND PCC_RTL_SOURCES ${PCC_RTL_DIR}/${unit}.pas)
endforeach()

set(PCC_RTL_OBJECTS)
foreach(unit ${PCC_RTL_UNITS})
  add_custom_command(
    OUTPUT ${PCC_RTL_DIR}/${unit}.o
    COMMAND $<TARGET_FILE:pcc> --no-project --mode=objfpc -O2 -c
            ${PCC_RTL_DIR}/${unit}.pas -o ${PCC_RTL_DIR}/${unit}.o
    DEPENDS pcc ${PCC_RTL_SOURCES}
            $<$<TARGET_EXISTS:pascalrt-ir>:${PROJECT_BINARY_DIR}/lib/libpascalrt.ll>)
  list(APPEND PCC_RTL_OBJECTS ${PCC_RTL_DIR}/${unit}.o)
endforeach()

add_custom_target(rtl ALL DEPENDS ${PCC_RTL_SOURCES} ${PCC_RTL_OBJECTS})
