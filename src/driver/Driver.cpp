#include "pcc/Driver/Driver.h"
#include "pcc/AST/ASTContext.h"
#include "pcc/AST/ASTPrinter.h"
#include "pcc/Basic/Directives.h"
#include "pcc/CodeGen/Backend.h"
#include "pcc/CodeGen/CodeGen.h"
#include "pcc/Driver/Linker.h"
#include "pcc/Driver/Project.h"
#include "pcc/Lexer/Lexer.h"
#include "pcc/Parse/Parser.h"
#include "pcc/Sema/Sema.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"

#include <functional>
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
  case Action::EmitParseTree: return "--emit-parse-tree";
  case Action::EmitLLVM:   return "--emit-llvm";
  case Action::EmitAsm:    return "-S";
  case Action::EmitObj:    return "-c";
  case Action::EmitExe:    return "executable generation";
  }
  return "?";
}

std::string pcc::getDefaultOutputFile(const CompilerOptions &Opts) {
  if (Opts.Act == Action::EmitTokens || Opts.Act == Action::EmitAST ||
      Opts.Act == Action::EmitParseTree)
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
  switch (Opts.Color) {
  case ColorMode::Always: Diags.setShowColors(true); break;
  case ColorMode::Never:  Diags.setShowColors(false); break;
  case ColorMode::Auto:   Diags.setShowColors(DiagOS.has_colors()); break;
  }
  configure();
}

void Driver::configure() {
  LangOpts = LangOptions::forMode(Opts.Mode);
  Diags.setErrorLimit(Opts.ErrorLimit);
  Diags.setWarningsAsErrors(Opts.WarningsAsErrors);
  if (!Opts.WarnUnusedVariables)
    Diags.disableWarning("unused-variable");
  if (!Opts.WarnDeprecated)
    Diags.disableWarning("deprecated");
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

/// The symbols Free Pascal predefines for the targets pcc's library supports
/// (Linux, macOS and Windows; x86-64 and AArch64).
static void addTargetDefines(const llvm::Triple &T,
                             std::vector<std::string> &Defines) {
  if (T.isOSLinux())
    Defines.insert(Defines.end(), {"LINUX", "UNIX"});
  else if (T.isMacOSX())
    Defines.insert(Defines.end(), {"DARWIN", "BSD", "UNIX"});
  else if (T.isOSWindows()) {
    Defines.insert(Defines.end(), {"WINDOWS", "MSWINDOWS"});
    if (T.isArch64Bit())
      Defines.push_back("WIN64");
  }
  if (T.getArch() == llvm::Triple::x86_64)
    Defines.insert(Defines.end(), {"CPUX86_64", "CPUAMD64", "CPU64"});
  else if (T.getArch() == llvm::Triple::aarch64)
    Defines.insert(Defines.end(), {"CPUAARCH64", "CPU64"});
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
  addTargetDefines(llvm::Triple(TripleStr), Opts.Defines);
  return true;
}

int Driver::run() {
  // The project file's settings (pcc.json), under the command line's.
  if (!applyProjectFile(Opts, SM, Diags))
    return 1;
  configure();
  if (Opts.OutputFile.empty())
    Opts.OutputFile = getDefaultOutputFile(Opts);
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
  CheckDirectives Directives;
  for (const std::string &N : Sema(Ctx, Diags, LangOpts).standardNames())
    PredeclaredNames.insert(N);
  Lexer Lex(SM, FID, Diags, Strings, LangOpts);
  prepareLexer(Lex, Directives);
  Parser P(Lex, Diags, Ctx);
  prepareParser(P, Ctx, Directives);
  if (P.startsWithUnit())
    return compileUnit(P, FID, Ctx, Directives);
  ProgramDecl *Program = P.parseProgram();
  llvm::SmallVector<UnitDecl *, 4> Units;
  if (!Diags.hasErrors())
    loadUnits(Program->getUses(), Ctx, Directives, Units);

  if (Opts.Act == Action::EmitParseTree) { // the parser alone (S5)
    auto OS = openTextOutput(Opts.OutputFile, Diags);
    if (!OS)
      return false;
    printParseTree(*Program, Units, SM, *OS);
    return true;
  }

  // --- M4: semantic analysis (skipped after syntax errors) ---------------
  bool SyntaxOK = !Diags.hasErrors();
  auto Reparse = [&](const RoutineDecl *G) {
    return Parser(Lex, Diags, Ctx).reparseGeneric(G);
  };
  bool SemaOK = SyntaxOK && !Diags.hasErrors() &&
                Sema(Ctx, Diags, LangOpts, Reparse)
                    .check(*Program, Units, Opts.SeparateUnits);

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
  CGOpts.Directives = &Directives;
  CGOpts.DebugInfo = Opts.DebugInfo;
  CGOpts.OptLevel = Opts.OptLevel;
  CGOpts.ZeroInit = Opts.ZeroInit.value_or(Opts.OptLevel == 0);
  CGOpts.KeepUnused = Opts.KeepUnused;
  CGOpts.SeparateUnits = Opts.SeparateUnits;

  // Separately compiled units: their objects, next to their sources.
  std::vector<std::string> UnitObjects;
  if (Opts.SeparateUnits && Opts.Act == Action::EmitExe)
    for (UnitDecl *U : Units) {
      llvm::SmallString<256> Obj(
          SM.getPresumedLoc(U->getLocation()).Filename);
      llvm::sys::path::replace_extension(Obj, "o");
      if (!llvm::sys::fs::exists(Obj)) {
        Diags.error("cannot find '" + Obj + "', the object file of unit '" +
                    U->getName().Spelling + "'; compile the unit with -c");
        return false;
      }
      UnitObjects.push_back(std::string(Obj));
    }

  for (UnitDecl *U : Units) {
    collectExternalLibraries(U->getInterface());
    collectExternalLibraries(U->getImplementation());
  }
  collectExternalLibraries(Program->getBlock());

  auto M = generateModule(*Program, Units, SM, LangOpts, CGOpts, LLVMCtx, *TM,
                          SM.getFilename(FID));
  return finishModule(*M, *TM, UnitObjects);
}

void Driver::collectExternalLibraries(Block *B) {
  if (!B)
    return;
  for (Decl *D : B->getDecls()) {
    llvm::StringRef L;
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isExternal())
      L = R->getLibraryName();
    else if (auto *V = llvm::dyn_cast<VarDecl>(D); V && V->isExternal())
      L = V->getLibraryName();
    // StdCLib (classic Mac OS's C library, named by Free Pascal's sources):
    // the C library, linked anyway.
    if (L.empty() || L.equals_insensitive("stdclib"))
      continue;
    std::string Lib = L.contains('.') || L.contains('/') ? ":" + L.str() : L.str();
    if (!llvm::is_contained(ExternalLibraries, Lib))
      ExternalLibraries.push_back(Lib);
  }
}

