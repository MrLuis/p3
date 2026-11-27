// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- Compiletime.cpp - Constants computed by compiletime functions ----===//
//
// `function F(...): T; compiletime;` is a function in a restricted subset
// (Sema checks it) that the compiler runs to compute a constant:
//
//   const CrcTable = BuildCrcTable;      // at the level of a program or unit
//
// After parsing, Driver::evaluateCompiletime writes, for each such
// constant, a small program: the uses of its file, the declarations the
// call needs (constants, types, typed constants and compiletime functions,
// found by the names they mention), a printer of the result as a Pascal
// constant, and a main block that prints F(args). It builds the program
// with this compiler, runs it (time and memory limits, as generators), and
// replaces the declaration in the source with the value:
//
//   const CrcTable: TCrcTable = (0, 1996959894, ...);   // structured: typed
//   const N = 6765;                                     // simple: untyped
//
// The pipeline then starts again on the new text (Driver::run), which keeps
// every line where it was. The program is in <generated dir>/.ct, laid out
// with the declarations on their own lines and columns, so its diagnostics
// are shown with the original file's name. Its output is reused while the
// program, the sources of the units and the compiler are unchanged.
//
//===----------------------------------------------------------------------===//

#include "p3/AST/AST.h"
#include "p3/Driver/Driver.h"
#include "p3/Driver/Generators.h"
#include "p3/Driver/Linker.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

using namespace p3;

namespace {

bool isIdentStart(char C) {
  return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_';
}
bool isIdentChar(char C) { return isIdentStart(C) || (C >= '0' && C <= '9'); }

/// Skips a comment or string starting at I (returns true and moves I past
/// it), or does nothing.
bool skipCommentOrString(llvm::StringRef T, size_t &I) {
  char C = T[I];
  if (C == '{') {
    size_t E = T.find('}', I + 1);
    I = E == llvm::StringRef::npos ? T.size() : E + 1;
    return true;
  }
  if (C == '(' && I + 1 < T.size() && T[I + 1] == '*') {
    size_t E = T.find("*)", I + 2);
    I = E == llvm::StringRef::npos ? T.size() : E + 2;
    return true;
  }
  if (C == '/' && I + 1 < T.size() && T[I + 1] == '/') {
    size_t E = T.find('\n', I);
    I = E == llvm::StringRef::npos ? T.size() : E;
    return true;
  }
  if (C == '\'') {
    ++I;
    while (I < T.size()) {
      if (T[I] == '\'') {
        if (I + 1 < T.size() && T[I + 1] == '\'') {
          I += 2;
          continue;
        }
        ++I;
        break;
      }
      ++I;
    }
    return true;
  }
  return false;
}

/// The identifiers T mentions (lower case), outside comments and strings.
/// (Backtick templates are scanned as code: their ${...} parts count.)
void scanIdentifiers(llvm::StringRef T, std::set<std::string> &Out) {
  for (size_t I = 0; I < T.size();) {
    if (skipCommentOrString(T, I))
      continue;
    char C = T[I];
    if (C >= '0' && C <= '9') { // a number (1e5, 0x..: letters after it)
      while (I < T.size() && (isIdentChar(T[I]) || T[I] == '.'))
        ++I;
      continue;
    }
    if (C == '$' || C == '#' || C == '%') { // $FF, #13, %101
      ++I;
      while (I < T.size() && (isIdentChar(T[I]) || T[I] == '$'))
        ++I;
      continue;
    }
    if (isIdentStart(C)) {
      size_t S = I;
      while (I < T.size() && isIdentChar(T[I]))
        ++I;
      Out.insert(T.slice(S, I).lower());
      continue;
    }
    ++I;
  }
}

/// The end of a routine heading starting at T: just after its first ';'
/// outside parentheses, comments and strings.
size_t headingEnd(llvm::StringRef T) {
  int Depth = 0;
  for (size_t I = 0; I < T.size();) {
    if (skipCommentOrString(T, I))
      continue;
    char C = T[I++];
    if (C == '(' || C == '[')
      ++Depth;
    else if ((C == ')' || C == ']') && Depth)
      --Depth;
    else if (C == ';' && !Depth)
      return I;
  }
  return T.size();
}

/// The compiler directives of T ({$...}) other than includes and
/// conditional compilation, in order.
std::string copiedDirectives(llvm::StringRef T) {
  std::string R;
  for (size_t I = 0; I < T.size();) {
    if (T[I] == '{' && I + 1 < T.size() && T[I + 1] == '$') {
      size_t E = T.find('}', I);
      if (E == llvm::StringRef::npos)
        break;
      llvm::StringRef D = T.slice(I + 2, E);
      size_t N = 0;
      while (N < D.size() && isIdentChar(D[N]))
        ++N;
      std::string Name = D.substr(0, N).lower();
      bool Skip = Name == "i" || Name == "include" || Name == "ifdef" ||
                  Name == "ifndef" || Name == "if" || Name == "ifopt" ||
                  Name == "else" || Name == "elseif" || Name == "endif" ||
                  Name == "ifend" || Name == "mode";
      if (!Skip && D.find('\n') == llvm::StringRef::npos)
        R += T.slice(I, E + 1).str();
      I = E + 1;
      continue;
    }
    if (skipCommentOrString(T, I))
      continue;
    ++I;
  }
  return R;
}

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

bool readText(llvm::StringRef Path, std::string &Out) {
  auto B = llvm::MemoryBuffer::getFile(Path);
  if (!B)
    return false;
  Out = (*B)->getBuffer().str();
  return true;
}

bool writeText(llvm::StringRef Path, llvm::StringRef Text) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC, llvm::sys::fs::OF_Text);
  if (EC)
    return false;
  OS << Text;
  return true;
}

