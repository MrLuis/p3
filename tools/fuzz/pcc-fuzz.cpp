//===- pcc-fuzz.cpp - libFuzzer target for the whole front end ------------===//
//
// Feeds arbitrary bytes through lexer, parser, AST printer, Sema and (when
// Sema accepts the program) code generation + IR verification. Any crash,
// sanitizer report or invalid IR is a bug.
//
// Build (separate build directory, everything instrumented):
//   cmake -S . -B build-fuzz -G Ninja -DPCC_ENABLE_FUZZING=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
//     -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined"
//   ninja -C build-fuzz pcc-fuzz
// Run:
//   build-fuzz/bin/pcc-fuzz -max_total_time=300 corpus/ tests/*/Inputs tests/run
//
//===----------------------------------------------------------------------===//

#include "pcc/AST/ASTContext.h"
#include "pcc/AST/ASTPrinter.h"
#include "pcc/Basic/Diagnostics.h"
#include "pcc/Basic/SourceManager.h"
#include "pcc/Basic/StringPool.h"
#include "pcc/CodeGen/Backend.h"
#include "pcc/CodeGen/CodeGen.h"
#include "pcc/Lexer/Lexer.h"
#include "pcc/Parse/Parser.h"
#include "pcc/Sema/Sema.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

#include <cstdint>
#include <cstdlib>

using namespace pcc;

static llvm::TargetMachine &targetMachine() {
  static std::unique_ptr<llvm::TargetMachine> TM = [] {
    initializeAllLLVMTargets();
    SourceManager SM;
    DiagnosticsEngine Diags(SM, llvm::errs());
    return createTargetMachine(llvm::sys::getDefaultTargetTriple(), 0, Diags);
  }();
  return *TM;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  SourceManager SM;
  DiagnosticsEngine Diags(SM, llvm::nulls());
  Diags.setErrorLimit(50);
  FileID FID = SM.addBuffer(
      llvm::StringRef(reinterpret_cast<const char *>(Data), Size), "fuzz.pas");

  // The first byte picks the dialect, so both get coverage.
  LangOptions LO = LangOptions::forMode(Size && (Data[0] & 1) ? LangMode::TP
                                                              : LangMode::ISO);
  StringPool Strings;
  ASTContext Ctx(Strings);
  Lexer Lex(SM, FID, Diags, Strings, LO);
  Parser P(Lex, Diags, Ctx);
  ProgramDecl *Program = P.parseProgram();
  printAST(*Program, SM, llvm::nulls());
  if (Diags.hasErrors() || !Sema(Ctx, Diags, LO).check(*Program))
    return 0;

  llvm::LLVMContext LLVMCtx;
  CodeGenOptions CGOpts;
  auto M = generateModule(*Program, SM, LO, CGOpts, LLVMCtx, targetMachine(),
                          "fuzz.pas");
  if (llvm::verifyModule(*M, &llvm::errs())) {
    llvm::errs() << "invalid IR for input:\n"
                 << llvm::StringRef(reinterpret_cast<const char *>(Data), Size)
                 << '\n';
    std::abort();
  }
  return 0;
}
