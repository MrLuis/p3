//===- CodeGen.h - AST -> LLVM IR ---------------------------------*- C++ -*-===//
#ifndef PCC_CODEGEN_CODEGEN_H
#define PCC_CODEGEN_CODEGEN_H

#include "llvm/ADT/StringRef.h"

#include <memory>

namespace llvm {
class LLVMContext;
class Module;
class TargetMachine;
} // namespace llvm

namespace pcc {

class CheckDirectives;
class ProgramDecl;
class SourceManager;
struct LangOptions;

/// Runtime checks. ISO 7185 calls these conditions "errors", so they are on
/// by default; -fno-range-checks / -fno-overflow-checks turn them off.
struct CodeGenOptions {
  /// Subrange assignment, array indices, chr/succ/pred/trunc/round results.
  bool RangeChecks = true;
  /// Integer + - * abs sqr and unary minus.
  bool OverflowChecks = true;
  /// {$R±} / {$Q±} switches found in the source (may be null).
  const CheckDirectives *Directives = nullptr;
  /// -g: emit DWARF debug information.
  bool DebugInfo = false;
  /// For the debug info's "optimized" flag.
  unsigned OptLevel = 0;
};

/// Lowers a Sema-checked program to an LLVM module for TM's target.
/// The program body becomes `int main(int argc, char **argv)`; program
/// variables become internal globals; each procedure/function becomes an
/// internal function. SM is used to put source locations into runtime error
/// messages.
std::unique_ptr<llvm::Module>
generateModule(ProgramDecl &P, const SourceManager &SM,
               const LangOptions &LangOpts, const CodeGenOptions &CGOpts,
               llvm::LLVMContext &Ctx, llvm::TargetMachine &TM,
               llvm::StringRef SourceFileName);

} // namespace pcc

#endif // PCC_CODEGEN_CODEGEN_H