const char *modeName(LangMode M) {
  switch (M) {
  case LangMode::ISO: return "iso";
  case LangMode::TP: return "tp";
  default: return "objfpc";
  }
}

/// One top-level declaration of a file, as the evaluation program may copy
/// it.
struct TopDecl {
  Decl *D;
  unsigned Order;           ///< position in the file
  const char *Section;      ///< "const", "type" or "" (a routine)
  const char *Begin, *End;  ///< its text
  std::string Replacement;  ///< a computed constant's text, if any
  bool Pending = false;     ///< a compiletime constant not computed yet
  bool HeadingOnly = false; ///< a routine heading: copied as `forward`
  /// A routine that is not compiletime, or a variable: copied (a heading
  /// marked `external`; `name: type`) for Sema to say they cannot be used.
  bool Unusable = false;
};

/// A source file of the compilation: a program or a unit.
struct FileInfo {
  Decl *Root;
  FileID FID;
  std::string Path;
  std::vector<TopDecl> Decls;
  std::vector<std::string> Uses;
  llvm::StringMap<std::vector<unsigned>> ByName; ///< lower name -> Decls
};

/// The end of an expression or type: its range's end.
const char *endOf(SourceRange R) { return R.getEnd().getPointer(); }

class Evaluator {
  SourceManager &SM;
  DiagnosticsEngine &Diags;
  const CompilerOptions &Opts;
  std::vector<FileInfo> Files;
  /// The compiletime functions, by lower-case name.
  llvm::StringMap<std::vector<RoutineDecl *>> Compiletime;
  /// The file each top-level declaration is in.
  llvm::DenseMap<Decl *, FileInfo *> FileOf;

public:
  Evaluator(SourceManager &SM, DiagnosticsEngine &Diags,
            const CompilerOptions &Opts)
      : SM(SM), Diags(Diags), Opts(Opts) {}

  void addFile(Decl *Root);
  bool any() const { return !Compiletime.empty(); }
  /// Computes the constants; Splices gets the new text of each file that
  /// had some. False after an error.
  bool run(llvm::StringMap<std::string> &Splices,
           llvm::function_ref<std::string(llvm::StringRef)> AbsPath);

private:
  bool isCompiletimeConst(FileInfo &F, TopDecl &T, RoutineDecl *&Fn);
  bool evaluate(FileInfo &F, TopDecl &X, RoutineDecl *Fn, std::string &Value,
                TypeSpec *&ResultTS);
  TypeDecl *findType(llvm::StringRef Lower, FileInfo *Prefer);
  bool printer(TypeSpec *TS, const std::string &E, FileInfo *Home,
               std::string &Code, std::string &Locals, unsigned &Counter,
               std::string &Err, llvm::StringRef EnumName = "");
  bool isStructured(TypeSpec *TS, FileInfo *Home);
};

