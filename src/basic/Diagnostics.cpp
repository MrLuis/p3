// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "p3/Basic/Diagnostics.h"
#include "p3/Basic/SourceManager.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

using namespace p3;

DiagnosticsEngine::DiagnosticsEngine(SourceManager &SM, llvm::raw_ostream &OS)
    : SM(SM), OS(OS) {}

bool DiagnosticsEngine::isLibraryLocation(SourceLocation L) const {
  if (!L.isValid())
    return false;
  PresumedLoc P = SM.getPresumedLoc(L);
  if (!P.isValid())
    return false;
  return P.Filename.contains("/lib/pascal/") ||
         P.Filename.contains("\\lib\\pascal\\");
}

void DiagnosticsEngine::report(Severity S, SourceLocation Loc,
                               const llvm::Twine &Msg,
                               llvm::ArrayRef<SourceRange> Ranges) {
  // Once we stopped, stay quiet: the "too many errors" / fatal message was the
  // last thing the user should see.
  if (FatalOccurred) {
    LastDiagSuppressed = true;
    return;
  }

  if (S == Severity::Note) {
    if (!LastDiagSuppressed)
      emit(S, Loc, Msg, Ranges);
    return;
  }

  if (S == Severity::Warning) {
    std::string Key;
    if (PresumedLoc P = SM.getPresumedLoc(Loc); P.isValid())
      Key = (P.Filename + ":" + llvm::Twine(P.Line) + ":" +
             llvm::Twine(P.Column) + ":")
                .str();
    Key += Msg.str();
    if (!WarningsSeen.insert(Key).second && SkipRepeatedWarnings) {
      LastDiagSuppressed = true;
      return;
    }
  }
  if (S == Severity::Warning && WarningsAsErrors)
    S = Severity::Error;

  LastDiagSuppressed = false;
  switch (S) {
  case Severity::Note:
  case Severity::Remark:
    break;
  case Severity::Warning:
    ++NumWarnings;
    break;
  case Severity::Error:
    if (ErrorLimit && NumErrors >= ErrorLimit) {
      emitTooManyErrors();
      LastDiagSuppressed = true;
      return;
    }
    ++NumErrors;
    break;
  case Severity::Fatal:
    ++NumErrors;
    FatalOccurred = true;
    break;
  }
  emit(S, Loc, Msg, Ranges);
}

void DiagnosticsEngine::emit(Severity S, SourceLocation Loc,
                             const llvm::Twine &Msg,
                             llvm::ArrayRef<SourceRange> Ranges) {
  // Fatal errors render as plain errors with a "fatal error:" prefix; the
  // other severities map 1:1 onto llvm::SourceMgr's kinds.
  if (S == Severity::Fatal) {
    if (PresumedLoc P = SM.getPresumedLoc(Loc); P.isValid())
      OS << P.Filename << ':' << P.Line << ':' << P.Column << ": ";
    else
      OS << ProgName << ": ";
    llvm::WithColor(OS, llvm::HighlightColor::Error,
                    ShowColors ? llvm::ColorMode::Enable
                               : llvm::ColorMode::Disable)
        << "fatal error: ";
    OS << Msg << '\n';
    OS.flush();
    return;
  }

  llvm::SourceMgr::DiagKind Kind = llvm::SourceMgr::DK_Error;
  switch (S) {
  case Severity::Note:    Kind = llvm::SourceMgr::DK_Note; break;
  case Severity::Remark:  Kind = llvm::SourceMgr::DK_Remark; break;
  case Severity::Warning: Kind = llvm::SourceMgr::DK_Warning; break;
  default:                Kind = llvm::SourceMgr::DK_Error; break;
  }

  llvm::SmallVector<llvm::SMRange, 4> SMRanges;
  for (const SourceRange &R : Ranges)
    if (R.isValid())
      SMRanges.push_back(R.getSMRange());

  if (!Loc.isValid()) {
    // "p3: error: msg" -- no file, line or source excerpt.
    llvm::SMDiagnostic D(/*filename=*/"", Kind, Msg.str());
    D.print(ProgName.c_str(), OS, ShowColors);
  } else {
    llvm::SMDiagnostic D =
        SM.getLLVMSourceMgr().GetMessage(Loc.getSMLoc(), Kind, Msg, SMRanges);
    D.print(/*ProgName=*/"", OS, ShowColors);
  }
  OS.flush();
}

void DiagnosticsEngine::emitTooManyErrors() {
  FatalOccurred = true;
  emit(Severity::Fatal, SourceLocation(),
       "too many errors emitted, stopping now [-ferror-limit=]", {});
}
