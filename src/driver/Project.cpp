//===- Project.cpp - p3.json project settings ----------------------------===//
//
// A small JSON reader (strict JSON; strings up to 255 bytes, as in the
// self-hosted compiler, whose selfhost/pcproject.pas this mirrors so both
// report the same diagnostics) and the settings it may hold.
//
//===----------------------------------------------------------------------===//

#include "p3/Driver/Project.h"
#include "p3/Basic/Diagnostics.h"
#include "p3/Basic/SourceManager.h"

#include "llvm/Support/FileSystem.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace p3;

namespace {

//===----------------------------------------------------------------------===//
// Paths (lexical, '/'-separated, as the self-hosted compiler does them)
//===----------------------------------------------------------------------===//

/// Path made absolute (against the current directory) without "." and
/// ".." components.
std::string absolutePath(const std::string &P) {
  std::string Full = P;
  if (Full.empty() || Full[0] != '/') {
    llvm::SmallString<256> Cwd;
    llvm::sys::fs::current_path(Cwd);
    Full = std::string(Cwd) + "/" + P;
  }
  std::vector<std::string> Parts;
  size_t I = 0;
  while (I <= Full.size()) {
    size_t J = Full.find('/', I);
    if (J == std::string::npos)
      J = Full.size();
    std::string C = Full.substr(I, J - I);
    if (C == "..") {
      if (!Parts.empty())
        Parts.pop_back();
    } else if (!C.empty() && C != ".") {
      Parts.push_back(C);
    }
    I = J + 1;
  }
  std::string R;
  for (const std::string &C : Parts)
    R += "/" + C;
  return R.empty() ? "/" : R;
}

/// The directory of an absolute path ("/" for "/x"), or "" for "/".
std::string parentDir(const std::string &P) {
  if (P == "/")
    return "";
  size_t S = P.rfind('/');
  return S == 0 ? "/" : P.substr(0, S);
}

std::string joinPath(const std::string &Dir, const std::string &Name) {
  return Dir.back() == '/' ? Dir + Name : Dir + "/" + Name;
}

bool isRegularFile(const std::string &P) {
  return llvm::sys::fs::is_regular_file(P);
}

//===----------------------------------------------------------------------===//
// JSON
//===----------------------------------------------------------------------===//

struct JValue {
  enum Kind { Object, Array, String, Number, Bool, Null } K = Null;
  size_t Begin = 0, End = 0; ///< offsets in the file
  std::string Text;          ///< String: the value; Number: as written
  bool B = false;
  struct Member {
    std::string Name;
    size_t NameBegin, NameEnd;
    std::unique_ptr<JValue> Value;
  };
  std::vector<Member> Members;                ///< Object
  std::vector<std::unique_ptr<JValue>> Elems; ///< Array
};

class JSONReader {
  llvm::StringRef Data;
  size_t Pos = 0;
  SourceLocation Start;
  DiagnosticsEngine &Diags;
  unsigned Depth = 0;

  SourceLocation locAt(size_t Off) const {
    return Start.getLocWithOffset(static_cast<int>(Off));
  }
  void error(size_t Off, const llvm::Twine &Msg) { Diags.error(locAt(Off), Msg); }

  void skipSpace() {
    while (Pos < Data.size() && (Data[Pos] == ' ' || Data[Pos] == '\t' ||
                                 Data[Pos] == '\n' || Data[Pos] == '\r'))
      ++Pos;
  }
  char peek() const { return Pos < Data.size() ? Data[Pos] : '\0'; }
  bool atEnd() const { return Pos >= Data.size(); }

  static int hexValue(char C) {
    if (C >= '0' && C <= '9')
      return C - '0';
    if (C >= 'a' && C <= 'f')
      return C - 'a' + 10;
    if (C >= 'A' && C <= 'F')
      return C - 'A' + 10;
    return -1;
  }

