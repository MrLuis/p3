// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "p3/Driver/Driver.h"
#include "p3/AST/ASTContext.h"
#include "p3/AST/ASTPrinter.h"
#include "p3/Basic/Directives.h"
#include "p3/CodeGen/Backend.h"
#include "p3/CodeGen/CodeGen.h"
#include "p3/Driver/Linker.h"
#include "p3/Driver/Project.h"
#include "p3/Lexer/Lexer.h"
#include "p3/Parse/Parser.h"
#include "p3/Sema/Sema.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSet.h"

#include <cctype>
#include <functional>
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Target/TargetMachine.h"

#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm-c/Core.h"
#include "llvm-c/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using namespace p3;

//===----------------------------------------------------------------------===//
// CompilerOptions helpers
//===----------------------------------------------------------------------===//

const char *p3::getActionName(Action A) {
  switch (A) {
  case Action::EmitTokens: return "--emit-tokens";
  case Action::EmitAST:    return "--emit-ast";
  case Action::EmitParseTree: return "--emit-parse-tree";
  case Action::EmitModel:  return "--emit-model";
  case Action::EmitLLVM:   return "--emit-llvm";
  case Action::EmitAsm:    return "-S";
  case Action::EmitObj:    return "-c";
  case Action::EmitExe:    return "executable generation";
  }
  return "?";
}

std::string p3::getDefaultOutputFile(const CompilerOptions &Opts) {
  if (Opts.Act == Action::EmitTokens || Opts.Act == Action::EmitAST ||
      Opts.Act == Action::EmitParseTree || Opts.Act == Action::EmitModel)
    return "-";

  llvm::StringRef Stem = llvm::sys::path::stem(Opts.InputFile);
  if (Stem.empty() || Opts.InputFile == "-")
    Stem = "a";

  switch (Opts.Act) {
  case Action::EmitLLVM: return (Stem + ".ll").str();
  case Action::EmitAsm:  return (Stem + ".s").str();
  case Action::EmitObj:  return (Stem + ".o").str();
  case Action::EmitExe:
    if (llvm::Triple(Opts.TargetTriple).isOSWindows())
      return (Stem + ".exe").str();
    return Opts.InputFile == "-" ? std::string("a.out") : Stem.str();
  default:
    return "-";
  }
}

namespace {

/// FNV-1a of a file's bytes, as 16 lowercase hex digits: the interface
/// files' source and dependency stamp (item 22).
uint64_t interfaceHash(llvm::StringRef S) {
  uint64_t H = 14695981039346656037ull;
  for (unsigned char C : S) {
    H ^= C;
    H *= 1099511628211ull;
  }
  return H;
}

std::string hashHex(uint64_t V) {
  static const char Digits[] = "0123456789abcdef";
  std::string S(16, '0');
  for (int I = 15; I >= 0; --I, V >>= 4)
    S[I] = Digits[V & 15];
  return S;
}

bool readFileBytes(llvm::StringRef Path, std::string &Out) {
  auto B = llvm::MemoryBuffer::getFile(Path);
  if (!B)
    return false;
  Out = (*B)->getBuffer().str();
  return true;
}

bool findUnitSource(llvm::StringRef Name, llvm::ArrayRef<std::string> Dirs,
                    std::string &Out) {
  for (const std::string &Dir : Dirs)
    for (const char *Ext : {".pas", ".pp"}) {
      llvm::SmallString<256> Path(Dir);
      llvm::sys::path::append(Path, Name + Ext);
      if (llvm::sys::fs::exists(Path)) {
        Out = std::string(Path);
        return true;
      }
    }
  return false;
}

/// The offset of the `implementation` keyword, skipping comments and
/// strings, or S.size() if there is none.
size_t implementationOffset(llvm::StringRef S) {
  size_t I = 0, N = S.size();
  while (I < N) {
    char C = S[I];
    if (C == '\'' || C == '#') { // string (and a # code follows it)
      ++I;
      while (I < N) {
        if (S[I] == '\'') {
          if (I + 1 < N && S[I + 1] == '\'') {
            I += 2;
            continue;
          }
          ++I;
          break;
        }
        ++I;
      }
      continue;
    }
    if (C == '{') {
      ++I;
      while (I < N && S[I] != '}')
        ++I;
      ++I;
      continue;
    }
    if (C == '(' && I + 1 < N && S[I + 1] == '*') {
      I += 2;
      while (I + 1 < N && !(S[I] == '*' && S[I + 1] == ')'))
        ++I;
      I += 2;
      continue;
    }
    if (C == '/' && I + 1 < N && S[I + 1] == '/') {
      while (I < N && S[I] != '\n')
        ++I;
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(C)) || C == '_') {
      size_t B = I;
      while (I < N && (std::isalnum(static_cast<unsigned char>(S[I])) ||
                       S[I] == '_'))
        ++I;
      if (S.slice(B, I).equals_insensitive("implementation"))
        return B;
      continue;
    }
    ++I;
  }
  return N;
}