void Evaluator::addFile(Decl *Root) {
  Files.push_back({});
  FileInfo &F = Files.back();
  F.Root = Root;
  F.FID = SM.getFileID(Root->getLocation());
  F.Path = SM.getFilename(F.FID).str();
  std::vector<Block *> Blocks;
  if (auto *P = llvm::dyn_cast<ProgramDecl>(Root)) {
    Blocks.push_back(P->getBlock());
    for (const Identifier &U : P->getUses())
      F.Uses.push_back(U.Spelling.str());
  } else if (auto *U = llvm::dyn_cast<UnitDecl>(Root)) {
    Blocks.push_back(U->getInterface());
    Blocks.push_back(U->getImplementation());
    for (const Identifier &I : U->getInterfaceUses())
      F.Uses.push_back(I.Spelling.str());
    for (const Identifier &I : U->getImplementationUses())
      F.Uses.push_back(I.Spelling.str());
  }
  unsigned Order = 0;
  for (Block *B : Blocks) {
    if (!B)
      continue;
    for (Decl *D : B->getDecls()) {
      TopDecl T{D, Order++, "", nullptr, nullptr, "", false, false};
      if (auto *C = llvm::dyn_cast<ConstDecl>(D)) {
        if (!C->getInit())
          continue;
        T.Section = "const";
        T.Begin = C->getName().Loc.getPointer();
        T.End = endOf(C->getInit()->getRange());
      } else if (auto *TD = llvm::dyn_cast<TypeDecl>(D)) {
        if (!TD->getTypeSpec() || TD->isGeneric())
          continue;
        T.Section = "type";
        T.Begin = TD->getName().Loc.getPointer();
        T.End = endOf(TD->getTypeSpec()->getRange());
        if (auto *E = llvm::dyn_cast<EnumTypeSpec>(TD->getTypeSpec()))
          if (!E->isScoped())
            for (const Identifier &N : E->getNames())
              F.ByName[N.Name].push_back(F.Decls.size());
      } else if (auto *V = llvm::dyn_cast<VarDecl>(D)) {
        if (!V->getTypeSpec())
          continue;
        T.Begin = V->getName().Loc.getPointer();
        if (V->isTypedConst()) {
          T.Section = "const";
          T.End = endOf(V->getInit()->getRange());
        } else { // (`a, b: T`: each alone)
          T.Section = "var";
          T.End = T.Begin + V->getName().Spelling.size();
          T.Replacement = V->getName().Spelling.str() + ": " +
                          std::string(V->getTypeSpec()->getBeginLoc().getPointer(),
                                      endOf(V->getTypeSpec()->getRange()));
          T.Unusable = true;
        }
      } else if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) {
        if (R->getClassName().isValid() || R->isGeneric())
          continue;
        if (R->isCompiletime())
          Compiletime[R->getName().Name].push_back(R);
        T.Begin = R->getLocation().getPointer();
        T.Unusable = !R->isCompiletime();
        if (Block *Body = R->getBody(); Body && Body->getBody() &&
                                        R->isCompiletime()) {
          T.End = endOf(Body->getBody()->getRange());
        } else {
          llvm::StringRef Rest(T.Begin, SM.getBufferData(SM.getFileID(
                                            R->getLocation())).end() - T.Begin);
          T.End = T.Begin + headingEnd(Rest);
          T.HeadingOnly = !R->getBody();
        }
      } else {
        continue;
      }
      if (!T.Begin || !T.End || T.End < T.Begin)
        continue;
      F.ByName[D->getName().Name].push_back(F.Decls.size());
      F.Decls.push_back(T);
    }
  }
}

bool Evaluator::isCompiletimeConst(FileInfo &F, TopDecl &T, RoutineDecl *&Fn) {
  auto *C = llvm::dyn_cast<ConstDecl>(T.D);
  if (!C)
    return false;
  Identifier Id;
  if (auto *Call = llvm::dyn_cast<CallExpr>(C->getInit()))
    Id = Call->getCallee();
  else if (auto *N = llvm::dyn_cast<NameRefExpr>(C->getInit()))
    Id = N->getName();
  else
    return false;
  auto It = Compiletime.find(Id.Name);
  if (It == Compiletime.end())
    return false;
  // (a routine of the file with that name that is not compiletime hides it)
  auto Own = F.ByName.find(Id.Name);
  if (Own != F.ByName.end()) {
    bool Hidden = true;
    for (unsigned I : Own->second)
      if (auto *R = llvm::dyn_cast<RoutineDecl>(F.Decls[I].D);
          R && R->isCompiletime())
        Hidden = false;
    if (Hidden)
      return false;
  }
  Fn = It->second.front();
  return true;
}

TypeDecl *Evaluator::findType(llvm::StringRef Lower, FileInfo *Prefer) {
  auto Look = [&](FileInfo &F) -> TypeDecl * {
    auto It = F.ByName.find(Lower);
    if (It == F.ByName.end())
      return nullptr;
    for (unsigned I : It->second)
      if (auto *TD = llvm::dyn_cast<TypeDecl>(F.Decls[I].D);
          TD && TD->getName().Name == Lower)
        return TD;
    return nullptr;
  };
  if (Prefer)
    if (TypeDecl *TD = Look(*Prefer))
      return TD;
  for (FileInfo &F : Files)
    if (TypeDecl *TD = Look(F))
      return TD;
  return nullptr;
}