bool Driver::compileUnit(Parser &P, FileID FID, ASTContext &Ctx,
                         CheckDirectives &Directives) {
  if (Opts.Act == Action::EmitParseTree || Opts.Act == Action::EmitAST) {
    Diags.error("'" + llvm::Twine(Opts.InputFile) +
                "' is a unit; --emit-parse-tree and --emit-ast need a program");
    return false;
  }
  UnitDecl *Unit = P.parseUnit();
  if (!Unit || Diags.hasErrors())
    return false;
  llvm::SmallVector<Identifier, 8> Uses(Unit->getInterfaceUses());
  Uses.append(Unit->getImplementationUses().begin(),
              Unit->getImplementationUses().end());
  llvm::SmallVector<UnitDecl *, 4> Deps;
  loadUnits(Uses, Ctx, Directives, Deps, Unit->getName().Name);
  auto Reparse = [&](const RoutineDecl *G) {
    return Parser(P.getLexer(), Diags, Ctx).reparseGeneric(G);
  };
  if (Diags.hasErrors() ||
      !Sema(Ctx, Diags, LangOpts, Reparse).checkUnit(*Unit, Deps))
    return false;
  if (Opts.Act == Action::EmitExe) {
    Diags.error("'" + llvm::Twine(Opts.InputFile) +
                "' is a unit: compile it with -c, -S or --emit-llvm and link "
                "it into a program built with -fseparate-units");
    return false;
  }
  auto TM = createTargetMachine(Opts.TargetTriple, Opts.OptLevel, Diags);
  if (!TM)
    return false;
  llvm::LLVMContext LLVMCtx;
  CodeGenOptions CGOpts;
  CGOpts.RangeChecks = Opts.RangeChecks;
  CGOpts.OverflowChecks = Opts.OverflowChecks;
  CGOpts.Directives = &Directives;
  CGOpts.DebugInfo = Opts.DebugInfo;
  CGOpts.OptLevel = Opts.OptLevel;
  CGOpts.ZeroInit = Opts.ZeroInit.value_or(Opts.OptLevel == 0);
  CGOpts.KeepUnused = Opts.KeepUnused;
  CGOpts.SeparateUnits = true;
  auto M = generateUnitModule(*Unit, Deps, SM, LangOpts, CGOpts, LLVMCtx, *TM,
                              SM.getFilename(FID));
  return finishModule(*M, *TM, {});
}

