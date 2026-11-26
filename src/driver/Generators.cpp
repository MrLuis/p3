// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- Generators.cpp - The model of attributed types -------------------===//
//
// See include/p3/Driver/Generators.h. One line per fact, fields separated by
// tabs (\, tab and newline escaped):
//
//   p3model  1
//   type     name  kind(record|class|enum)  parent  line  col
//   field    name  type-as-written  visibility  line  col
//   method   name  kind  result  visibility  static  line  col
//   param    name  type-as-written  mode(value|var|const|out)
//   attr     name  line  col        (of the type, field or method before it)
//   arg      kind(int|real|string|bool|ident)  value
//   value    name  line  col    (of an enumeration, kind enum)
//   endtype
//
//===----------------------------------------------------------------------===//

#include "p3/Driver/Generators.h"
#include "p3/Driver/Linker.h"

#include "p3/AST/AST.h"
#include "p3/AST/ASTContext.h"
#include "p3/Driver/Driver.h"
#include "p3/Lexer/Lexer.h"
#include "p3/Parse/Parser.h"
#include "p3/Sema/Sema.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

using namespace p3;

std::string p3::escapeField(llvm::StringRef S) {
  std::string R;
  for (char C : S) {
    if (C == '\\')
      R += "\\\\";
    else if (C == '\t')
      R += "\\t";
    else if (C == '\n')
      R += "\\n";
    else if (C == '\r')
      R += "\\r";
    else
      R += C;
  }
  return R;
}

std::string p3::unescapeField(llvm::StringRef S) {
  std::string R;
  for (size_t I = 0; I < S.size(); ++I) {
    if (S[I] == '\\' && I + 1 < S.size()) {
      char N = S[++I];
      R += N == 't' ? '\t' : N == 'n' ? '\n' : N == 'r' ? '\r' : N;
    } else {
      R += S[I];
    }
  }
  return R;
}

namespace {

class ModelBuilder {
  const SourceManager &SM;
  llvm::StringRef Attr;
  std::string Out;

  /// The source text between two locations of one buffer, with runs of
  /// white space as one blank.
  std::string textOf(SourceRange R) const {
    if (!R.getBegin().isValid() || !R.getEnd().isValid())
      return "";
    const char *B = R.getBegin().getPointer(), *E = R.getEnd().getPointer();
    std::string T;
    bool Blank = false;
    for (const char *P = B; P < E; ++P) {
      if (*P == ' ' || *P == '\t' || *P == '\n' || *P == '\r') {
        Blank = !T.empty();
      } else {
        if (Blank)
          T += ' ';
        Blank = false;
        T += *P;
      }
    }
    return T;
  }

  std::string typeText(TypeSpec *TS) const {
    return TS ? textOf(TS->getRange()) : std::string();
  }

  void line(const char *Tag, std::initializer_list<std::string> Fields) {
    Out += Tag;
    for (const std::string &F : Fields) {
      Out += '\t';
      Out += escapeField(F);
    }
    Out += '\n';
  }

  static std::string num(unsigned N) { return std::to_string(N); }

  std::pair<std::string, std::string> pos(SourceLocation L) const {
    PresumedLoc P = SM.getPresumedLoc(L);
    return {num(P.Line), num(P.Column)};
  }

  void arg(Expr *E) {
    if (auto *I = llvm::dyn_cast<IntegerLiteralExpr>(E))
      return line("arg", {"int", std::to_string(I->getValue())});
    if (auto *U = llvm::dyn_cast<UnaryExpr>(E))
      if (U->getOp() == UnaryOp::Minus)
        if (auto *I = llvm::dyn_cast<IntegerLiteralExpr>(U->getOperand()))
          return line("arg", {"int", "-" + std::to_string(I->getValue())});
    if (auto *R = llvm::dyn_cast<RealLiteralExpr>(E))
      return line("arg", {"real", textOf(R->getRange())});
    if (auto *S = llvm::dyn_cast<StringLiteralExpr>(E))
      return line("arg", {"string", S->getValue().str()});
    std::string T = textOf(E->getRange());
    std::string L = llvm::StringRef(T).lower();
    if (L == "true" || L == "false")
      return line("arg", {"bool", L});
    line("arg", {"ident", T});
  }

