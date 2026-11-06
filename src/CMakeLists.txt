configure_file(${PROJECT_SOURCE_DIR}/include/pcc/Basic/Version.h.in
               ${PROJECT_BINARY_DIR}/include/pcc/Basic/Version.h @ONLY)

add_library(pccBasic STATIC
  basic/SourceManager.cpp
  basic/Diagnostics.cpp
)
target_include_directories(pccBasic PUBLIC ${PROJECT_BINARY_DIR}/include)
target_link_libraries(pccBasic PUBLIC ${PCC_LLVM_LIBS})

add_library(pccLexer STATIC
  lexer/TokenKinds.cpp
  lexer/Lexer.cpp
)
target_link_libraries(pccLexer PUBLIC pccBasic)

add_library(pccAST STATIC
  ast/AST.cpp
  ast/Type.cpp
  ast/ASTPrinter.cpp
)
target_link_libraries(pccAST PUBLIC pccBasic)

add_library(pccParse STATIC
  parse/Parser.cpp
)
target_link_libraries(pccParse PUBLIC pccLexer pccAST)

add_library(pccSema STATIC
  sema/Sema.cpp
)
target_link_libraries(pccSema PUBLIC pccAST)

add_library(pccCodeGen STATIC
  codegen/CodeGen.cpp
  codegen/Backend.cpp
)
target_link_libraries(pccCodeGen PUBLIC pccAST)

add_library(pccDriver STATIC
  driver/Driver.cpp
  driver/Linker.cpp
)
target_link_libraries(pccDriver PUBLIC pccParse pccSema pccCodeGen)

add_executable(pcc driver/main.cpp)
target_link_libraries(pcc PRIVATE pccDriver)
set_target_properties(pcc PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
