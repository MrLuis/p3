# libpascalrt: the C runtime linked into every Pascal executable.
# Lands in <build>/lib so the compiler finds it at <bindir>/../lib.
add_library(pascalrt STATIC io.c)
target_include_directories(pascalrt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(pascalrt PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON
  ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