  void attrs(Decl *D) {
    for (const Attribute &A : D->getAttributes()) {
      auto [L, C] = pos(A.Name.Loc);
      line("attr", {A.Name.Spelling.str(), L, C});
      for (Expr *E : A.Args)
        arg(E);
    }
  }

  static const char *visibility(Visibility V) {
    switch (V) {
    case Visibility::Public: return "public";
    case Visibility::Published: return "published";
    case Visibility::Protected: return "protected";
    case Visibility::Private: return "private";
    case Visibility::StrictProtected: return "strictprotected";
    case Visibility::StrictPrivate: return "strictprivate";
    }
    return "public";
  }

  void member(Decl *D) {
    if (auto *F = llvm::dyn_cast<FieldDecl>(D)) {
      auto [L, C] = pos(F->getLocation());
      line("field", {F->getName().Spelling.str(), typeText(F->getTypeSpec()),
                     visibility(F->getVisibility()), L, C});
      attrs(F);
    } else if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) {
      auto [L, C] = pos(R->getLocation());
      const char *Kind = R->isConstructor()  ? "constructor"
                         : R->isDestructor() ? "destructor"
                         : R->isFunction()   ? "function"
                                             : "procedure";
      line("method", {R->getName().Spelling.str(), Kind,
                      R->isFunction() ? typeText(R->getResultTypeSpec()) : "",
                      visibility(R->getVisibility()),
                      R->isClassMethod() || R->isStatic() ? "1" : "0", L, C});
      attrs(R);
      for (VarDecl *P : R->getParams()) {
        const char *Mode = P->isOutParam()                       ? "out"
                           : P->getVarKind() == VarDecl::VarParam ? "var"
                           : P->getVarKind() == VarDecl::ConstParam ? "const"
                                                                    : "value";
        line("param", {P->getName().Spelling.str(), typeText(P->getTypeSpec()),
                       Mode});
      }
    }
  }

public:
  unsigned NumTypes = 0;

  ModelBuilder(const SourceManager &SM, llvm::StringRef Attr)
      : SM(SM), Attr(Attr) {}

  void add(TypeDecl *TD) {
    TypeSpec *TS = TD->getTypeSpec();
    if (!TS || TD->isGeneric() || !TD->hasAttribute(Attr))
      return;
    llvm::ArrayRef<Decl *> Members;
    std::string Kind, Parent;
    llvm::SmallVector<Decl *, 16> All;
    if (auto *RS = llvm::dyn_cast<RecordTypeSpec>(TS)) {
      Kind = "record";
      if (FieldList *FL = RS->getFields())
        All.append(FL->Fixed.begin(), FL->Fixed.end());
      All.append(RS->getMembers().begin(), RS->getMembers().end());
    } else if (auto *CS = llvm::dyn_cast<ClassTypeSpec>(TS)) {
      if (CS->isForward())
        return;
      Kind = "class";
      Parent = CS->getParent().isValid() ? CS->getParent().Spelling.str() : "";
      All.append(CS->getMembers().begin(), CS->getMembers().end());
    } else if (llvm::isa<EnumTypeSpec>(TS)) {
      Kind = "enum";
    } else {
      return;
    }
    if (Out.empty())
      Out = "p3model\t1\n";
    auto [L, C] = pos(TD->getLocation());
    line("type", {TD->getName().Spelling.str(), Kind, Parent, L, C});
    attrs(TD);
    for (Decl *M : All)
      member(M);
    if (auto *ES = llvm::dyn_cast<EnumTypeSpec>(TS))
      for (const Identifier &V : ES->getNames()) {
        auto [VL, VC] = pos(V.Loc);
        line("value", {V.Spelling.str(), VL, VC});
      }
    line("endtype", {});
    ++NumTypes;
  }

  std::string take() { return std::move(Out); }
};

} // namespace