bool Driver::finishModule(llvm::Module &Mod, llvm::TargetMachine &TMRef,
                          llvm::ArrayRef<std::string> UnitObjects) {
  llvm::Module *M = &Mod;
  llvm::TargetMachine *TM = &TMRef;
  if (!verifyModule(*M, Diags))
    return false;
  // At -O0 the module stays as generated: LLVM's O0 pipeline would only
  // run target-registered passes (AArch64 adds a loop pass whose
  // LoopSimplify reshapes loops), making -O0 IR differ between targets
  // and from the self-hosted compiler's (SELFHOST.md S10).
  if (Opts.OptLevel > 0) {
    // The runtime's IR is built for the host.
    if (Opts.InlineRuntime &&
        llvm::Triple(Opts.TargetTriple) ==
            llvm::Triple(llvm::sys::getDefaultTargetTriple()))
      linkRuntimeIR(*M, findRuntimeIR(), Diags);
    optimizeModule(*M, *TM, Opts.OptLevel);
  }

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
  std::vector<std::string> Libraries = Opts.Libraries;
  Libraries.insert(Libraries.end(), ExternalLibraries.begin(),
                   ExternalLibraries.end());
  return linkExecutable(ObjPath, UnitObjects, Opts.OutputFile, Opts.LibraryDirs,
                        Libraries, Diags);
}

Driver::~Driver() = default;

void Driver::prepareLexer(Lexer &L, CheckDirectives &Directives) {
  L.setDirectives(&Directives);
  L.setIncludeSupport(SM, Opts.IncludeDirs);
  L.setDefines(Opts.Defines);
  L.setPredeclared(&PredeclaredNames);
}

void Driver::prepareParser(Parser &P, ASTContext &Ctx,
                           CheckDirectives &Directives) {
  // declared(NAME) after a uses clause: the units' names and those of their
  // interfaces (an enumeration's values too).
  P.enableDeclared([this, &Ctx, &Directives](llvm::ArrayRef<Identifier> Uses,
                                             llvm::StringRef Self) {
    llvm::SmallVector<UnitDecl *, 4> Loaded;
    loadUnits(Uses, Ctx, Directives, Loaded, Self);
    std::vector<std::string> Names;
    for (const Identifier &U : Uses) {
      Names.push_back(U.Name.str());
      UnitDecl *Unit = UnitsDone.lookup(U.Name);
      if (!Unit)
        continue;
      for (Decl *D : Unit->getInterface()->getDecls()) {
        Names.push_back(D->getName().Name.str());
        if (auto *TD = llvm::dyn_cast<TypeDecl>(D))
          if (auto *ES = llvm::dyn_cast_or_null<EnumTypeSpec>(TD->getTypeSpec()))
            for (const Identifier &V : ES->getNames())
              Names.push_back(V.Name.str());
      }
    }
    return Names;
  });
}