/// Does Unit declare any generic types, routines, or methods whose
/// implementation tokens a precompiled .p3i interface would lose?
bool declsHaveGenerics(llvm::ArrayRef<Decl *> Decls) {
  for (const Decl *D : Decls) {
    if (const auto *TD = llvm::dyn_cast<TypeDecl>(D)) {
      if (TD->isGeneric())
        return true;
      const TypeSpec *TS = TD->getTypeSpec();
      if (const auto *RT = llvm::dyn_cast_or_null<RecordTypeSpec>(TS)) {
        if (declsHaveGenerics(RT->getMembers()))
          return true;
      } else if (const auto *CT = llvm::dyn_cast_or_null<ClassTypeSpec>(TS)) {
        if (declsHaveGenerics(CT->getMembers()))
          return true;
      } else if (const auto *HT = llvm::dyn_cast_or_null<HelperTypeSpec>(TS)) {
        if (declsHaveGenerics(HT->getMembers()))
          return true;
      }
    } else if (const auto *RD = llvm::dyn_cast<RoutineDecl>(D)) {
      if (RD->isGeneric())
        return true;
    }
  }
  return false;
}

bool unitHasGenerics(const UnitDecl &Unit) {
  return (Unit.getInterface() &&
          declsHaveGenerics(Unit.getInterface()->getDecls())) ||
         (Unit.getImplementation() &&
          declsHaveGenerics(Unit.getImplementation()->getDecls()));
}

/// Reads a .p3i header; false if it is malformed or its source or a
/// dependency changed. Dirs: where a dependency's source is looked for.
bool interfaceValid(llvm::StringRef Path, llvm::StringRef SourcePath,
                    llvm::ArrayRef<std::string> Dirs) {
  std::string Text;
  if (!readFileBytes(Path, Text))
    return false;
  llvm::StringRef S(Text);
  llvm::StringRef Marker = "// p3 interface v1\n";
  size_t M = S.find(Marker);
  if (M == llvm::StringRef::npos)
    return false;
  S = S.drop_front(M + Marker.size());
  std::string SourceBytes;
  if (!readFileBytes(SourcePath, SourceBytes))
    return false;
  std::string SourceHash = hashHex(interfaceHash(SourceBytes));
  bool SawSource = false;
  while (!S.empty()) {
    size_t NL = S.find('\n');
    llvm::StringRef Line = S.substr(0, NL);
    if (NL == llvm::StringRef::npos)
      S = "";
    else
      S = S.drop_front(NL + 1);
    if (Line.consume_front("// generics")) {
      return false;
    }
    if (Line.consume_front("// source ")) {
      if (Line.trim() != SourceHash)
        return false;
      SawSource = true;
    } else if (Line.consume_front("// use ")) {
      size_t Space = Line.find(' ');
      if (Space == llvm::StringRef::npos)
        return false;
      llvm::StringRef Name = Line.substr(0, Space);
      llvm::StringRef Hash = Line.substr(Space + 1).trim();
      std::string Dep;
      if (!findUnitSource(Name, Dirs, Dep))
        return false;
      std::string DepBytes;
      if (!readFileBytes(Dep, DepBytes) ||
          hashHex(interfaceHash(DepBytes)) != Hash)
        return false;
    } else if (!Line.starts_with("//")) {
      break; // the unit text starts here
    }
  }
  return SawSource;
}