/// The predeclared types a printer knows: 'i'nteger, 'r'eal, 's'tring,
/// 'c'har, 'b'oolean; 0 if not one.
char predeclaredKind(llvm::StringRef N) {
  static const std::map<std::string, char> Kinds = {
      {"integer", 'i'},  {"longint", 'i'},  {"int64", 'i'},
      {"shortint", 'i'}, {"smallint", 'i'}, {"byte", 'i'},
      {"word", 'i'},     {"cardinal", 'i'}, {"longword", 'i'},
      {"dword", 'i'},    {"qword", 'i'},    {"uint64", 'i'},
      {"int8", 'i'},     {"int16", 'i'},    {"int32", 'i'},
      {"uint8", 'i'},    {"uint16", 'i'},   {"uint32", 'i'},
      {"nativeint", 'i'}, {"nativeuint", 'i'}, {"sizeint", 'i'},
      {"sizeuint", 'i'}, {"ptrint", 'i'},   {"ptruint", 'i'},
      {"real", 'r'},     {"double", 'r'},   {"single", 'r'},
      {"extended", 'r'}, {"string", 's'},   {"ansistring", 's'},
      {"shortstring", 's'}, {"rawbytestring", 's'}, {"utf8string", 's'},
      {"char", 'c'},     {"ansichar", 'c'}, {"widechar", 'c'},
      {"boolean", 'b'},  {"bytebool", 'b'}, {"wordbool", 'b'},
      {"longbool", 'b'}};
  auto It = Kinds.find(N.str());
  return It == Kinds.end() ? 0 : It->second;
}

std::string textOf(SourceRange R) {
  return std::string(R.getBegin().getPointer(),
                     R.getEnd().getPointer() - R.getBegin().getPointer());
}

bool Evaluator::isStructured(TypeSpec *TS, FileInfo *Home) {
  for (int Guard = 0; TS && Guard < 64; ++Guard) {
    if (llvm::isa<ArrayTypeSpec>(TS) || llvm::isa<RecordTypeSpec>(TS))
      return true;
    auto *N = llvm::dyn_cast<NamedTypeSpec>(TS);
    if (!N)
      return false;
    TypeDecl *TD = findType(N->getName().Name, Home);
    TS = TD ? TD->getTypeSpec() : nullptr;
  }
  return false;
}

