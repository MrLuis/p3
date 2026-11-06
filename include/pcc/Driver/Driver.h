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

namespace pcc {

class ASTContext;
class CheckDirectives;
struct Identifier;
class UnitDecl;

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

  void loadUnits(llvm::ArrayRef<Identifier> Uses, ASTContext &Ctx,
                 CheckDirectives &Directives,
                 llvm::SmallVectorImpl<UnitDecl *> &Ordered);

  /// --emit-tokens
  void dumpTokens(FileID FID, llvm::raw_ostream &OS);

public:
  Driver(CompilerOptions Opts, llvm::raw_ostream &DiagOS);

  /// Runs the whole pipeline. Returns the process exit code.
  int run();

  SourceManager &getSourceManager() { return SM; }
  DiagnosticsEngine &getDiagnostics() { return Diags; }
  const CompilerOptions &getOptions() const { return Opts; }
};

} // namespace pcc

#endif // PCC_DRIVER_DRIVER_H