std::string p3::buildModel(const SourceManager &SM, Decl *Root,
                           llvm::StringRef AttrLower, unsigned &NumTypes) {
  ModelBuilder B(SM, AttrLower);
  llvm::ArrayRef<Decl *> Decls;
  if (auto *U = llvm::dyn_cast<UnitDecl>(Root))
    Decls = U->getInterface()->getDecls();
  else if (auto *P = llvm::dyn_cast<ProgramDecl>(Root))
    Decls = P->getBlock()->getDecls();
  for (Decl *D : Decls)
    if (auto *TD = llvm::dyn_cast<TypeDecl>(D))
      B.add(TD);
  NumTypes = B.NumTypes;
  return B.take();
}

//===----------------------------------------------------------------------===//
// The driver side: build and run the generators, splice in what they write
//===----------------------------------------------------------------------===//

bool p3::isLibraryGenerator(llvm::StringRef Path) {
  return !Path.empty() && Path.find('/') == llvm::StringRef::npos &&
         Path.find('\\') == llvm::StringRef::npos &&
         !Path.ends_with(".pas") && !Path.ends_with(".pp");
}

std::string p3::generatorFile(llvm::StringRef Path) {
  if (!isLibraryGenerator(Path))
    return Path.str();
  llvm::SmallString<256> F(findUnitLibrary());
  llvm::sys::path::append(F, "generators", Path + ".pas");
  return std::string(F);
}

std::string p3::absolutePath(llvm::StringRef P) {
  llvm::SmallString<256> A(P);
  llvm::sys::fs::make_absolute(A);
  llvm::sys::path::remove_dots(A, /*remove_dot_dot=*/true);
  return std::string(A);
}

namespace {

/// A file parsed without reporting (the compilation proper reports).
struct QuietParse {
  std::unique_ptr<llvm::raw_null_ostream> Null;
  std::unique_ptr<ASTContext> Ctx;
  std::unique_ptr<DiagnosticsEngine> Diags;
  std::unique_ptr<Lexer> Lex;
  Decl *Root = nullptr; ///< a ProgramDecl or a UnitDecl; null after errors
  FileID FID = 0;
};

std::vector<std::string> splitLines(llvm::StringRef S) {
  std::vector<std::string> R;
  while (!S.empty()) {
    auto [L, Rest] = S.split('\n');
    R.push_back(L.str());
    S = Rest;
  }
  return R;
}

/// Splits a model or output line at its tabs.
std::vector<std::string> fields(llvm::StringRef Line) {
  std::vector<std::string> R;
  size_t S = 0;
  for (;;) {
    size_t T = Line.find('\t', S);
    if (T == llvm::StringRef::npos) {
      R.push_back(unescapeField(Line.substr(S)));
      return R;
    }
    R.push_back(unescapeField(Line.slice(S, T)));
    S = T + 1;
  }
}

struct GeneratorInfo {
  std::string Path, Name, Attr, Exe;
};

/// FNV-1a over Data, from Seed (as the interface files' hash).
uint64_t fnv(llvm::StringRef Data, uint64_t Seed = 14695981039346656037ull) {
  uint64_t H = Seed;
  for (unsigned char C : Data) {
    H ^= C;
    H *= 1099511628211ull;
  }
  return H;
}

std::string hex16(uint64_t V) {
  static const char Digits[] = "0123456789abcdef";
  std::string S(16, '0');
  for (int I = 15; I >= 0; --I, V >>= 4)
    S[I] = Digits[V & 15];
  return S;
}

bool readFileText(llvm::StringRef Path, std::string &Out) {
  auto B = llvm::MemoryBuffer::getFile(Path);
  if (!B)
    return false;
  Out = (*B)->getBuffer().str();
  return true;
}

/// Is the stamp file Path just Value? (a cache is valid)
bool stampIs(llvm::StringRef Path, llvm::StringRef Value) {
  std::string Text;
  return readFileText(Path, Text) && Text == Value;
}

bool writeText(llvm::StringRef Path, llvm::StringRef Text) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC, llvm::sys::fs::OF_Text);
  if (EC)
    return false;
  OS << Text;
  return true;
}

/// Writes Text unless the file already has it (a build system sees an
/// unchanged file).
bool writeIfChanged(llvm::StringRef Path, llvm::StringRef Text) {
  std::string Old;
  if (readFileText(Path, Old) && Old == Text)
    return true;
  return writeText(Path, Text);
}