/// Pascal statements that write the value of E (of type TS) as a constant.
bool Evaluator::printer(TypeSpec *TS, const std::string &E, FileInfo *Home,
                        std::string &Code, std::string &Locals,
                        unsigned &Counter, std::string &Err,
                        llvm::StringRef EnumName) {
  if (auto *N = llvm::dyn_cast<NamedTypeSpec>(TS)) {
    llvm::StringRef L = N->getName().Name;
    if (N->getQualifiers().empty())
      if (TypeDecl *TD = findType(L, Home))
        return printer(TD->getTypeSpec(), E, FileOf.lookup(TD), Code, Locals,
                       Counter, Err, TD->getName().Spelling);
    switch (predeclaredKind(L)) {
    case 'i': Code += "write(" + E + ");"; return true;
    case 'r': Code += "p3ct_real(" + E + ");"; return true;
    case 's': Code += "p3ct_str(" + E + ");"; return true;
    case 'c': Code += "write('#', ord(" + E + "));"; return true;
    case 'b':
      Code += "if " + E + " then write('True') else write('False');";
      return true;
    default:
      Err = "type '" + N->getName().Spelling.str() +
            "' is not declared in the program or its units' sources, nor "
            "an ordinal, real or string type";
      return false;
    }
  }
  if (auto *En = llvm::dyn_cast<EnumTypeSpec>(TS)) {
    if (En->isScoped() && EnumName.empty()) {
      Err = "an anonymous scoped enumeration cannot be written as a constant";
      return false;
    }
    std::string Prefix = En->isScoped() ? EnumName.str() + "." : "";
    Code += "case ord(" + E + ") of ";
    unsigned I = 0;
    for (const Identifier &V : En->getNames())
      Code += std::to_string(I++) + ": write('" + Prefix + V.Spelling.str() +
              "'); ";
    Code += "end;";
    return true;
  }
  if (auto *S = llvm::dyn_cast<SubrangeTypeSpec>(TS)) {
    std::string Lo = textOf(S->getLo()->getRange());
    if (!Lo.empty() && (Lo[0] == '\'' || Lo[0] == '#')) {
      Code += "write('#', ord(" + E + "));";
      return true;
    }
    if (auto *NR = llvm::dyn_cast<NameRefExpr>(S->getLo()))
      for (FileInfo &F : Files) {
        auto It = F.ByName.find(NR->getName().Name);
        if (It == F.ByName.end())
          continue;
        for (unsigned I : It->second)
          if (auto *TD = llvm::dyn_cast<TypeDecl>(F.Decls[I].D))
            if (llvm::isa<EnumTypeSpec>(TD->getTypeSpec()))
              return printer(TD->getTypeSpec(), E, &F, Code, Locals, Counter,
                             Err, TD->getName().Spelling);
      }
    Code += "write(" + E + ");";
    return true;
  }
  if (llvm::isa<StringTypeSpec>(TS)) {
    Code += "p3ct_str(" + E + ");";
    return true;
  }
  if (auto *A = llvm::dyn_cast<ArrayTypeSpec>(TS)) {
    std::string Elem = E;
    std::string Close;
    for (TypeSpec *Index : A->getIndexes()) {
      std::string K = std::to_string(Counter++);
      std::string T = "p3ct_t" + K, I = "p3ct_i" + K;
      Locals += "type " + T + " = " + textOf(Index->getRange()) + "; var " +
                I + ": " + T + "; ";
      Code += "write('('); for " + I + " := low(" + T + ") to high(" + T +
              ") do begin if " + I + " <> low(" + T + ") then write(', '); ";
      Elem += "[" + I + "]";
      Close = "end; write(')');" + Close;
    }
    if (!printer(A->getElement(), Elem, Home, Code, Locals, Counter, Err))
      return false;
    Code += Close;
    return true;
  }
  if (auto *R = llvm::dyn_cast<RecordTypeSpec>(TS)) {
    FieldList *FL = R->getFields();
    if (FL && FL->Variant) {
      Err = "a record with variant parts cannot be written as a constant";
      return false;
    }
    if (!FL || FL->Fixed.empty()) {
      Err = "a record without fields cannot be written as a constant";
      return false;
    }
    Code += "write('(');";
    bool First = true;
    for (FieldDecl *Fd : FL->Fixed) {
      Code += std::string("write('") + (First ? "" : "; ") +
              Fd->getName().Spelling.str() + ": ');";
      First = false;
      if (!printer(Fd->getTypeSpec(), E + "." + Fd->getName().Spelling.str(),
                   Home, Code, Locals, Counter, Err))
        return false;
    }
    Code += "write(')');";
    return true;
  }
  if (llvm::isa<SetTypeSpec>(TS)) { // (p3 has no set constants yet)
    Err = "a set cannot be written as a constant";
    return false;
  }
  Err = "a value of type '" + textOf(TS->getRange()) +
        "' cannot be written as a constant (pointers, objects, dynamic "
        "arrays, files and procedural types cannot)";
  return false;
}

const char *PrinterHelpers =
    "procedure p3ct_str(const s: ansistring);\n"
    "var i: longint; q: boolean;\n"
    "begin\n"
    "  if length(s) = 0 then begin write(''''''); exit end;\n"
    "  q := false;\n"
    "  for i := 1 to length(s) do\n"
    "    if (ord(s[i]) >= 32) and (ord(s[i]) < 127) then begin\n"
    "      if not q then begin write(''''); q := true end;\n"
    "      if s[i] = '''' then write('''''') else write(s[i]);\n"
    "    end else begin\n"
    "      if q then begin write(''''); q := false end;\n"
    "      write('#', ord(s[i]));\n"
    "    end;\n"
    "  if q then write('''');\n"
    "end;\n"
    "procedure p3ct_real(x: double);\n"
    "begin\n"
    "  if (x <> x) or (x - x <> 0) then write('@p3ct-nonfinite@')\n"
    "  else write(x:30);\n"
    "end;\n";

