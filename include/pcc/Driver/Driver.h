//===- Driver.h - Runs the compilation pipeline -------------------*- C++ -*-===//
#ifndef PCC_DRIVER_DRIVER_H
#define PCC_DRIVER_DRIVER_H

#include "pcc/Basic/Diagnostics.h"
#include "pcc/Basic/SourceManager.h"
#include "pcc/Driver/CompilerOptions.h"

namespace llvm {
class raw_ostream;
}

namespace pcc {

class Driver {
  CompilerOptions Opts;
  SourceManager SM;
  DiagnosticsEngine Diags;

  bool validateOptions();
  bool validateTarget();

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