  /// A string at Pos (its opening quote): its value in Out.
  bool readString(std::string &Out) {
    size_t Begin = Pos++;
    Out.clear();
    while (true) {
      if (atEnd() || Data[Pos] == '\n') {
        error(Begin, "unterminated string");
        return false;
      }
      char C = Data[Pos];
      if (C == '"') {
        ++Pos;
        break;
      }
      if (static_cast<unsigned char>(C) < 0x20) {
        error(Pos, "control character in a string; write it as an escape "
                   "such as \\t");
        return false;
      }
      if (C != '\\') {
        Out += C;
        ++Pos;
        continue;
      }
      size_t EscBegin = Pos++;
      char E = peek();
      ++Pos;
      switch (E) {
      case '"': Out += '"'; break;
      case '\\': Out += '\\'; break;
      case '/': Out += '/'; break;
      case 'b': Out += '\b'; break;
      case 'f': Out += '\f'; break;
      case 'n': Out += '\n'; break;
      case 'r': Out += '\r'; break;
      case 't': Out += '\t'; break;
      case 'u': {
        unsigned V = 0;
        for (int I = 0; I < 4; ++I) {
          int H = hexValue(peek());
          if (H < 0) {
            error(EscBegin, "invalid escape sequence in a string");
            return false;
          }
          V = V * 16 + static_cast<unsigned>(H);
          ++Pos;
        }
        if (V < 0x80) {
          Out += static_cast<char>(V);
        } else if (V < 0x800) {
          Out += static_cast<char>(0xC0 | (V >> 6));
          Out += static_cast<char>(0x80 | (V & 0x3F));
        } else {
          Out += static_cast<char>(0xE0 | (V >> 12));
          Out += static_cast<char>(0x80 | ((V >> 6) & 0x3F));
          Out += static_cast<char>(0x80 | (V & 0x3F));
        }
        break;
      }
      default:
        error(EscBegin, "invalid escape sequence in a string");
        return false;
      }
    }
    if (Out.size() > 255) {
      error(Begin, "string longer than 255 characters");
      return false;
    }
    return true;
  }

  bool isDigit(char C) const { return C >= '0' && C <= '9'; }

  bool readNumber(JValue &V) {
    size_t Begin = Pos;
    if (peek() == '-')
      ++Pos;
    bool OK = isDigit(peek());
    if (peek() == '0') {
      ++Pos;
    } else {
      while (isDigit(peek()))
        ++Pos;
    }
    if (OK && peek() == '.') {
      ++Pos;
      OK = isDigit(peek());
      while (isDigit(peek()))
        ++Pos;
    }
    if (OK && (peek() == 'e' || peek() == 'E')) {
      ++Pos;
      if (peek() == '+' || peek() == '-')
        ++Pos;
      OK = isDigit(peek());
      while (isDigit(peek()))
        ++Pos;
    }
    if (!OK) {
      error(Begin, "invalid number");
      return false;
    }
    V.K = JValue::Number;
    V.Text = Data.substr(Begin, Pos - Begin).str();
    return true;
  }

  bool readWord(JValue &V) {
    size_t Begin = Pos;
    while (peek() >= 'a' && peek() <= 'z')
      ++Pos;
    llvm::StringRef W = Data.substr(Begin, Pos - Begin);
    if (W == "true" || W == "false") {
      V.K = JValue::Bool;
      V.B = W == "true";
      return true;
    }
    if (W == "null") {
      V.K = JValue::Null;
      return true;
    }
    error(Begin, "expected a value (an object, array, string, number, true, "
                 "false or null)");
    return false;
  }

public:
  JSONReader(llvm::StringRef Data, SourceLocation Start, DiagnosticsEngine &D)
      : Data(Data), Start(Start), Diags(D) {}

  std::unique_ptr<JValue> readValue() {
    skipSpace();
    auto V = std::make_unique<JValue>();
    V->Begin = Pos;
    if (++Depth > 64) {
      error(Pos, "values nested too deeply");
      return nullptr;
    }
    bool OK;
    char C = peek();
    if (C == '{') {
      ++Pos;
      V->K = JValue::Object;
      OK = readMembers(*V);
    } else if (C == '[') {
      ++Pos;
      V->K = JValue::Array;
      OK = readElements(*V);
    } else if (C == '"') {
      V->K = JValue::String;
      OK = readString(V->Text);
    } else if (C == '-' || isDigit(C)) {
      OK = readNumber(*V);
    } else if (C >= 'a' && C <= 'z') {
      OK = readWord(*V);
    } else {
      error(Pos, "expected a value (an object, array, string, number, true, "
                 "false or null)");
      OK = false;
    }
    --Depth;
    V->End = Pos;
    return OK ? std::move(V) : nullptr;
  }