/// Writes the precompiled interface of Unit next to its object (item 22):
/// the source text through the `implementation` keyword, with a header
/// holding the source and interface dependencies' hashes.
bool writeInterfaceFile(UnitDecl &Unit, llvm::ArrayRef<UnitDecl *> Deps,
                        llvm::StringRef Source,
                        llvm::ArrayRef<std::string> Dirs,
                        llvm::StringRef Path, DiagnosticsEngine &Diags) {
  size_t Off = implementationOffset(Source);
  if (Off >= Source.size())
    return false;
  std::string Text = Source.substr(0, Off).str();
  Text += "\n// p3 interface v1\n";
  if (unitHasGenerics(Unit))
    Text += "// generics\n";
  Text += "// source " + hashHex(interfaceHash(Source)) + "\n";
  for (const Identifier &Use : Unit.getInterfaceUses()) {
    UnitDecl *Dep = nullptr;
    for (UnitDecl *D : Deps)
      if (D->getName().Name == Use.Name) {
        Dep = D;
        break;
      }
    std::string DepPath;
    std::string DepBytes;
    if (!Dep || !findUnitSource(Use.Name, Dirs, DepPath) ||
        !readFileBytes(DepPath, DepBytes))
      continue;
    Text += "// use " + Use.Name.str() + " " +
            hashHex(interfaceHash(DepBytes)) + "\n";
  }
  Text += "implementation\nend.\n";
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC, llvm::sys::fs::OF_Text);
  if (EC) {
    Diags.warning("cannot write interface file '" + Path + "'");
    return false;
  }
  OS << Text;
  return true;
}

} // namespace

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

/// The symbols Free Pascal predefines for the targets p3's library supports
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

/// optimization3 item 2: on AArch64 Linux, atomics use the library's
/// `__aarch64_*` helpers, which run the LSE instructions where the CPU has
/// them and an LL/SC loop elsewhere (clang's default there). Without the
/// feature the generic CPU always gets the loop, in the program's code and in
/// the inlined runtime (reference counts: `ansistr` is a third slower). A
/// -mattr that says anything about LSE is left alone. Darwin is not changed:
/// not measured.
static void addDefaultTargetFeatures(const llvm::Triple &T,
                                     std::string &Features) {
  if (T.getArch() != llvm::Triple::aarch64 || !T.isOSLinux())
    return;
  llvm::StringRef F = Features;
  if (F.contains_insensitive("lse") || F.contains_insensitive("outline-atomics"))
    return;
  if (!Features.empty())
    Features += ',';
  Features += "+outline-atomics";
}

void Driver::resolveTargetCPU() {
  if (Opts.TargetCPU != "native")
    return;
  Opts.TargetCPU = llvm::sys::getHostCPUName().str();
  if (Opts.TargetFeatures.empty()) {
    char *F = LLVMGetHostCPUFeatures();
    if (F) {
      Opts.TargetFeatures = F;
      LLVMDisposeMessage(F);
    }
  }
}

bool Driver::validateTarget() {
  initializeAllLLVMTargets();
  std::string TripleStr = Opts.TargetTriple.empty()
                              ? llvm::sys::getDefaultTargetTriple()
                              : llvm::Triple::normalize(Opts.TargetTriple);
  std::string Err;
  llvm::Triple TheTriple(TripleStr);
  if (!llvm::TargetRegistry::lookupTarget(TheTriple, Err)) {
    Diags.error("unsupported target '" + llvm::Twine(TripleStr) + "': " + Err);
    return false;
  }
  Opts.TargetTriple = TripleStr;
  addTargetDefines(llvm::Triple(TripleStr), Opts.Defines);
  addDefaultTargetFeatures(llvm::Triple(TripleStr), Opts.TargetFeatures);
  return true;
}