bool Evaluator::evaluate(FileInfo &F, TopDecl &X, RoutineDecl *Fn,
                         std::string &Value, TypeSpec *&ResultTS) {
  namespace fs = llvm::sys::fs;
  auto *C = llvm::cast<ConstDecl>(X.D);
  SourceLocation XLoc = C->getName().Loc;
  ResultTS = Fn->getResultTypeSpec();
  auto Fail = [&](const llvm::Twine &M) {
    Diags.error(XLoc, "cannot compute constant '" + C->getName().Spelling +
                          "': " + M,
                {C->getName().getRange()});
    return false;
  };
  if (SM.getFileID(XLoc) != F.FID)
    return Fail("a compiletime constant must be in the file itself, not in "
                "an included one");
  if (!ResultTS)
    return Fail("'" + Fn->getName().Spelling + "' has no result type");

  // --- the declarations it needs ----------------------------------------
  std::set<std::string> Names;
  const char *CallBegin = C->getInit()->getBeginLoc().getPointer();
  std::string CallText(CallBegin, X.End - CallBegin);
  scanIdentifiers(CallText, Names);
  std::set<unsigned> Included;
  std::vector<std::string> Work(Names.begin(), Names.end());
  while (!Work.empty()) {
    std::string N = Work.back();
    Work.pop_back();
    auto It = F.ByName.find(N);
    if (It == F.ByName.end())
      continue;
    for (unsigned I : It->second) {
      TopDecl &T = F.Decls[I];
      if (&T == &X)
        return Fail("its value depends on itself");
      if (auto *R = llvm::dyn_cast<RoutineDecl>(T.D); R && !R->isCompiletime()) {
        // (the definition of a compiletime heading is marked on either)
        bool CT = false;
        for (RoutineDecl *Y : Compiletime.lookup(N))
          CT |= Y->getName().Name == R->getName().Name;
        if (CT) { // a heading or the definition, without the directive
          T.Unusable = false;
          if (Block *B = R->getBody(); B && B->getBody())
            T.End = endOf(B->getBody()->getRange());
        }
      }
      if (T.Pending)
        return Fail("it uses compiletime constant '" +
                    T.D->getName().Spelling + "', declared after it");
      if (!Included.insert(I).second || T.Unusable)
        continue;
      std::set<std::string> More;
      scanIdentifiers(T.Replacement.empty()
                          ? llvm::StringRef(T.Begin, T.End - T.Begin)
                          : llvm::StringRef(T.Replacement),
                      More);
      for (const std::string &M : More)
        if (!Names.count(M)) {
          Names.insert(M);
          Work.push_back(M);
        }
    }
  }

  // --- the program, with each declaration on its own line and column -----
  llvm::StringRef Src = SM.getBufferData(F.FID);
  std::string Out = "program p3ct;";
  std::vector<std::string> Uses;
  for (const std::string &U : F.Uses)
    if (std::find_if(Uses.begin(), Uses.end(), [&](const std::string &V) {
          return llvm::StringRef(V).equals_insensitive(U);
        }) == Uses.end())
      Uses.push_back(U);
  if (!Uses.empty()) {
    Out += " uses ";
    for (size_t I = 0; I < Uses.size(); ++I)
      Out += (I ? ", " : "") + Uses[I];
    Out += ";";
  }
  Out += copiedDirectives(Src.substr(0, X.Begin - Src.data()));
  unsigned Line = 1, Col = Out.size() + 1;
  auto Append = [&](llvm::StringRef S) {
    for (char Ch : S) {
      if (Ch == '\n') {
        ++Line;
        Col = 1;
      } else {
        ++Col;
      }
    }
    Out += S.str();
  };
  for (unsigned I : Included) {
    TopDecl &T = F.Decls[I];
    std::string Text = T.Replacement.empty()
                           ? std::string(T.Begin, T.End - T.Begin)
                           : T.Replacement;
    if (T.Unusable && llvm::isa<RoutineDecl>(T.D))
      Text += " external;";
    else if (T.HeadingOnly)
      Text += " compiletime; forward;";
    else
      Text += ";";
    std::string Kw = *T.Section ? std::string(" ") + T.Section : "";
    SourceLocation L = SourceLocation::fromPointer(T.Begin);
    PresumedLoc P = SM.getPresumedLoc(L);
    if (SM.getFileID(L) == F.FID && P.Line > Line) {
      Append(Kw);
      Append(std::string(P.Line - Line, '\n'));
      Append(std::string(P.Column - 1, ' '));
    } else {
      Append("\n" + Kw + " ");
    }
    Append(Text);
  }
  std::string Locals, Code;
  unsigned Counter = 0;
  std::string Err;
  if (!printer(ResultTS, "p3ct_v", FileOf.lookup(Fn), Code, Locals, Counter,
               Err))
    return Fail("the result of '" + Fn->getName().Spelling + "': " + Err);
  Append("\n");
  Append(PrinterHelpers);
  Append("procedure p3ct_out(const p3ct_v: " + textOf(ResultTS->getRange()) +
         ");\n" + Locals + "\nbegin\n  " + Code + "\nend;\n");
  Append("begin\n  write('@p3ct@');\n  p3ct_out(" + CallText +
         ");\n  writeln;\nend.\n");

  // --- build and run it, unless its output is known ----------------------
  std::string GenDir = Opts.GeneratedDir;
  if (GenDir.empty()) {
    llvm::SmallString<256> D(llvm::sys::path::parent_path(Opts.InputFile));
    llvm::sys::path::append(D, "gen");
    GenDir = std::string(D);
  }
  llvm::SmallString<256> Dir(GenDir);
  llvm::sys::fs::make_absolute(Dir);
  std::string GenAbs(Dir);
  llvm::sys::path::append(Dir, ".ct");
  if (auto EC = fs::create_directories(Dir))
    return Fail("cannot create '" + Dir + "': " + EC.message());
  std::string Base = (generatedFileBase(F.Path) + "." +
                      C->getName().Spelling)
                         .str();
  llvm::SmallString<256> SrcPath(Dir), Exe(Dir);
  llvm::sys::path::append(SrcPath, Base + ".pas");
  llvm::sys::path::append(Exe, Base);
  // (the program is Base.exe on Windows; its output and stamp are named
  // after Base, the same on every host)
  std::string ExeS = hostExecutable(Exe), OutPath = std::string(Exe) + ".out",
              ErrPath = std::string(Exe) + ".err", StampPath = std::string(Exe) + ".stamp";

  std::string Self = selfExecutable();
  // (without warnings about the variables copied only for diagnostics)
  std::vector<std::string> Args{Self, "--no-project", "-O0",
                                "--Wno-unused-variable",
                                std::string("--mode=") + modeName(Opts.Mode),
                                "--generated-dir=" + GenAbs};
  llvm::SmallString<256> FileDir(llvm::sys::path::parent_path(F.Path));
  fs::make_absolute(FileDir);
  llvm::SmallString<256> InputDir(llvm::sys::path::parent_path(Opts.InputFile));
  fs::make_absolute(InputDir);
  Args.push_back("-I" + std::string(FileDir));
  if (InputDir != FileDir)
    Args.push_back("-I" + std::string(InputDir));
  for (const std::string &D : Opts.IncludeDirs) {
    llvm::SmallString<256> A(D);
    fs::make_absolute(A);
    Args.push_back("-I" + std::string(A));
  }
  for (const std::string &D : Opts.Defines)
    Args.push_back("-d" + D);
  for (const std::string &G : Opts.Generators) {
    llvm::SmallString<256> A(G);
    if (!isLibraryGenerator(G))
      fs::make_absolute(A);
    Args.push_back("--generator=" + std::string(A));
  }
  Args.push_back("--generator-timeout=" +
                 std::to_string(Opts.GeneratorTimeout));
  Args.push_back("-o");
  Args.push_back(ExeS);
  Args.push_back(std::string(SrcPath));

  // The key: the program, the options, the sources of the compilation's
  // files and the compiler itself.
  uint64_t Key = fnv(Out);
  for (const std::string &A : Args)
    Key = fnv(A, Key);
  for (FileInfo &G : Files)
    Key = fnv(SM.getBufferData(G.FID), Key);
  fs::file_status St;
  if (!fs::status(Self, St))
    Key = fnv(std::to_string(St.getSize()) + ":" +
                  std::to_string(St.getLastModificationTime()
                                     .time_since_epoch()
                                     .count()),
              Key);
  std::string Stamp = hex16(Key), Old, Output;
  if (!(readText(StampPath, Old) && Old == Stamp && readText(OutPath, Output))) {
    if (!writeText(SrcPath, Out))
      return Fail("cannot write '" + SrcPath + "'");
    std::vector<llvm::StringRef> Refs(Args.begin(), Args.end());
    std::vector<std::optional<llvm::StringRef>> Redirects = {
        std::nullopt, llvm::StringRef(ErrPath), llvm::StringRef(ErrPath)};
    std::string ExecErr;
    int RC = llvm::sys::ExecuteAndWait(Self, Refs, std::nullopt, Redirects, 0,
                                       0, &ExecErr);
    if (RC != 0) {
      // Its diagnostics, with the source file's name (the lines are the
      // same).
      std::string Text;
      readText(ErrPath, Text);
      std::string From = std::string(SrcPath) + ":";
      std::string Shown = F.Path + ":";
      for (size_t P = 0; (P = Text.find(From, P)) != std::string::npos;
           P += Shown.size())
        Text.replace(P, From.size(), Shown);
      llvm::errs() << Text;
      return Fail("the program that computes it (" + SrcPath +
                  ") did not compile");
    }
    llvm::StringRef ExeRef(ExeS);
    Redirects = {llvm::StringRef(""), llvm::StringRef(OutPath),
                 llvm::StringRef(ErrPath)};
    RC = llvm::sys::ExecuteAndWait(ExeRef, {ExeRef}, std::nullopt, Redirects,
                                   Opts.GeneratorTimeout, /*MemoryLimit=*/2048,
                                   &ExecErr);
    if (RC == -2)
      return Fail("'" + Fn->getName().Spelling + "' ran longer than " +
                  std::to_string(Opts.GeneratorTimeout) + " seconds");
    if (RC != 0) {
      std::string Text;
      readText(ErrPath, Text);
      for (size_t P = 0; (P = Text.find(SrcPath.str().str(), P)) != std::string::npos;
           P += F.Path.size())
        Text.replace(P, SrcPath.size(), F.Path);
      while (!Text.empty() && (Text.back() == '\n' || Text.back() == '\r'))
        Text.pop_back();
      return Fail("'" + Fn->getName().Spelling + "' failed (exit status " +
                  std::to_string(RC) + ")" + (Text.empty() ? "" : ": " + Text));
    }
    if (!readText(OutPath, Output))
      return Fail("cannot read '" + OutPath + "'");
    writeText(StampPath, Stamp);
  }
  size_t M = Output.rfind("@p3ct@");
  if (M == std::string::npos)
    return Fail("its program wrote no value");
  size_t E = Output.find('\n', M);
  Value = Output.substr(M + 6, E == std::string::npos ? std::string::npos
                                                      : E - M - 6);
  while (!Value.empty() && (Value.back() == '\r' || Value.back() == ' '))
    Value.pop_back();
  if (Value.find("@p3ct-nonfinite@") != std::string::npos)
    return Fail("its value is not a finite number");
  return true;
}

