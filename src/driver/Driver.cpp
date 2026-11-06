#include "pcc/Driver/Driver.h"
#include "pcc/AST/ASTContext.h"
#include "pcc/AST/ASTPrinter.h"
#include "pcc/CodeGen/Backend.h"
#include "pcc/CodeGen/CodeGen.h"
#include "pcc/Driver/Linker.h"
#include "pcc/Lexer/Lexer.h"
#include "pcc/Parse/Parser.h"
#include "pcc/Sema/Sema.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Target/TargetMachine.h"

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Path.h"
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
    : Opts(std::move(O)), LangOpts(LangOptions::forMode(Opts.Mode)),
      Diags(SM, DiagOS) {
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
  initializeAllLLVMTargets();
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

  FileID FID = *FIDOrErr;
  bool OK = runPipeline(FID);
  return OK && !Diags.hasErrors() ? 0 : 1;
}

/// Opens Opts.OutputFile for a text dump, reporting failures.
static std::unique_ptr<llvm::raw_fd_ostream>
openTextOutput(llvm::StringRef Path, DiagnosticsEngine &Diags) {
  std::error_code EC;
  auto OS = std::make_unique<llvm::raw_fd_ostream>(Path, EC,
                                                   llvm::sys::fs::OF_Text);
  if (EC) {
    Diags.error("cannot open output file '" + Path + "': " + EC.message());
    return nullptr;
  }
  return OS;
}

bool Driver::runPipeline(FileID FID) {
  // --- M1: lexing ---------------------------------------------------------
  if (Opts.Act == Action::EmitTokens) {
    auto OS = openTextOutput(Opts.OutputFile, Diags);
    if (!OS)
      return false;
    dumpTokens(FID, *OS);
    return true;
  }

  // --- M2: parsing --------------------------------------------------------
  ASTContext Ctx(Strings);
  Lexer Lex(SM, FID, Diags, Strings, LangOpts);
  Parser P(Lex, Diags, Ctx);
  ProgramDecl *Program = P.parseProgram();

  // --- M4: semantic analysis (skipped after syntax errors) ---------------
  bool SyntaxOK = !Diags.hasErrors();
  bool SemaOK = SyntaxOK && Sema(Ctx, Diags, LangOpts).check(*Program);

  if (Opts.Act == Action::EmitAST) {
    // Dump even after errors: a partial tree helps debugging. Types are
    // shown when Sema ran.
    auto OS = openTextOutput(Opts.OutputFile, Diags);
    if (!OS)
      return false;
    printAST(*Program, SM, *OS);
    return true;
  }
  if (!SemaOK)
    return false;

  // --- M3: code generation ------------------------------------------------
  if (Opts.DebugInfo)
    Diags.warning("debug information (-g) is not implemented yet; ignored");

  bool IsHost = llvm::Triple(Opts.TargetTriple) ==
                llvm::Triple(llvm::sys::getDefaultTargetTriple());
  if (Opts.Act == Action::EmitExe && !IsHost) {
    Diags.error("linking for a non-host target ('" +
                llvm::Twine(Opts.TargetTriple) +
                "') is not supported yet; use -c, -S or --emit-llvm");
    return false;
  }

  auto TM = createTargetMachine(Opts.TargetTriple, Opts.OptLevel, Diags);
  if (!TM)
    return false;

  llvm::LLVMContext LLVMCtx;
  CodeGenOptions CGOpts;
  CGOpts.RangeChecks = Opts.RangeChecks;
  CGOpts.OverflowChecks = Opts.OverflowChecks;
  auto M = generateModule(*Program, SM, LangOpts, CGOpts, LLVMCtx, *TM,
                          SM.getFilename(FID));
  if (!verifyModule(*M, Diags))
    return false;
  optimizeModule(*M, *TM, Opts.OptLevel);

  switch (Opts.Act) {
  case Action::EmitLLVM:
    return emitOutput(*M, *TM, OutputKind::LLVMIR, Opts.OutputFile, Diags);
  case Action::EmitAsm:
    return emitOutput(*M, *TM, OutputKind::Assembly, Opts.OutputFile, Diags);
  case Action::EmitObj:
    return emitOutput(*M, *TM, OutputKind::Object, Opts.OutputFile, Diags);
  case Action::EmitExe:
    break;
  default:
    llvm_unreachable("dump actions handled above");
  }

  // Executable: object file in a temp dir, then link.
  llvm::SmallString<128> ObjPath;
  if (std::error_code EC =
          llvm::sys::fs::createTemporaryFile("pcc", "o", ObjPath)) {
    Diags.error("cannot create temporary file: " + EC.message());
    return false;
  }
  llvm::FileRemover RemoveObj(ObjPath);
  if (!emitOutput(*M, *TM, OutputKind::Object, ObjPath, Diags))
    return false;
  return linkExecutable(ObjPath, Opts.OutputFile, Diags);
}

void Driver::dumpTokens(FileID FID, llvm::raw_ostream &OS) {
  // One token per line:  <line>:<col> <kind> '<spelling>' [value]
  Lexer L(SM, FID, Diags, Strings, LangOpts);
  Token T;
  do {
    L.lex(T);
    PresumedLoc P = SM.getPresumedLoc(T.getLocation());
    OS << P.Line << ':' << P.Column << ' ' << tok::getTokenName(T.getKind())
       << " '" << T.getRawText() << '\'';
    switch (T.getKind()) {
    case tok::identifier:
      OS << " name=" << T.getIdentifier();
      break;
    case tok::integer_literal:
      OS << " value=" << T.getIntegerValue();
      break;
    case tok::real_literal:
      OS << " value=" << llvm::format("%g", T.getRealValue());
      break;
    case tok::string_literal:
      OS << " value=\"";
      OS.write_escaped(T.getStringValue());
      OS << '"';
      break;
    default:
      break;
    }
    OS << '\n';
  } while (T.isNot(tok::eof) && !Diags.shouldStop());
}
