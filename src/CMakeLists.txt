configure_file(${PROJECT_SOURCE_DIR}/include/p3/Basic/Version.h.in
               ${PROJECT_BINARY_DIR}/include/p3/Basic/Version.h @ONLY)

add_library(p3Basic STATIC
  basic/SourceManager.cpp
  basic/Diagnostics.cpp
)
target_include_directories(p3Basic PUBLIC ${PROJECT_BINARY_DIR}/include)
target_link_libraries(p3Basic PUBLIC ${P3_LLVM_LIBS})

add_library(p3Lexer STATIC
  lexer/TokenKinds.cpp
  lexer/Lexer.cpp
)
target_link_libraries(p3Lexer PUBLIC p3Basic)

add_library(p3AST STATIC
  ast/AST.cpp
  ast/Type.cpp
  ast/ASTPrinter.cpp
)
target_link_libraries(p3AST PUBLIC p3Basic)

add_library(p3Parse STATIC
  parse/Parser.cpp
)
target_link_libraries(p3Parse PUBLIC p3Lexer p3AST)

add_library(p3Sema STATIC
  sema/Sema.cpp
)
target_link_libraries(p3Sema PUBLIC p3AST)

add_library(p3CodeGen STATIC
  codegen/CodeGen.cpp
  codegen/Backend.cpp
)
target_link_libraries(p3CodeGen PUBLIC p3AST)

add_library(p3Driver STATIC
  driver/Driver.cpp
  driver/Compiletime.cpp
  driver/Generators.cpp
  driver/Linker.cpp
  driver/Project.cpp
)
target_link_libraries(p3Driver PUBLIC p3Parse p3Sema p3CodeGen)

add_executable(p3 driver/main.cpp)
target_link_libraries(p3 PRIVATE p3Driver)
set_target_properties(p3 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# `cmake --install`: bin/p3, with the runtime and units in ../lib, where it
# looks for them (hence these names, not GNUInstallDirs' lib64 and such).
# The installed p3 keeps the path to a shared libLLVM that is outside the
# system's search path (Homebrew's, say).
set_target_properties(p3 PROPERTIES INSTALL_RPATH_USE_LINK_PATH ON)
install(TARGETS p3 RUNTIME DESTINATION bin)
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION share/doc/p3)
