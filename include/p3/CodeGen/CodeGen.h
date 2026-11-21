//===- CodeGen.h - AST -> LLVM IR ---------------------------------*- C++ -*-===//
#ifndef P3_CODEGEN_CODEGEN_H
#define P3_CODEGEN_CODEGEN_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>

namespace llvm {
class LLVMContext;
class Module;
class TargetMachine;
} // namespace llvm

namespace p3 {

class CheckDirectives;
class ProgramDecl;
class UnitDecl;
class SourceManager;
struct LangOptions;

/// Runtime checks. ISO 7185 calls these conditions "errors", so they are on
/// by default; -fno-range-checks / -fno-overflow-checks turn them off.
struct CodeGenOptions {
  /// Subrange assignment, array indices, chr/succ/pred/trunc/round results.
  bool RangeChecks = true;
  /// Integer + - * abs sqr and unary minus.
  bool OverflowChecks = true;
  /// -ffast-math: set the fast-math flags on floating-point instructions.
  bool FastMath = false;
  /// The target CPU and features (-mcpu / -mattr); the functions' target
  /// attributes carry them into the IR.
  std::string TargetCPU = "generic", TargetFeatures;
  /// {$R±} / {$Q±} switches found in the source (may be null).
  const CheckDirectives *Directives = nullptr;
  /// -g: emit DWARF debug information.
  bool DebugInfo = false;
  /// For the debug info's "optimized" flag.
  unsigned OptLevel = 0;
  /// -Os (1) / -Oz (2): optsize/minsize function attributes.
  unsigned SizeLevel = 0;
  /// Local arrays, records and strings start zeroed (-fzero-init; the
  /// default at -O0). Scalars, and what holds managed values or files,
  /// always do.
  bool ZeroInit = true;
  /// Keep the internal routines and data that nothing reachable refers to
  /// (-fkeep-unused); otherwise they are generated and then removed from
  /// the module (see removeUnreachable in CodeGen.cpp).
  bool KeepUnused = false;
  /// The units were compiled separately (each to its own object): their
  /// interface variables and routines are external symbols
  /// (pas.<unit>$<name>), and main calls their initialization functions
  /// (pas.<unit>$$init) instead of containing their code.
  bool SeparateUnits = false;
  /// -fprofile-instr-generate[=<file>]: the counters go in, and the
  /// profile runtime is linked (a nonempty value is the profraw path).
  std::string ProfileInstrGenerate;
  /// -fprofile-instr-use=<file>: optimize with the profile.
  std::string ProfileInstrUse;
};

/// Lowers a Sema-checked program to an LLVM module for TM's target.
/// The program body becomes `int main(int argc, char **argv)`; program
/// variables become internal globals; each procedure/function becomes an
/// internal function. SM is used to put source locations into runtime error
/// messages.
std::unique_ptr<llvm::Module>
generateModule(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units,
               const SourceManager &SM,
               const LangOptions &LangOpts, const CodeGenOptions &CGOpts,
               llvm::LLVMContext &Ctx, llvm::TargetMachine &TM,
               llvm::StringRef SourceFileName);

/// Lowers unit U, compiled on its own, to a module: its interface
/// variables and routines are defined as external symbols
/// (pas.<unit>$<name>), its initialization part becomes the function
/// pas.<unit>$$init. Deps are the units it uses (dependency order), of which
/// only the interface symbols are declared.
std::unique_ptr<llvm::Module>
generateUnitModule(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps,
                   const SourceManager &SM, const LangOptions &LangOpts,
                   const CodeGenOptions &CGOpts, llvm::LLVMContext &Ctx,
                   llvm::TargetMachine &TM, llvm::StringRef SourceFileName);

} // namespace p3

#endif // P3_CODEGEN_CODEGEN_H
