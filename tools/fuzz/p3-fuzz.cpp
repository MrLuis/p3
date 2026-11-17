//===- p3-fuzz.cpp - libFuzzer target for the whole front end ------------===//
//
// Feeds arbitrary bytes through lexer, parser, AST printer, Sema and (when
// Sema accepts the program) code generation + IR verification. Any crash,
// sanitizer report or invalid IR is a bug.
//
// Build (separate build directory, everything instrumented):
//   cmake -S . -B build-fuzz -G Ninja -DP3_ENABLE_FUZZING=ON \
//     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
//     -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined"
//   ninja -C build-fuzz p3-fuzz
// Run:
//   build-fuzz/bin/p3-fuzz -max_total_time=300 corpus/ tests/*/Inputs tests/run
//
//===----------------------------------------------------------------------===//

#include "p3/AST/ASTContext.h"
#include "p3/AST/ASTPrinter.h"
#include "p3/Basic/Diagnostics.h"
#include "p3/Basic/SourceManager.h"
#include "p3/Basic/StringPool.h"
#include "p3/CodeGen/Backend.h"
#include "p3/CodeGen/CodeGen.h"
#include "p3/Lexer/Lexer.h"
#include "p3/Parse/Parser.h"
#include "p3/Sema/Sema.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

#include <cstdint>
#include <cstdlib>

using namespace p3;

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

  // The first byte picks the dialect, so all get coverage: ISO when even,
  // else Turbo/FPC, with classes (objfpc) when bit 1 is set too.
  LangMode Mode = LangMode::ISO;
  if (Size && (Data[0] & 1))
    Mode = (Data[0] & 2) ? LangMode::ObjFPC : LangMode::TP;
  LangOptions LO = LangOptions::forMode(Mode);
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
  auto M = generateModule(*Program, {}, SM, LO, CGOpts, LLVMCtx, targetMachine(),
                          "fuzz.pas");
  if (llvm::verifyModule(*M, &llvm::errs())) {
    llvm::errs() << "invalid IR for input:\n"
                 << llvm::StringRef(reinterpret_cast<const char *>(Data), Size)
                 << '\n';
    std::abort();
  }
  return 0;
}