/// What the generators wrote for one type.
struct TypeOutput {
  std::string Name, Kind;
  std::vector<std::string> Members, Impl, Decls;
};

} // namespace

llvm::ErrorOr<FileID> Driver::loadSource(llvm::StringRef Path) {
  if (!Splices.empty()) {
    auto It = Splices.find(absolutePath(Path));
    if (It != Splices.end())
      return SM.addBuffer(It->second, Path);
  }
  return SM.loadFile(Path);
}

namespace {

std::unique_ptr<QuietParse> parseQuietly(SourceManager &SM, StringPool &Strings,
                                         const LangOptions &LangOpts,
                                         const CompilerOptions &Opts,
                                         llvm::StringSet<> &Predeclared,
                                         llvm::StringRef Path, FileID FID) {
  auto Q = std::make_unique<QuietParse>();
  Q->FID = FID;
  Q->Null = std::make_unique<llvm::raw_null_ostream>();
  Q->Ctx = std::make_unique<ASTContext>(Strings);
  Q->Diags = std::make_unique<DiagnosticsEngine>(SM, *Q->Null);
  Q->Lex = std::make_unique<Lexer>(SM, FID, *Q->Diags, Strings, LangOpts);
  Q->Lex->setIncludeSupport(SM, Opts.IncludeDirs);
  Q->Lex->setDefines(Opts.Defines);
  Q->Lex->setPredeclared(&Predeclared);
  Parser P(*Q->Lex, *Q->Diags, *Q->Ctx);
  if (P.startsWithUnit())
    Q->Root = P.parseUnit();
  else
    Q->Root = P.parseProgram();
  if (Q->Diags->hasErrors())
    Q->Root = nullptr;
  return Q;
}

/// The location of line:col (1-based) in a buffer.
SourceLocation locAt(const SourceManager &SM, FileID FID, unsigned Line,
                     unsigned Col) {
  llvm::StringRef Buf = SM.getBufferData(FID);
  size_t Off = 0;
  for (unsigned L = 1; L < Line && Off < Buf.size(); ++L) {
    size_t NL = Buf.find('\n', Off);
    if (NL == llvm::StringRef::npos)
      break;
    Off = NL + 1;
  }
  Off += Col ? Col - 1 : 0;
  if (Off > Buf.size())
    Off = Buf.size();
  return SourceLocation::fromPointer(Buf.data() + Off);
}

} // namespace

bool Driver::emitModel() {
  auto FIDOrErr = SM.loadFile(Opts.InputFile);
  if (!FIDOrErr) {
    Diags.error("cannot open input file '" + llvm::Twine(Opts.InputFile) +
                "': " + FIDOrErr.getError().message());
    return false;
  }
  for (const std::string &N : Sema(*std::make_unique<ASTContext>(Strings), Diags, LangOpts).standardNames())
    PredeclaredNames.insert(N);
  auto Q = parseQuietly(SM, Strings, LangOpts, Opts, PredeclaredNames,
                        Opts.InputFile, *FIDOrErr);
  if (!Q->Root) {
    Diags.error("cannot read '" + llvm::Twine(Opts.InputFile) +
                "': it has syntax errors");
    return false;
  }
  unsigned N = 0;
  std::string Model = buildModel(SM, Q->Root, Opts.ModelAttr, N);
  std::error_code EC;
  llvm::raw_fd_ostream OS(Opts.OutputFile.empty() ? "-" : Opts.OutputFile, EC,
                          llvm::sys::fs::OF_Text);
  if (EC) {
    Diags.error("cannot open output file '" + Opts.OutputFile +
                "': " + EC.message());
    return false;
  }
  OS << Model;
  return true;
}