  bool readMembers(JValue &V) {
    skipSpace();
    if (peek() == '}') {
      ++Pos;
      return true;
    }
    while (true) {
      skipSpace();
      if (peek() != '"') {
        error(Pos, "expected a string (the name of a setting)");
        return false;
      }
      JValue::Member M;
      M.NameBegin = Pos;
      if (!readString(M.Name))
        return false;
      M.NameEnd = Pos;
      skipSpace();
      if (peek() != ':') {
        error(Pos, "expected ':' after the name");
        return false;
      }
      ++Pos;
      M.Value = readValue();
      if (!M.Value)
        return false;
      V.Members.push_back(std::move(M));
      skipSpace();
      if (peek() == ',') {
        ++Pos;
        continue;
      }
      if (peek() == '}') {
        ++Pos;
        return true;
      }
      error(Pos, "expected ',' or '}'");
      return false;
    }
  }

  bool readElements(JValue &V) {
    skipSpace();
    if (peek() == ']') {
      ++Pos;
      return true;
    }
    while (true) {
      std::unique_ptr<JValue> E = readValue();
      if (!E)
        return false;
      V.Elems.push_back(std::move(E));
      skipSpace();
      if (peek() == ',') {
        ++Pos;
        continue;
      }
      if (peek() == ']') {
        ++Pos;
        return true;
      }
      error(Pos, "expected ',' or ']'");
      return false;
    }
  }

  /// The whole file: one object.
  std::unique_ptr<JValue> readFile() {
    skipSpace();
    if (peek() != '{') {
      error(Pos, "a project file holds an object: { \"main\": ... }");
      return nullptr;
    }
    std::unique_ptr<JValue> V = readValue();
    if (!V)
      return nullptr;
    skipSpace();
    if (!atEnd()) {
      error(Pos, "unexpected text after the project's settings");
      return nullptr;
    }
    return V;
  }
};

//===----------------------------------------------------------------------===//
// Settings
//===----------------------------------------------------------------------===//

/// The settings of a project file, checked.
struct Settings {
  std::optional<std::string> Main, Output, Target;
  std::optional<LangMode> Mode;
  std::optional<unsigned> OptLevel, ErrorLimit;
  std::optional<bool> Debug, SeparateUnits, WarningsAsErrors,
      WarnUnusedVariables, WarnDeprecated, RangeChecks, OverflowChecks;
  std::vector<std::string> UnitPaths, Defines, LibraryPaths, Libraries;
};

class SettingsChecker {
  SourceLocation Start;
  DiagnosticsEngine &Diags;

  SourceRange rangeOf(const JValue &V) const {
    return {Start.getLocWithOffset(static_cast<int>(V.Begin)),
            Start.getLocWithOffset(static_cast<int>(V.End))};
  }
  void bad(const JValue &V, const std::string &Name, const llvm::Twine &What) {
    Diags.error(rangeOf(V).getBegin(), "'" + Name + "' must be " + What,
                {rangeOf(V)});
  }

  std::optional<std::string> string(const JValue &V, const std::string &Name) {
    if (V.K == JValue::String)
      return V.Text;
    bad(V, Name, "a string");
    return std::nullopt;
  }
  std::optional<bool> boolean(const JValue &V, const std::string &Name) {
    if (V.K == JValue::Bool)
      return V.B;
    bad(V, Name, "true or false");
    return std::nullopt;
  }
  /// An integer from 0 to Max (at most 9 digits).
  std::optional<unsigned> integer(const JValue &V, const std::string &Name,
                                  unsigned Max, const char *What) {
    bool OK = V.K == JValue::Number && !V.Text.empty() && V.Text.size() <= 9;
    unsigned N = 0;
    for (char C : V.Text) {
      if (C < '0' || C > '9') {
        OK = false;
        break;
      }
      N = N * 10 + static_cast<unsigned>(C - '0');
    }
    if (OK && N <= Max)
      return N;
    bad(V, Name, What);
    return std::nullopt;
  }
  bool strings(const JValue &V, const std::string &Name,
               std::vector<std::string> &Out) {
    bool OK = V.K == JValue::Array;
    for (size_t I = 0; OK && I < V.Elems.size(); ++I)
      OK = V.Elems[I]->K == JValue::String;
    if (!OK) {
      bad(V, Name, "an array of strings");
      return false;
    }
    for (const auto &E : V.Elems)
      Out.push_back(E->Text);
    return true;
  }