/// Finds, parses and orders (dependencies first) the units named in Uses.
void Driver::loadUnits(llvm::ArrayRef<Identifier> Uses, ASTContext &Ctx,
                       CheckDirectives &Directives,
                       llvm::SmallVectorImpl<UnitDecl *> &Ordered,
                       llvm::StringRef Self) {
  if (!Self.empty())
    UnitsInProgress.insert(Self);
  if (!Uses.empty() && LangOpts.Mode == LangMode::ISO) {
    Diags.error(Uses[0].Loc, "units are a Turbo/Free Pascal extension; use --mode=tp");
    return;
  }
  for (const Identifier &U : Uses)
    visitUnit(U, Ctx, Directives);
  Ordered.assign(UnitsOrdered.begin(), UnitsOrdered.end());
}

void Driver::visitUnit(const Identifier &U, ASTContext &Ctx,
                       CheckDirectives &Directives) {
  // (a use that failed to load is not tried again when the units are
  // loaded once more, after an early load for declared(NAME))
  if (UnitsDone.count(U.Name) || FailedUses.count(U.Loc.getPointer()) ||
      Diags.shouldStop())
    return;
  if (!UnitsInProgress.insert(U.Name).second) {
    Diags.error(U.Loc, "circular unit reference to '" + U.Spelling + "'",
                {U.getRange()});
    return;
  }
  // Search the input's directory, -I directories, then the unit library.
  llvm::SmallVector<std::string, 4> Dirs{
      llvm::sys::path::parent_path(Opts.InputFile).str()};
  Dirs.append(Opts.IncludeDirs.begin(), Opts.IncludeDirs.end());
  Dirs.push_back(findUnitLibrary());
  std::string Found;
  for (const std::string &Dir : Dirs) {
    for (std::string Base : {U.Name.str(), U.Spelling.str()})
      for (const char *Ext : {".pas", ".pp"}) {
        llvm::SmallString<256> Path(Dir);
        llvm::sys::path::append(Path, Base + Ext);
        if (Found.empty() && llvm::sys::fs::exists(Path))
          Found = std::string(Path);
      }
  }
  if (Found.empty()) {
    UnitsInProgress.erase(U.Name);
    FailedUses.insert(U.Loc.getPointer());
    // objfpc: Exception and its kin are predeclared, so `uses sysutils`
    // works without the library.
    if (LangOpts.ObjKeywords && U.Name == "sysutils")
      return;
    Diags.error(U.Loc,
                "cannot find unit '" + U.Spelling + "' (looked for " +
                    U.Name + ".pas next to the program, in -I directories "
                    "and in pcc's unit library)",
                {U.getRange()});
    return;
  }
  auto FIDOrErr = SM.loadFile(Found);
  if (!FIDOrErr) {
    FailedUses.insert(U.Loc.getPointer());
    Diags.error(U.Loc, "cannot open unit file '" + llvm::Twine(Found) + "'");
    return;
  }
  UnitLexers.push_back(std::make_unique<Lexer>(SM, *FIDOrErr, Diags, Strings, LangOpts));
  prepareLexer(*UnitLexers.back(), Directives);
  UnitDecl *Unit;
  {
    Parser P(*UnitLexers.back(), Diags, Ctx);
    prepareParser(P, Ctx, Directives);
    Unit = P.parseUnit();
  }
  if (!Unit) {
    UnitsInProgress.erase(U.Name);
    FailedUses.insert(U.Loc.getPointer());
    return;
  }
  if (Unit->getName().Name != U.Name)
    Diags.error(Unit->getName().Loc,
                "file '" + llvm::Twine(Found) + "' declares unit '" +
                    Unit->getName().Spelling + "', expected '" + U.Spelling +
                    "'");
  for (const Identifier &Dep : Unit->getInterfaceUses())
    visitUnit(Dep, Ctx, Directives);
  for (const Identifier &Dep : Unit->getImplementationUses())
    visitUnit(Dep, Ctx, Directives);
  UnitsInProgress.erase(U.Name);
  UnitsDone[U.Name] = Unit;
  UnitsOrdered.push_back(Unit);
}

void Driver::dumpTokens(FileID FID, llvm::raw_ostream &OS) {
  // One token per line:  <line>:<col> <kind> '<spelling>' [value]
  Lexer L(SM, FID, Diags, Strings, LangOpts);
  L.setIncludeSupport(SM, Opts.IncludeDirs);
  L.setDefines(Opts.Defines);
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
    case tok::template_head:
    case tok::template_middle:
    case tok::template_tail:
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