bool Driver::runGenerators() {
  namespace fs = llvm::sys::fs;
  auto Fail = [&](const llvm::Twine &M) {
    Diags.error(M);
    return false;
  };
  // (as visitUnit names them: no "./" before a unit next to the input)
  llvm::SmallString<256> InputDir(llvm::sys::path::parent_path(Opts.InputFile));
  std::string GenDir = Opts.GeneratedDir;
  if (GenDir.empty()) {
    llvm::SmallString<256> D(InputDir);
    llvm::sys::path::append(D, "gen");
    GenDir = std::string(D);
  }
  GenDir = absolutePath(GenDir);
  llvm::SmallString<256> BinDir(GenDir);
  llvm::sys::path::append(BinDir, ".bin");
  if (auto EC = fs::create_directories(BinDir))
    return Fail("cannot create '" + BinDir + "': " + EC.message());

  for (const std::string &N : Sema(*std::make_unique<ASTContext>(Strings), Diags, LangOpts).standardNames())
    PredeclaredNames.insert(N);
  std::string Self = fs::getMainExecutable(nullptr, nullptr);
  // The directories units are looked for in (not the library: its units
  // have no generated code).
  std::vector<std::string> Dirs{std::string(InputDir)};
  Dirs.insert(Dirs.end(), Opts.IncludeDirs.begin(), Opts.IncludeDirs.end());

  // --- the generators: header, then build ---------------------------------
  std::vector<GeneratorInfo> Gens;
  for (const std::string &Given : Opts.Generators) {
    std::string Path = generatorFile(Given);
    auto FIDOrErr = SM.loadFile(Path);
    if (!FIDOrErr)
      return Fail("cannot open generator '" + Path +
                  "': " + FIDOrErr.getError().message());
    auto Q = parseQuietly(SM, Strings, LangOpts, Opts, PredeclaredNames, Path,
                          *FIDOrErr);
    auto *PD = llvm::dyn_cast_or_null<ProgramDecl>(Q->Root);
    if (!PD) {
      // (let the compiler report why)
      Gens.push_back({Path, "", "", ""});
    } else if (!PD->isGenerator()) {
      return Fail("'" + Path + "' is not a source generator: it must start "
                  "with 'generator Name for Attribute;'");
    } else {
      GeneratorInfo G;
      G.Path = Path;
      G.Name = PD->getName().Spelling.str();
      G.Attr = PD->getGeneratorFor().Name.str();
      llvm::SmallString<256> Exe(BinDir);
      llvm::sys::path::append(Exe, G.Name);
      G.Exe = hostExecutable(Exe);
      Gens.push_back(G);
    }
    GeneratorInfo &G = Gens.back();
    if (G.Exe.empty()) {
      llvm::SmallString<256> Exe(BinDir);
      llvm::sys::path::append(Exe, llvm::sys::path::stem(Path));
      G.Exe = hostExecutable(Exe);
    }
    // The generator is built again when its source (or the -I directories)
    // changed; delete gen/.bin to force it.
    std::string SrcText;
    uint64_t BuildHash = 0;
    if (readFileText(Path, SrcText)) {
      BuildHash = fnv(SrcText);
      for (const std::string &D : Opts.IncludeDirs)
        BuildHash = fnv(D, BuildHash);
    }
    std::string BuildStamp = G.Exe + ".stamp";
    if (BuildHash && fs::exists(G.Exe) && stampIs(BuildStamp, hex16(BuildHash)))
      continue;
    std::vector<std::string> Args{Self, "--no-project", "--mode=objfpc"};
    for (const std::string &D : Opts.IncludeDirs)
      Args.push_back("-I" + D);
    llvm::StringRef GenDirOfSource = llvm::sys::path::parent_path(Path);
    Args.push_back("-I" + (GenDirOfSource.empty() ? std::string(".")
                                                  : GenDirOfSource.str()));
    Args.push_back("-o");
    Args.push_back(G.Exe);
    Args.push_back(Path);
    std::vector<llvm::StringRef> Refs(Args.begin(), Args.end());
    std::string Err;
    int RC = llvm::sys::ExecuteAndWait(Self, Refs, std::nullopt, {}, 0, 0, &Err);
    if (RC != 0)
      return Fail("generator '" + Path + "' failed to build" +
                  (Err.empty() ? "" : ": " + Err));
    if (BuildHash)
      writeText(BuildStamp, hex16(BuildHash));
  }

  // --- the project's sources: the input and the units it uses -------------
  std::vector<std::string> Sources;
  std::set<std::string> Seen;
  struct Walk {
    Driver &D;
    SourceManager &SM;
    std::vector<std::string> &Sources;
    std::set<std::string> &Seen;
    const std::vector<std::string> &Dirs;
  };
  std::vector<std::string> Work{Opts.InputFile};
  Seen.insert(absolutePath(Opts.InputFile));
  struct Parsed {
    std::string Path;
    std::unique_ptr<QuietParse> Q;
  };
  std::vector<Parsed> Files;
  while (!Work.empty()) {
    std::string Path = Work.back();
    Work.pop_back();
    auto FIDOrErr = SM.loadFile(Path);
    if (!FIDOrErr)
      continue; // (reported by the compilation)
    auto Q = parseQuietly(SM, Strings, LangOpts, Opts, PredeclaredNames, Path,
                          *FIDOrErr);
    if (!Q->Root)
      continue;
    llvm::SmallVector<Identifier, 8> Uses;
    if (auto *PD = llvm::dyn_cast<ProgramDecl>(Q->Root)) {
      Uses.append(PD->getUses().begin(), PD->getUses().end());
    } else if (auto *UD = llvm::dyn_cast<UnitDecl>(Q->Root)) {
      Uses.append(UD->getInterfaceUses().begin(), UD->getInterfaceUses().end());
      Uses.append(UD->getImplementationUses().begin(),
                  UD->getImplementationUses().end());
    }
    for (const Identifier &U : Uses)
      for (const std::string &Dir : Dirs) {
        std::string Found;
        for (llvm::StringRef Base : {U.Name, U.Spelling})
          for (const char *Ext : {".pas", ".pp"}) {
            llvm::SmallString<256> P(Dir);
            llvm::sys::path::append(P, Base + Ext);
            if (Found.empty() && fs::exists(P))
              Found = std::string(P);
          }
        if (!Found.empty()) {
          if (Seen.insert(absolutePath(Found)).second)
            Work.push_back(Found);
          break;
        }
      }
    Files.push_back({Path, std::move(Q)});
  }

  // --- run each generator over each file ------------------------------------
  bool OK = true;
  for (Parsed &F : Files) {
    QuietParse &Q = *F.Q;
    std::vector<TypeOutput> Types;
    std::map<std::string, size_t> TypeIndex;
    for (const GeneratorInfo &G : Gens) {
      if (G.Attr.empty())
        continue;
      unsigned NumTypes = 0;
      std::string Model = buildModel(SM, Q.Root, G.Attr, NumTypes);
      if (!NumTypes)
        continue;
      std::string Base = llvm::sys::path::stem(F.Path).str();
      llvm::SmallString<256> ModelPath(GenDir), OutPath(GenDir);
      llvm::sys::path::append(ModelPath, Base + "." + G.Name + ".model");
      llvm::sys::path::append(OutPath, Base + "." + G.Name + ".out");
      if (!writeIfChanged(ModelPath, Model))
        return Fail("cannot write '" + ModelPath + "'");
      // The output is kept while the model and the generator are the
      // same: the same input gives the same output.
      std::string ExeBytes;
      readFileText(G.Exe, ExeBytes);
      std::string RunStamp = hex16(fnv(Model, fnv(ExeBytes)));
      std::string StampPath = std::string(OutPath) + ".stamp";
      if (!(fs::exists(OutPath) && stampIs(StampPath, RunStamp))) {
        std::vector<std::optional<llvm::StringRef>> Redirects = {
            llvm::StringRef(ModelPath), llvm::StringRef(OutPath), std::nullopt};
        std::string Err;
        int RC = llvm::sys::ExecuteAndWait(
            G.Exe, {G.Exe}, std::nullopt, Redirects, Opts.GeneratorTimeout,
            /*MemoryLimit=*/2048, &Err);
        if (RC == -2) {
          Diags.error("generator '" + G.Name + "' ran longer than " +
                      std::to_string(Opts.GeneratorTimeout) + " seconds");
          OK = false;
          continue;
        }
        if (RC != 0) {
          Diags.error("generator '" + G.Name + "' failed (exit status " +
                      std::to_string(RC) + ")" +
                      (Err.empty() ? "" : ": " + Err));
          OK = false;
          continue;
        }
        writeText(StampPath, RunStamp);
      }
      auto OutBuf = llvm::MemoryBuffer::getFile(OutPath);
      if (!OutBuf) {
        Diags.error("cannot read '" + OutPath + "'");
        OK = false;
        continue;
      }
      TypeOutput *Cur = nullptr;
      for (const std::string &Ln : splitLines((*OutBuf)->getBuffer())) {
        if (Ln.empty())
          continue;
        std::vector<std::string> Fs = fields(Ln);
        const std::string &Tag = Fs[0];
        if (Tag == "type" && Fs.size() >= 3) {
          auto [It, New] = TypeIndex.insert({llvm::StringRef(Fs[1]).lower(), Types.size()});
          if (New)
            Types.push_back({Fs[1], Fs[2], {}, {}, {}});
          Cur = &Types[It->second];
        } else if (Tag == "member" && Cur && Fs.size() >= 2) {
          Cur->Members.push_back(Fs[1]);
        } else if (Tag == "line" && Cur && Fs.size() >= 2) {
          Cur->Impl.push_back(Fs[1]);
        } else if (Tag == "decl" && Cur && Fs.size() >= 2) {
          Cur->Decls.push_back(Fs[1]);
        } else if ((Tag == "error" || Tag == "warning") && Fs.size() >= 4) {
          SourceLocation L = locAt(SM, Q.FID, std::stoul(Fs[1]), std::stoul(Fs[2]));
          if (Tag == "error") {
            Diags.error(L, Fs[3]);
            OK = false;
          } else {
            Diags.warning(L, Fs[3]);
          }
        }
      }
    }
    if (Types.empty())
      continue;

    // The generated code: type halves (of the types given members), the
    // declarations, then the method bodies.
    std::string Base = llvm::sys::path::stem(F.Path).str();
    std::string Intf = "{ Generated by p3 for " + Base +
                       ": do not edit (it is rewritten at each build). }\n";
    std::string Impl = Intf;
    bool Halves = false;
    for (const TypeOutput &T : Types) {
      if (T.Members.empty())
        continue;
      if (!Halves)
        Intf += "type\n";
      Halves = true;
      Intf += "  " + T.Name + " = partial " + T.Kind + "\n";
      for (const std::string &M : T.Members)
        Intf += "    " + M + "\n";
      Intf += "  end;\n";
    }
    for (const TypeOutput &T : Types) {
      for (const std::string &D : T.Decls)
        Intf += D + "\n";
      for (const std::string &L : T.Impl)
        Impl += L + "\n";
    }
    llvm::SmallString<256> IntfPath(GenDir), ImplPath(GenDir);
    llvm::sys::path::append(IntfPath, Base + ".intf.inc");
    llvm::sys::path::append(ImplPath, Base + ".impl.inc");
    if (!writeIfChanged(IntfPath, Intf) || !writeIfChanged(ImplPath, Impl))
      return Fail("cannot write '" + IntfPath + "'");

    // The source with the two includes, on the lines of the points they
    // go before (so no line of it moves).
    llvm::StringRef Src = SM.getBufferData(Q.FID);
    auto Off = [&](SourceLocation L) { return (size_t)(L.getPointer() - Src.data()); };
    std::string IncI = "{$I " + std::string(IntfPath) + "}";
    std::string IncM = "{$I " + std::string(ImplPath) + "}";
    std::string Spliced;
    if (auto *UD = llvm::dyn_cast<UnitDecl>(Q.Root)) {
      size_t A = Off(UD->getImplementationLoc()), B = Off(UD->getImplementationDeclsEnd());
      Spliced = Src.substr(0, A).str() + IncI + Src.substr(A, B - A).str() + IncM +
                Src.substr(B).str();
    } else {
      auto *PD = llvm::cast<ProgramDecl>(Q.Root);
      size_t A = Off(PD->getBlock()->getBody()->getBeginLoc());
      Spliced = Src.substr(0, A).str() + IncI + IncM + Src.substr(A).str();
    }
    Splices[absolutePath(F.Path)] = std::move(Spliced);
  }
  return OK;
}
