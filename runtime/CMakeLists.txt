# libpascalrt: the C runtime linked into every Pascal executable.
# Lands in <build>/lib so the compiler finds it at <bindir>/../lib.
add_library(pascalrt STATIC io.c sys.c ansi.c format.c)
target_include_directories(pascalrt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# DWARF 4 like pcc's own output: GNU ld mis-merges mixed DWARF 4/5 objects.
target_compile_options(pascalrt PRIVATE $<$<CONFIG:Debug>:-gdwarf-4>)
set_target_properties(pascalrt PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON
  ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