bool Evaluator::run(llvm::StringMap<std::string> &Splices,
                    llvm::function_ref<std::string(llvm::StringRef)> AbsPath) {
  for (FileInfo &F : Files)
    for (TopDecl &T : F.Decls)
      FileOf[T.D] = &F;
  bool OK = true;
  for (FileInfo &F : Files) {
    // Pending first: a constant may not use one computed after it.
    std::vector<std::pair<unsigned, RoutineDecl *>> Todo;
    for (unsigned I = 0; I < F.Decls.size(); ++I) {
      RoutineDecl *Fn = nullptr;
      if (isCompiletimeConst(F, F.Decls[I], Fn)) {
        F.Decls[I].Pending = true;
        Todo.push_back({I, Fn});
      }
    }
    if (Todo.empty())
      continue;
    struct Edit {
      size_t Off, Len;
      std::string Text;
    };
    std::vector<Edit> Edits;
    llvm::StringRef Src = SM.getBufferData(F.FID);
    for (auto [I, Fn] : Todo) {
      TopDecl &X = F.Decls[I];
      X.Pending = false;
      std::string Value;
      TypeSpec *RTS = nullptr;
      if (!evaluate(F, X, Fn, Value, RTS)) {
        OK = false;
        continue;
      }
      if (!OK) // (only the errors of the first constant that failed)
        continue;
      const Identifier &Name = X.D->getName();
      std::string New = Name.Spelling.str();
      if (isStructured(RTS, FileOf.lookup(Fn)))
        New += ": " + textOf(RTS->getRange()) + " = " + Value;
      else
        New += " = " + Value;
      // (the lines it took stay: the following ones do not move)
      llvm::StringRef Old(X.Begin, X.End - X.Begin);
      New += std::string(Old.count('\n'), '\n');
      X.Replacement = New;
      Edits.push_back({(size_t)(X.Begin - Src.data()), Old.size(), New});
    }
    if (!OK) // (the files after it would report the same errors again)
      return false;
    if (Edits.empty())
      continue;
    std::string Text;
    size_t At = 0;
    for (const Edit &E : Edits) {
      Text += Src.substr(At, E.Off - At).str() + E.Text;
      At = E.Off + E.Len;
    }
    Text += Src.substr(At).str();
    Splices[AbsPath(F.Path)] = std::move(Text);
  }
  return OK;
}

} // namespace

bool Driver::evaluateCompiletime(Decl *Root, llvm::ArrayRef<UnitDecl *> Units,
                                 bool &Restart) {
  Restart = false;
  Evaluator E(SM, Diags, Opts);
  for (UnitDecl *U : Units)
    if (!llvm::StringRef(SM.getFilename(SM.getFileID(U->getLocation())))
             .ends_with(".p3i"))
      E.addFile(U);
  E.addFile(Root);
  if (!E.any())
    return true;
  llvm::StringMap<std::string> New;
  bool OK = E.run(New, [&](llvm::StringRef P) { return absolutePath(P); });
  for (auto &KV : New)
    Splices[KV.getKey()] = KV.getValue();
  Restart = OK && !New.empty();
  return OK;
}