  /// Reports duplicate names among an object's members. False if any.
  bool checkDuplicates(const JValue &Obj, const std::string &Prefix) {
    bool OK = true;
    for (size_t I = 0; I < Obj.Members.size(); ++I)
      for (size_t J = 0; J < I; ++J)
        if (Obj.Members[I].Name == Obj.Members[J].Name) {
          Diags.error(Start.getLocWithOffset(
                          static_cast<int>(Obj.Members[I].NameBegin)),
                      "duplicate setting '" + Prefix + Obj.Members[I].Name +
                          "'");
          OK = false;
          break;
        }
    return OK;
  }

  void unknown(const JValue::Member &M, const std::string &Prefix) {
    Diags.warning(Start.getLocWithOffset(static_cast<int>(M.NameBegin)),
                  "unknown setting '" + Prefix + M.Name + "'; ignored",
                  {SourceRange(Start.getLocWithOffset(static_cast<int>(M.NameBegin)),
                               Start.getLocWithOffset(static_cast<int>(M.NameEnd)))});
  }

public:
  SettingsChecker(SourceLocation Start, DiagnosticsEngine &D)
      : Start(Start), Diags(D) {}

  void check(const JValue &Root, Settings &S) {
    checkDuplicates(Root, "");
    for (const JValue::Member &M : Root.Members) {
      const JValue &V = *M.Value;
      const std::string &N = M.Name;
      if (N == "name" || N == "version" || N == "description") {
        string(V, N); // for people and tools: not used by p3
      } else if (N == "main") {
        S.Main = string(V, N);
      } else if (N == "output") {
        S.Output = string(V, N);
      } else if (N == "target") {
        S.Target = string(V, N);
      } else if (N == "mode") {
        if (V.K == JValue::String && V.Text == "iso")
          S.Mode = LangMode::ISO;
        else if (V.K == JValue::String && V.Text == "tp")
          S.Mode = LangMode::TP;
        else if (V.K == JValue::String && V.Text == "objfpc")
          S.Mode = LangMode::ObjFPC;
        else
          bad(V, N, "\"iso\", \"tp\" or \"objfpc\"");
      } else if (N == "optimize") {
        S.OptLevel = integer(V, N, 3, "an integer from 0 to 3");
      } else if (N == "errorLimit") {
        S.ErrorLimit = integer(V, N, 999999999, "an integer (0: no limit)");
      } else if (N == "debug") {
        S.Debug = boolean(V, N);
      } else if (N == "separateUnits") {
        S.SeparateUnits = boolean(V, N);
      } else if (N == "unitPaths") {
        strings(V, N, S.UnitPaths);
      } else if (N == "defines") {
        strings(V, N, S.Defines);
      } else if (N == "libraryPaths") {
        strings(V, N, S.LibraryPaths);
      } else if (N == "libraries") {
        strings(V, N, S.Libraries);
      } else if (N == "warnings" || N == "checks") {
        if (V.K != JValue::Object) {
          bad(V, N, "an object");
          continue;
        }
        std::string Prefix = N + ".";
        checkDuplicates(V, Prefix);
        for (const JValue::Member &Sub : V.Members) {
          const JValue &SV = *Sub.Value;
          std::string SN = Prefix + Sub.Name;
          if (N == "warnings" && Sub.Name == "asErrors")
            S.WarningsAsErrors = boolean(SV, SN);
          else if (N == "warnings" && Sub.Name == "unusedVariable")
            S.WarnUnusedVariables = boolean(SV, SN);
          else if (N == "warnings" && Sub.Name == "deprecated")
            S.WarnDeprecated = boolean(SV, SN);
          else if (N == "checks" && Sub.Name == "range")
            S.RangeChecks = boolean(SV, SN);
          else if (N == "checks" && Sub.Name == "overflow")
            S.OverflowChecks = boolean(SV, SN);
          else
            unknown(Sub, Prefix);
        }
      } else {
        unknown(M, "");
      }
    }
  }
};

/// A path written in the project file: relative to its directory (Base:
/// "" or ending in '/').
std::string projectPath(const std::string &Base, const std::string &P) {
  return !P.empty() && P[0] == '/' ? P : Base + P;
}

} // namespace