int Driver::run() {
  // The project file's settings (p3.json), under the command line's.
  if (!applyProjectFile(Opts, SM, Diags))
    return 1;
  resolveTargetCPU();
  configure();
  if (Opts.OutputFile.empty())
    Opts.OutputFile = getDefaultOutputFile(Opts);
  if (!validateOptions())
    return 1;

  if (Opts.Act == Action::EmitModel)
    return emitModel() ? 0 : 1;
  if (!Opts.Generators.empty() && !runGenerators())
    return 1;

  auto FIDOrErr = loadSource(Opts.InputFile);
  if (!FIDOrErr) {
    Diags.error("cannot open input file '" + llvm::Twine(Opts.InputFile) +
                "': " + FIDOrErr.getError().message());
    return 1;
  }

  FileID FID = *FIDOrErr;
  bool OK = runPipeline(FID);
  if (CompiletimeRestart) {
    // Compiletime constants were computed: again, on the new text (the
    // warnings given already are not repeated).
    CompiletimeRestart = false;
    UnitsDone.clear();
    UnitsInProgress.clear();
    FailedUses.clear();
    UnitsOrdered.clear();
    ExternalLibraries.clear();
    Diags.setSkipRepeatedWarnings(true);
    auto Again = loadSource(Opts.InputFile);
    if (!Again) {
      Diags.error("cannot open input file '" + llvm::Twine(Opts.InputFile) +
                  "'");
      return 1;
    }
    OK = runPipeline(*Again);
  }
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
    loadUnits(Program->getUses(), Ctx, Directives, Units, "",
              Opts.SeparateUnits);

  if (Opts.Act == Action::EmitParseTree) { // the parser alone (S5)
    auto OS = openTextOutput(Opts.OutputFile, Diags);
    if (!OS)
      return false;
    printParseTree(*Program, Units, SM, *OS);
    return true;
  }

  // --- compiletime constants: computed, then parsed again (not for a
  // dump: --emit-ast leaves them as calls) -------------------------------
  LangOpts.CompiletimeDeferred = Opts.Act == Action::EmitAST;
  if (!Diags.hasErrors() && Opts.Act != Action::EmitAST) {
    bool Restart = false;
    if (!evaluateCompiletime(Program, Units, Restart))
      return false;
    if (Restart) {
      CompiletimeRestart = true;
      return true;
    }
  }

  // --- M4: semantic analysis (skipped after syntax errors) ---------------
  bool SyntaxOK = !Diags.hasErrors();
  auto Reparse = [&](const RoutineDecl *G) {
    return Parser(Lex, Diags, Ctx).reparseGeneric(G);
  };
  auto ReparseType = [&](const TypeDecl *G,
                         llvm::SmallVectorImpl<Decl *> &Methods) {
    return Parser(Lex, Diags, Ctx).reparseGenericType(G, Methods);
  };
  bool SemaOK = SyntaxOK && !Diags.hasErrors() &&
                Sema(Ctx, Diags, LangOpts, Reparse, ReparseType)
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

  // Linking for another target needs the runtime built for it.
  if (Opts.Act == Action::EmitExe && !isHostTriple(Opts.TargetTriple)) {
    if (!haveRuntimeLibrary(Opts.TargetTriple)) {
      Diags.error("no Pascal runtime for target '" +
                  llvm::Twine(Opts.TargetTriple) +
                  "' (configure p3 with -DP3_CROSS_TARGETS=" +
                  runtimeTargetName(Opts.TargetTriple) +
                  "); -c, -S and --emit-llvm work without it");
      return false;
    }
  }

  auto TM = createTargetMachine(Opts.TargetTriple, Opts.OptLevel, Opts.TargetCPU,
                                Opts.TargetFeatures, Diags);
  if (!TM)
    return false;

  llvm::LLVMContext LLVMCtx;
  CodeGenOptions CGOpts;
  CGOpts.RangeChecks = Opts.RangeChecks;
  CGOpts.OverflowChecks = Opts.OverflowChecks;
  CGOpts.FastMath = Opts.FastMath;
  CGOpts.TargetCPU = Opts.TargetCPU;
  CGOpts.TargetFeatures = Opts.TargetFeatures;
  CGOpts.Directives = &Directives;
  CGOpts.DebugInfo = Opts.DebugInfo;
  CGOpts.OptLevel = Opts.OptLevel;
  CGOpts.SizeLevel = Opts.SizeLevel;
  CGOpts.ProfileInstrGenerate = Opts.ProfileInstrGenerate;
  CGOpts.ProfileInstrUse = Opts.ProfileInstrUse;
  CGOpts.ZeroInit = Opts.ZeroInit.value_or(Opts.OptLevel == 0);
  CGOpts.KeepUnused = Opts.KeepUnused;
  CGOpts.SeparateUnits = Opts.SeparateUnits;

  // Separately compiled units: their objects, next to their sources.
  std::vector<std::string> UnitObjects;
  if (Opts.SeparateUnits)
    for (UnitDecl *U : Units) {
      llvm::SmallString<256> Obj(
          SM.getPresumedLoc(U->getLocation()).Filename);
      llvm::sys::path::replace_extension(Obj, "o");
      // The library's units are compiled for the host only.
      if (!isHostTriple(Opts.TargetTriple) &&
          llvm::sys::path::parent_path(Obj) == findUnitLibrary()) {
        if (Opts.Act == Action::EmitExe) {
          Diags.error("unit '" + U->getName().Spelling +
                      "' of p3's library is compiled for the host only; build "
                      "for '" + Opts.TargetTriple + "' without -fseparate-units");
          return false;
        }
        continue;
      }
      if (!llvm::sys::fs::exists(Obj)) {
        if (Opts.Act == Action::EmitExe) {
          Diags.error("cannot find '" + Obj + "', the object file of unit '" +
                      U->getName().Spelling + "'; compile the unit with -c");
          return false;
        }
        continue; // -c/-S/--emit-llvm: the object is only needed to link
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
  loadUnits(Uses, Ctx, Directives, Deps, Unit->getName().Name, true);
  if (!Diags.hasErrors()) {
    bool Restart = false;
    if (!evaluateCompiletime(Unit, Deps, Restart))
      return false;
    if (Restart) {
      CompiletimeRestart = true;
      return true;
    }
  }
  auto Reparse = [&](const RoutineDecl *G) {
    return Parser(P.getLexer(), Diags, Ctx).reparseGeneric(G);
  };
  auto ReparseType = [&](const TypeDecl *G,
                         llvm::SmallVectorImpl<Decl *> &Methods) {
    return Parser(P.getLexer(), Diags, Ctx).reparseGenericType(G, Methods);
  };
  if (Diags.hasErrors() ||
      !Sema(Ctx, Diags, LangOpts, Reparse, ReparseType).checkUnit(*Unit, Deps))
    return false;
  if (Opts.Act == Action::EmitObj && Opts.OutputFile != "-") {
    llvm::SmallVector<std::string, 4> Dirs{
        llvm::sys::path::parent_path(Opts.InputFile).str()};
    Dirs.append(Opts.IncludeDirs.begin(), Opts.IncludeDirs.end());
    Dirs.push_back(findUnitLibrary());
    llvm::StringRef Source = SM.getBufferData(FID);
    llvm::SmallString<256> P3I(Opts.OutputFile);
    llvm::sys::path::replace_extension(P3I, "p3i");
    writeInterfaceFile(*Unit, Deps, Source, Dirs, P3I, Diags);
    llvm::SmallString<256> SrcP3I(SM.getFilename(FID));
    llvm::sys::path::replace_extension(SrcP3I, "p3i");
    if (SrcP3I != P3I)
      writeInterfaceFile(*Unit, Deps, Source, Dirs, SrcP3I, Diags);
  }
  if (Opts.Act == Action::EmitExe) {
    Diags.error("'" + llvm::Twine(Opts.InputFile) +
                "' is a unit: compile it with -c, -S or --emit-llvm and link "
                "it into a program built with -fseparate-units");
    return false;
  }
  auto TM = createTargetMachine(Opts.TargetTriple, Opts.OptLevel, Opts.TargetCPU,
                                Opts.TargetFeatures, Diags);
  if (!TM)
    return false;
  llvm::LLVMContext LLVMCtx;
  CodeGenOptions CGOpts;
  CGOpts.RangeChecks = Opts.RangeChecks;
  CGOpts.OverflowChecks = Opts.OverflowChecks;
  CGOpts.FastMath = Opts.FastMath;
  CGOpts.TargetCPU = Opts.TargetCPU;
  CGOpts.TargetFeatures = Opts.TargetFeatures;
  CGOpts.Directives = &Directives;
  CGOpts.DebugInfo = Opts.DebugInfo;
  CGOpts.OptLevel = Opts.OptLevel;
  CGOpts.SizeLevel = Opts.SizeLevel;
  CGOpts.ProfileInstrGenerate = Opts.ProfileInstrGenerate;
  CGOpts.ProfileInstrUse = Opts.ProfileInstrUse;
  CGOpts.ZeroInit = Opts.ZeroInit.value_or(Opts.OptLevel == 0);
  CGOpts.KeepUnused = Opts.KeepUnused;
  CGOpts.SeparateUnits = true;
  auto M = generateUnitModule(*Unit, Deps, SM, LangOpts, CGOpts, LLVMCtx, *TM,
                              SM.getFilename(FID));
  // At -O1+ leave the unit's IR next to its object: a program compiled with
  // -fseparate-units links it before optimizing, to inline inline routines
  // (item 17), or with -fwhole-program in place of the object. Any other
  // compile of the object removes an earlier IR file, which would no
  // longer match it: an IR file present is always the object's.
  if (Opts.Act == Action::EmitObj && Opts.OutputFile != "-") {
    llvm::SmallString<256> IRPath(Opts.OutputFile);
    llvm::sys::path::replace_extension(IRPath, "ll");
    if (Opts.OptLevel > 0)
      writeModuleIR(*M, IRPath, Diags);
    else
      llvm::sys::fs::remove(IRPath);
  }
  return finishModule(*M, *TM, {});
}

/// -fwhole-program: the IR next to each unit's object (Path.o -> Path.ll),
/// in IRs. Every unit needs one, since a unit left as an object would refer
/// to symbols the others made internal: otherwise a warning, and false
/// (the units link as objects).
bool Driver::wholeProgramIRs(llvm::ArrayRef<std::string> UnitObjects,
                             std::vector<std::string> &IRs) {
  for (const std::string &Obj : UnitObjects) {
    llvm::SmallString<256> IR(Obj);
    llvm::sys::path::replace_extension(IR, "ll");
    if (!llvm::sys::fs::exists(IR)) {
      Diags.warning("-fwhole-program: '" + IR +
                    "' is missing (compile the unit with -c at -O1 or "
                    "higher); the units are linked as objects");
      return false;
    }
    IRs.push_back(std::string(IR));
  }
  return true;
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
  // (-fwhole-program: the units' code is linked as IR, not as objects.)
  llvm::ArrayRef<std::string> LinkObjects = UnitObjects;
  if (Opts.OptLevel > 0) {
    std::vector<std::string> UnitIRs;
    if (Opts.WholeProgram && !UnitObjects.empty() &&
        wholeProgramIRs(UnitObjects, UnitIRs)) {
      if (!linkUnitIRWhole(*M, UnitIRs, Diags))
        return false;
      LinkObjects = {};
    } else if (!UnitObjects.empty())
      linkUnitIR(*M, UnitObjects, Diags);
    // The runtime's IR is built for the host.
    if (Opts.InlineRuntime &&
        llvm::Triple(Opts.TargetTriple) ==
            llvm::Triple(llvm::sys::getDefaultTargetTriple()))
      linkRuntimeIR(*M, findRuntimeIR(), Opts.TargetCPU, Opts.TargetFeatures,
                    Diags, Opts.InternalizeRuntime);
    optimizeModule(*M, *TM, Opts.OptLevel, Opts.SizeLevel,
                   Opts.ProfileInstrGenerate, Opts.ProfileInstrUse);
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
          llvm::sys::fs::createTemporaryFile("p3", "o", ObjPath)) {
    Diags.error("cannot create temporary file: " + EC.message());
    return false;
  }
  llvm::FileRemover RemoveObj(ObjPath);
  if (!emitOutput(*M, *TM, OutputKind::Object, ObjPath, Diags))
    return false;
  std::vector<std::string> Libraries = Opts.Libraries;
  if (!Opts.ProfileInstrGenerate.empty()) {
    std::string ProfileRuntime = findProfileRuntimeLibrary();
    if (ProfileRuntime.empty()) {
      Diags.error("cannot find the profile runtime (libclang_rt.profile) "
                  "next to LLVM's libraries; set P3_PROFILE_RUNTIME");
      return false;
    }
    Libraries.push_back(ProfileRuntime);
  }
  Libraries.insert(Libraries.end(), ExternalLibraries.begin(),
                   ExternalLibraries.end());
  return linkExecutable(ObjPath, LinkObjects, Opts.OutputFile, Opts.LibraryDirs,
                        Libraries, Opts.TargetTriple, Diags);
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
    loadUnits(Uses, Ctx, Directives, Loaded, Self, Opts.SeparateUnits);
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
                       llvm::StringRef Self, bool InterfacesOnly) {
  if (!Self.empty())
    UnitsInProgress.insert(Self);
  if (!Uses.empty() && LangOpts.Mode == LangMode::ISO) {
    Diags.error(Uses[0].Loc, "units are a Turbo/Free Pascal extension; use --mode=tp");
    return;
  }
  for (const Identifier &U : Uses)
    visitUnit(U, Ctx, Directives, InterfacesOnly);
  Ordered.assign(UnitsOrdered.begin(), UnitsOrdered.end());
}

void Driver::visitUnit(const Identifier &U, ASTContext &Ctx,
                       CheckDirectives &Directives, bool InterfacesOnly) {
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
                    "and in p3's unit library)",
                {U.getRange()});
    return;
  }
  if (InterfacesOnly) {
    llvm::SmallString<256> P3I(Found);
    llvm::sys::path::replace_extension(P3I, "p3i");
    if (llvm::sys::fs::exists(P3I) && interfaceValid(P3I, Found, Dirs))
      Found = std::string(P3I);
  }
  auto FIDOrErr = loadSource(Found);
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
    visitUnit(Dep, Ctx, Directives, InterfacesOnly);
  if (!InterfacesOnly)
    for (const Identifier &Dep : Unit->getImplementationUses())
      visitUnit(Dep, Ctx, Directives, InterfacesOnly);
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
