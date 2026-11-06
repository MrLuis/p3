#include "pcc/Driver/Driver.h"

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using namespace pcc;

//===----------------------------------------------------------------------===//
// CompilerOptions helpers
//===----------------------------------------------------------------------===//

const char *pcc::getActionName(Action A) {
  switch (A) {
  case Action::EmitTokens: return "--emit-tokens";
  case Action::EmitAST:    return "--emit-ast";
  case Action::EmitLLVM:   return "--emit-llvm";
  case Action::EmitAsm:    return "-S";
  case Action::EmitObj:    return "-c";
  case Action::EmitExe:    return "executable generation";
  }
  return "?";
}

std::string pcc::getDefaultOutputFile(const CompilerOptions &Opts) {
  if (Opts.Act == Action::EmitTokens || Opts.Act == Action::EmitAST)
    return "-";

  llvm::StringRef Stem = llvm::sys::path::stem(Opts.InputFile);
  if (Stem.empty() || Opts.InputFile == "-")
    Stem = "a";

  switch (Opts.Act) {
  case Action::EmitLLVM: return (Stem + ".ll").str();
  case Action::EmitAsm:  return (Stem + ".s").str();
  case Action::EmitObj:  return (Stem + ".o").str();
  case Action::EmitExe:
    return Opts.InputFile == "-" ? std::string("a.out") : Stem.str();
  default:
    return "-";
  }
}

//===----------------------------------------------------------------------===//
// Driver
//===----------------------------------------------------------------------===//

Driver::Driver(CompilerOptions O, llvm::raw_ostream &DiagOS)
    : Opts(std::move(O)), Diags(SM, DiagOS) {
  Diags.setErrorLimit(Opts.ErrorLimit);
  Diags.setWarningsAsErrors(Opts.WarningsAsErrors);
  switch (Opts.Color) {
  case ColorMode::Always: Diags.setShowColors(true); break;
  case ColorMode::Never:  Diags.setShowColors(false); break;
  case ColorMode::Auto:   Diags.setShowColors(DiagOS.has_colors()); break;
  }
  if (Opts.OutputFile.empty())
    Opts.OutputFile = getDefaultOutputFile(Opts);
}

bool Driver::validateOptions() {
  if (Opts.InputFile.empty()) {
    Diags.error("no input file");
    return false;
  }
  if (Opts.OptLevel > 3) {
    Diags.error("invalid optimization level '-O" +
                llvm::Twine(Opts.OptLevel) + "' (expected 0-3)");
    return false;
  }
  return validateTarget();
}

bool Driver::validateTarget() {
  llvm::InitializeAllTargetInfos();
  std::string TripleStr = Opts.TargetTriple.empty()
                              ? llvm::sys::getDefaultTargetTriple()
                              : llvm::Triple::normalize(Opts.TargetTriple);
  std::string Err;
  if (!llvm::TargetRegistry::lookupTarget(TripleStr, Err)) {
    Diags.error("unsupported target '" + llvm::Twine(TripleStr) + "': " + Err);
    return false;
  }
  Opts.TargetTriple = TripleStr;
  return true;
}

int Driver::run() {
  if (!validateOptions())
    return 1;

  auto FIDOrErr = SM.loadFile(Opts.InputFile);
  if (!FIDOrErr) {
    Diags.error("cannot open input file '" + llvm::Twine(Opts.InputFile) +
                "': " + FIDOrErr.getError().message());
    return 1;
  }

  // Pipeline stages are added milestone by milestone:
  //   M1 lexer -> M2 parser/AST -> M4 sema -> M3 codegen/object/link.
  Diags.error(llvm::Twine(getActionName(Opts.Act)) +
              " is not implemented yet");
  return Diags.hasErrors() ? 1 : 0;
}
