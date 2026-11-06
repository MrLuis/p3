//===- Diagnostics.h - Error / warning reporting ------------------*- C++ -*-===//
//
// All user-facing compiler messages go through DiagnosticsEngine. Output format
// (clang-style):
//
//   hello.pas:3:5: error: undeclared identifier 'x'
//       x := 1;
//       ^
//
// Diagnostics without a location print as "pcc: error: ...".
//
//===----------------------------------------------------------------------===//
#ifndef PCC_BASIC_DIAGNOSTICS_H
#define PCC_BASIC_DIAGNOSTICS_H

#include "pcc/Basic/SourceLocation.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Twine.h"

#include <string>

namespace llvm {
class raw_ostream;
}

namespace pcc {

class SourceManager;

enum class Severity { Note, Remark, Warning, Error, Fatal };

class DiagnosticsEngine {
  SourceManager &SM;
  llvm::raw_ostream &OS;
  std::string ProgName = "pcc";

  bool ShowColors = false;
  bool WarningsAsErrors = false;
  unsigned ErrorLimit = 0; ///< 0 = unlimited

  unsigned NumErrors = 0;
  unsigned NumWarnings = 0;
  bool FatalOccurred = false;
  /// Notes attach to the preceding diagnostic and are dropped with it.
  bool LastDiagSuppressed = false;

  void emit(Severity S, SourceLocation Loc, const llvm::Twine &Msg,
            llvm::ArrayRef<SourceRange> Ranges);
  void emitTooManyErrors();

public:
  DiagnosticsEngine(SourceManager &SM, llvm::raw_ostream &OS);

  void setProgName(llvm::StringRef Name) { ProgName = Name.str(); }
  void setShowColors(bool V) { ShowColors = V; }
  void setWarningsAsErrors(bool V) { WarningsAsErrors = V; }
  void setErrorLimit(unsigned N) { ErrorLimit = N; }

  void report(Severity S, SourceLocation Loc, const llvm::Twine &Msg,
              llvm::ArrayRef<SourceRange> Ranges = {});

  void error(SourceLocation Loc, const llvm::Twine &Msg,
             llvm::ArrayRef<SourceRange> Ranges = {}) {
    report(Severity::Error, Loc, Msg, Ranges);
  }
  void warning(SourceLocation Loc, const llvm::Twine &Msg,
               llvm::ArrayRef<SourceRange> Ranges = {}) {
    report(Severity::Warning, Loc, Msg, Ranges);
  }
  void note(SourceLocation Loc, const llvm::Twine &Msg,
            llvm::ArrayRef<SourceRange> Ranges = {}) {
    report(Severity::Note, Loc, Msg, Ranges);
  }
  void fatal(SourceLocation Loc, const llvm::Twine &Msg) {
    report(Severity::Fatal, Loc, Msg);
  }

  /// Location-less convenience overloads ("pcc: error: ...").
  void error(const llvm::Twine &Msg) { error(SourceLocation(), Msg); }
  void warning(const llvm::Twine &Msg) { warning(SourceLocation(), Msg); }
  void note(const llvm::Twine &Msg) { note(SourceLocation(), Msg); }
  void fatal(const llvm::Twine &Msg) { fatal(SourceLocation(), Msg); }

  unsigned getNumErrors() const { return NumErrors; }
  unsigned getNumWarnings() const { return NumWarnings; }
  bool hasErrors() const { return NumErrors != 0 || FatalOccurred; }
  /// True once a fatal error occurred or the error limit was reached; the
  /// compiler should stop as soon as practical.
  bool shouldStop() const { return FatalOccurred; }
};

} // namespace pcc

#endif // PCC_BASIC_DIAGNOSTICS_H