bool p3::applyProjectFile(CompilerOptions &Opts, SourceManager &SM,
                           DiagnosticsEngine &Diags) {
  if (Opts.NoProject)
    return true;
  // The file, and the directory its paths are relative to (Base).
  std::string Path, Base;
  if (!Opts.ProjectFile.empty()) {
    Path = Opts.ProjectFile;
    size_t S = Path.rfind('/');
    Base = S == std::string::npos ? "" : Path.substr(0, S + 1);
  } else {
    std::string Dir;
    if (Opts.InputFile.empty() || Opts.InputFile == "-")
      Dir = absolutePath(".");
    else
      Dir = parentDir(absolutePath(Opts.InputFile));
    std::string Cwd = absolutePath(".");
    for (; !Dir.empty(); Dir = parentDir(Dir))
      if (isRegularFile(joinPath(Dir, "p3.json"))) {
        Base = Dir == Cwd ? "" : joinPath(Dir, "");
        Path = Base + "p3.json";
        break;
      }
    if (Path.empty())
      return true; // no project
  }

  auto FIDOrErr = SM.loadFile(Path);
  if (!FIDOrErr) {
    Diags.error("cannot open project file '" + llvm::Twine(Path) +
                "': " + FIDOrErr.getError().message());
    return false;
  }
  SourceLocation Start = SM.getStartOfFile(*FIDOrErr);
  unsigned ErrorsBefore = Diags.getNumErrors();
  std::unique_ptr<JValue> Root =
      JSONReader(SM.getBufferData(*FIDOrErr), Start, Diags).readFile();
  if (!Root)
    return false;
  Settings S;
  SettingsChecker(Start, Diags).check(*Root, S);
  if (Diags.getNumErrors() != ErrorsBefore)
    return false;

  // Command-line values win; lists: the command line's first.
  bool BuildsMain = Opts.InputFile.empty();
  if (BuildsMain && S.Main)
    Opts.InputFile = projectPath(Base, *S.Main);
  if (BuildsMain && S.Output && !Opts.GivenOutput &&
      Opts.Act == Action::EmitExe) {
    Opts.OutputFile = projectPath(Base, *S.Output);
    size_t Slash = Opts.OutputFile.rfind('/');
    if (Slash != std::string::npos && Slash > 0)
      llvm::sys::fs::create_directories(Opts.OutputFile.substr(0, Slash));
  }
  if (S.Mode && !Opts.GivenMode)
    Opts.Mode = *S.Mode;
  if (S.OptLevel && !Opts.GivenOptLevel)
    Opts.OptLevel = *S.OptLevel;
  if (S.Target && !Opts.GivenTarget)
    Opts.TargetTriple = *S.Target;
  if (S.ErrorLimit && !Opts.GivenErrorLimit)
    Opts.ErrorLimit = *S.ErrorLimit;
  Opts.DebugInfo = Opts.DebugInfo || S.Debug.value_or(false);
  Opts.SeparateUnits = Opts.SeparateUnits || S.SeparateUnits.value_or(false);
  Opts.WarningsAsErrors =
      Opts.WarningsAsErrors || S.WarningsAsErrors.value_or(false);
  Opts.WarnUnusedVariables =
      Opts.WarnUnusedVariables && S.WarnUnusedVariables.value_or(true);
  Opts.WarnDeprecated = Opts.WarnDeprecated && S.WarnDeprecated.value_or(true);
  Opts.RangeChecks = Opts.RangeChecks && S.RangeChecks.value_or(true);
  Opts.OverflowChecks = Opts.OverflowChecks && S.OverflowChecks.value_or(true);
  for (const std::string &P : S.UnitPaths)
    Opts.IncludeDirs.push_back(projectPath(Base, P));
  for (const std::string &P : S.LibraryPaths)
    Opts.LibraryDirs.push_back(projectPath(Base, P));
  Opts.Defines.insert(Opts.Defines.end(), S.Defines.begin(), S.Defines.end());
  Opts.Libraries.insert(Opts.Libraries.end(), S.Libraries.begin(),
                        S.Libraries.end());
  if (BuildsMain && !S.Main) {
    Diags.error("no input file, and the project file '" + llvm::Twine(Path) +
                "' has no \"main\" program");
    return false;
  }
  return true;
}
