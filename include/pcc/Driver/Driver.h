//===- Driver.h - Runs the compilation pipeline -------------------*- C++ -*-===//
#ifndef PCC_DRIVER_DRIVER_H
#define PCC_DRIVER_DRIVER_H

#include "pcc/Basic/Diagnostics.h"
#include "pcc/Basic/LangOptions.h"
#include "pcc/Basic/SourceManager.h"
#include "pcc/Basic/StringPool.h"
#include "pcc/Driver/CompilerOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {
class raw_ostream;
}

namespace llvm {
class Module;
class TargetMachine;
} // namespace llvm

namespace pcc {

class Parser;

class ASTContext;
class CheckDirectives;
struct Identifier;
class UnitDecl;
class Block;

class Driver {
  CompilerOptions Opts;
  LangOptions LangOpts;
  SourceManager SM;
  DiagnosticsEngine Diags;
  StringPool Strings;

  bool validateOptions();
  bool validateTarget();

  /// lex -> parse -> sema -> codegen -> emit/link, stopping at Opts.Act.
  bool runPipeline(FileID FID);

  /// Self: the unit being compiled, if any (using it is circular).
  void loadUnits(llvm::ArrayRef<Identifier> Uses, ASTContext &Ctx,
                 CheckDirectives &Directives,
                 llvm::SmallVectorImpl<UnitDecl *> &Ordered,
                 llvm::StringRef Self = "");

  /// The libraries named by the external routines of the program and its
  /// units (`external 'z'`), linked after the -l ones: -lz, or -l:name for
  /// a file name ('libz.so.1').
  std::vector<std::string> ExternalLibraries;
  void collectExternalLibraries(Block *B);

  /// Verifies, optimizes and writes M (or links it with UnitObjects).
  bool finishModule(llvm::Module &M, llvm::TargetMachine &TM,
                    llvm::ArrayRef<std::string> UnitObjects);
  /// The pipeline for a unit file: its own module (S11).
  bool compileUnit(Parser &P, FileID FID, ASTContext &Ctx,
                   CheckDirectives &Directives);

  /// --emit-tokens
  void dumpTokens(FileID FID, llvm::raw_ostream &OS);

public:
  Driver(CompilerOptions Opts, llvm::raw_ostream &DiagOS);
  /// Applies Opts' language mode and diagnostic settings.
  void configure();

  /// Runs the whole pipeline. Returns the process exit code.
  int run();

  SourceManager &getSourceManager() { return SM; }
  DiagnosticsEngine &getDiagnostics() { return Diags; }
  const CompilerOptions &getOptions() const { return Opts; }
};

} // namespace pcc

#endif // PCC_DRIVER_DRIVER_H
