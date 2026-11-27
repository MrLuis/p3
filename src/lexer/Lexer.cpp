// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "p3/Lexer/Lexer.h"
#include "p3/Basic/Diagnostics.h"
#include "p3/Basic/Directives.h"
#include "p3/Basic/SourceManager.h"
#include "p3/Basic/StringPool.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"

using namespace p3;

static bool isIdentStart(char C) { return llvm::isAlpha(C) || C == '_'; }
static bool isIdentBody(char C) { return llvm::isAlnum(C) || C == '_'; }

Lexer::Lexer(const SourceManager &SM, FileID FID, DiagnosticsEngine &Diags,
             StringPool &Strings, const LangOptions &LangOpts)
    : Diags(Diags), Strings(Strings), LangOpts(LangOpts),
      InlineOn(LangOpts.Mode == LangMode::ObjFPC),
      COperators(LangOpts.Mode != LangMode::ISO),
      ResultSwitch(LangOpts.Mode == LangMode::ObjFPC),
      OutSwitch(LangOpts.Mode == LangMode::ObjFPC),
      DefaultParamsSwitch(LangOpts.Mode == LangMode::ObjFPC) {
  llvm::StringRef Buf = SM.getBufferData(FID);
  Cur = BufBegin = Buf.data();
  BufEnd = Buf.data() + Buf.size();
  // MemoryBuffers are NUL-terminated, so peeking one char past a token
  // (e.g. Cur[1]) is always safe, even at BufEnd.
  NameScopes.emplace_back(); // the file's own declarations (declared())
  Defines.insert("P3");
  Defines.insert(LangOpts.Mode == LangMode::ISO  ? "P3_ISO"
                 : LangOpts.Mode == LangMode::TP ? "P3_TP"
                                                 : "P3_OBJFPC");
}

void Lexer::setDefines(llvm::ArrayRef<std::string> Names) {
  for (const std::string &N : Names)
    Defines.insert(llvm::StringRef(N).upper());
}

void Lexer::formToken(Token &T, tok::TokenKind K, const char *Start) {
  T.Kind = K;
  T.Loc = locOf(Start);
  T.Length = static_cast<unsigned>(Cur - Start);
  T.Data = {};
  T.IntValue = 0;
  T.Based = false;
  T.WideChar = 0;
  T.Template = false;
}

//===----------------------------------------------------------------------===//
// Whitespace and comments
//===----------------------------------------------------------------------===//

void Lexer::skipBlockComment(unsigned OpenLen, llvm::StringRef Close) {
  const char *Start = Cur;
  llvm::StringRef Rest(Cur + OpenLen, BufEnd - (Cur + OpenLen));
  size_t Pos = Rest.find(Close);
  if (LangOpts.Mode == LangMode::ISO) {
    // ISO 7185 6.1.8: '{' / '(*' and '}' / '*)' are equivalent, so either
    // closer ends either kind of comment. (Turbo/FPC pair them strictly.)
    llvm::StringRef Other = Close == "}" ? "*)" : "}";
    size_t OtherPos = Rest.find(Other);
    if (OtherPos < Pos) {
      Pos = OtherPos;
      Close = Other;
    }
  }
  if (Pos == llvm::StringRef::npos) {
    Diags.error(locOf(Start), "unterminated comment",
                {SourceRange(locOf(Start), locOf(Start + OpenLen))});
    Cur = BufEnd;
    return;
  }
  // A comment starting with '$' is a compiler directive: {$R-}.
  if (Rest.starts_with("$"))
    handleDirective(Start, Rest.slice(1, Pos));
  Cur = Rest.data() + Pos + Close.size();
  if (!PendingInclude.empty())
    enterPendingInclude(); // the included text follows the comment
}

void Lexer::enterPendingInclude() {
  std::string Name = std::move(PendingInclude);
  PendingInclude.clear();
  if (IncludeStack.size() >= 16) {
    Diags.error(PendingIncludeLoc, "include files are nested too deeply");
    return;
  }
  // Next to the including file, then the -I directories.
  llvm::SmallVector<std::string, 4> Dirs;
  FileID FID = IncludeSM->getFileID(locOf(BufBegin));
  Dirs.push_back(
      llvm::sys::path::parent_path(IncludeSM->getFilename(FID)).str());
  Dirs.append(IncludeDirs.begin(), IncludeDirs.end());
  std::string Found;
  for (const std::string &Dir : Dirs) {
    llvm::SmallString<256> Path(Dir);
    if (llvm::sys::path::is_absolute(Name)) // (generated code is included so)
      Path = Name;
    else
      llvm::sys::path::append(Path, Name);
    if (llvm::sys::fs::is_regular_file(Path)) {
      Found = std::string(Path);
      break;
    }
  }
  if (Found.empty()) {
    Diags.error(PendingIncludeLoc,
                "cannot find include file '" + Name +
                    "' (looked next to this file and in -I directories)");
    return;
  }
  auto IncOrErr = IncludeSM->loadFile(Found);
  if (!IncOrErr) {
    Diags.error(PendingIncludeLoc, "cannot open include file '" + Found + "'");
    return;
  }
  IncludeStack.push_back({BufBegin, BufEnd, Cur});
  llvm::StringRef Buf = IncludeSM->getBufferData(*IncOrErr);
  Cur = BufBegin = Buf.data();
  BufEnd = Buf.data() + Buf.size();
}

/// {$modeswitch NAME[+|-| ON| OFF]} with Free Pascal 3.2.2's names:
/// advancedrecords and typehelpers (objfpc), result, out and
/// defaultparameters (any mode), and p3's own racechecks (objfpc), turn
/// their features on or off; the others
/// p3's mode decides (class, objpas, properties, exceptions: objfpc; cvar,
/// initfinal, hintdirective, allowinline: always) or p3 lacks: a request
/// for what p3 does is accepted, any other is reported and ignored.
void Lexer::handleModeSwitch(const char *Start, llvm::StringRef Arg) {
  llvm::StringRef Name = Arg.take_while(isIdentBody);
  llvm::StringRef Rest = Arg.drop_front(Name.size()).trim();
  bool On;
  if (Rest.empty() || Rest == "+" || Rest.equals_insensitive("on"))
    On = true;
  else if (Rest == "-" || Rest.equals_insensitive("off"))
    On = false;
  else {
    Diags.warning(locOf(Start), "expected {$modeswitch NAME} (with +, -, ON "
                                "or OFF); ignored");
    return;
  }
  std::string N = Name.lower();
  bool ObjFPC = LangOpts.Mode == LangMode::ObjFPC;
  if (N == "advancedrecords") {
    AdvancedRecords = On;
    return;
  }
  if (N == "typehelpers") {
    TypeHelpers = On;
    return;
  }
  if (N == "racechecks") { // p3's own: spawn blocks share only shared data
    RaceChecks = On;
    return;
  }
  if (N == "result") {
    ResultSwitch = On;
    return;
  }
  if (N == "out") {
    OutSwitch = On;
    return;
  }
  if (N == "defaultparameters") {
    DefaultParamsSwitch = On;
    return;
  }
  // The others: what p3's mode decides, what it always does, or a
  // feature it lacks.
  enum { ByMode, Always, Lacking } Kind;
  if (N == "class" || N == "objpas" || N == "properties" || N == "exceptions")
    Kind = ByMode;
  else if (N == "cvar" || N == "initfinal" || N == "hintdirective" ||
           N == "allowinline")
    Kind = Always;
  else if (N == "pchartostring" || N == "nestedcomments" ||
           N == "classicprocvars" || N == "macprocvars" ||
           N == "repeatforward" || N == "pointertoprocvar" ||
           N == "autoderef" || N == "ansistrings" || N == "duplicatelocals" ||
           N == "objectivec1" || N == "objectivec2" || N == "nestedprocvars" ||
           N == "nonlocalgoto" || N == "isounaryminus" ||
           N == "systemcodepage" || N == "finalfields" ||
           N == "unicodestrings" || N == "cblocks" || N == "isoio" ||
           N == "isoprogramparas" || N == "isomod" || N == "arrayoperators")
    Kind = Lacking;
  else {
    Diags.warning(locOf(Start), "unknown mode switch '" + Name + "'; ignored");
    return;
  }
  if (Kind == ByMode && On != ObjFPC)
    Diags.warning(locOf(Start),
                  "mode switch '" + N + "' cannot be turned " +
                      (On ? "on in --mode=tp; use --mode=objfpc"
                          : "off in --mode=objfpc; use --mode=tp") +
                      " (ignored)");
  else if (Kind == Always && !On)
    Diags.warning(locOf(Start),
                  "mode switch '" + N + "' cannot be turned off; ignored");
  else if (Kind == Lacking && On)
    Diags.warning(locOf(Start),
                  "mode switch '" + N + "' is not supported; ignored");
}

void Lexer::handleDirective(const char *Start, llvm::StringRef Text) {
  llvm::StringRef Word = Text.take_while(llvm::isAlpha);
  llvm::StringRef Arg = Text.drop_front(Word.size()).trim();
  if (handleConditional(Start, Word, Arg))
    return;
  if (!isActive())
    return; // in a branch that is not compiled
  // {$I file} / {$INCLUDE file}: include a source file.
  if ((Word.equals_insensitive("i") || Word.equals_insensitive("include")) &&
      !Arg.empty() && Arg != "+" && Arg != "-") {
    if (Arg.size() >= 2 && Arg.front() == '\'' && Arg.back() == '\'')
      Arg = Arg.drop_front().drop_back();
    if (!IncludeSM) {
      Diags.warning(locOf(Start), "include directive ignored here");
      return;
    }
    PendingInclude = Arg.str();
    PendingIncludeLoc = locOf(Start);
    return;
  }
  // {$interfaces com|corba}: the kind of the interfaces declared after it.
  if (Word.equals_insensitive("interfaces")) {
    if (Arg.equals_insensitive("corba"))
      CorbaInterfaces = true;
    else if (Arg.equals_insensitive("com"))
      CorbaInterfaces = false;
    else
      Diags.error(locOf(Start), "expected {$interfaces com} or "
                                "{$interfaces corba}");
    return;
  }

  // {$push} / {$pop}: save and restore the local switches (at most 21
  // levels, as in FPC).
  // {$nullsafe on|off|+|-} (p3's own, objfpc): in the routines that start
  // after it, a plain class or interface type is never nil (T? may be).
  if (Word.equals_insensitive("nullsafe")) {
    llvm::StringRef A = Arg.trim();
    bool On;
    if (A.equals_insensitive("on") || A == "+")
      On = true;
    else if (A.equals_insensitive("off") || A == "-")
      On = false;
    else {
      Diags.warning(locOf(Start), "expected {$nullsafe on} or {$nullsafe off}; "
                                  "ignored");
      return;
    }
    if (LangOpts.Mode != LangMode::ObjFPC) {
      Diags.warning(locOf(Start), "{$nullsafe} needs --mode=objfpc; ignored");
      return;
    }
    NullSafe = On;
    return;
  }

  if (Word.equals_insensitive("push") && Arg.empty()) {
    if (SwitchStack.size() >= 21) {
      Diags.error(locOf(Start), "too many levels of {$push} (at most 21)");
      return;
    }
    SavedSwitches S{LongStrings, CorbaInterfaces, InlineOn, ScopedEnums,
                    NullSafe, PackRecords, CheckDirectives::Default,
                    CheckDirectives::Default, CheckDirectives::Default};
    if (Directives) {
      S.Range = Directives->stateAt(CheckDirectives::Range, locOf(Start));
      S.Overflow = Directives->stateAt(CheckDirectives::Overflow, locOf(Start));
      S.TypedAddress =
          Directives->stateAt(CheckDirectives::TypedAddress, locOf(Start));
    }
    SwitchStack.push_back(S);
    return;
  }
  if (Word.equals_insensitive("pop") && Arg.empty()) {
    if (SwitchStack.empty()) {
      Diags.error(locOf(Start), "{$pop} without a preceding {$push}");
      return;
    }
    SavedSwitches S = SwitchStack.pop_back_val();
    LongStrings = S.LongStrings;
    CorbaInterfaces = S.CorbaInterfaces;
    InlineOn = S.InlineOn;
    ScopedEnums = S.ScopedEnums;
    NullSafe = S.NullSafe;
    PackRecords = S.PackRecords;
    if (Directives) {
      Directives->addState(locOf(Start), BufBegin, BufEnd,
                           CheckDirectives::Range, S.Range);
      Directives->addState(locOf(Start), BufBegin, BufEnd,
                           CheckDirectives::Overflow, S.Overflow);
      Directives->addState(locOf(Start), BufBegin, BufEnd,
                           CheckDirectives::TypedAddress, S.TypedAddress);
    }
    return;
  }

  // {$packrecords N|C|DEFAULT|NORMAL}, {$align N|ON|OFF}, {$aN}, {$a+|-}:
  // how the fields of the records declared after it are aligned (as in
  // FPC, where ON and + mean 4, OFF and - mean 1).
  auto PackValue = [](llvm::StringRef A) -> int {
    unsigned N;
    if (A.getAsInteger(10, N))
      return -1;
    return N == 1 || N == 2 || N == 4 || N == 8 || N == 16 || N == 32
               ? static_cast<int>(N)
               : -1;
  };
  if (Word.equals_insensitive("packrecords")) {
    if (Arg.equals_insensitive("c") || Arg.equals_insensitive("default") ||
        Arg.equals_insensitive("normal"))
      PackRecords = 0;
    else if (int N = PackValue(Arg); N > 0)
      PackRecords = static_cast<unsigned>(N);
    else
      Diags.error(locOf(Start), "illegal record alignment '" + Arg +
                                    "': use 1, 2, 4, 8, 16, 32, C, DEFAULT "
                                    "or NORMAL");
    return;
  }
  if (Word.equals_insensitive("align")) {
    if (Arg.equals_insensitive("on") || Arg == "+")
      PackRecords = 4;
    else if (Arg.equals_insensitive("off") || Arg == "-")
      PackRecords = 1;
    else if (int N = PackValue(Arg); N > 0)
      PackRecords = static_cast<unsigned>(N);
    else
      Diags.error(locOf(Start), "illegal record alignment '" + Arg +
                                    "': use 1, 2, 4, 8, 16, 32, ON or OFF");
    return;
  }
  if (Word.equals_insensitive("a") && PackValue(Arg) > 0) { // {$A4}
    PackRecords = static_cast<unsigned>(PackValue(Arg));
    return;
  }

  // {$scopedenums on|off}: whether the values of the enumerations declared
  // after it are reached only through their type (TColor.Red), as in FPC.
  if (Word.equals_insensitive("scopedenums")) {
    if (Arg.equals_insensitive("on") || Arg == "+")
      ScopedEnums = true;
    else if (Arg.equals_insensitive("off") || Arg == "-")
      ScopedEnums = false;
    else
      Diags.error(locOf(Start),
                  "expected {$scopedenums on} or {$scopedenums off}");
    return;
  }

  // {$inline on|off}: whether the `inline` directive inlines (default: on in
  // objfpc mode, as in Free Pascal).
  if (Word.equals_insensitive("inline")) {
    if (Arg.equals_insensitive("on") || Arg == "+")
      InlineOn = true;
    else if (Arg.equals_insensitive("off") || Arg == "-")
      InlineOn = false;
    else
      Diags.error(locOf(Start), "expected {$inline on} or {$inline off}");
    return;
  }

  // {$coperators on|off}: whether += -= *= /= are assignments (default: on
  // except in ISO mode; as in Free Pascal 3.2, {$pop} does not restore it).
  if (Word.equals_insensitive("coperators")) {
    if (Arg.equals_insensitive("on") || Arg == "+")
      COperators = true;
    else if (Arg.equals_insensitive("off") || Arg == "-")
      COperators = false;
    else
      Diags.error(locOf(Start),
                  "expected {$coperators on} or {$coperators off}");
    return;
  }

  // {$macro on|off}: whether macros ({$define NAME := value}) are defined
  // and expanded.
  if (Word.equals_insensitive("macro")) {
    if (Arg.equals_insensitive("on") || Arg == "+")
      MacrosOn = true;
    else if (Arg.equals_insensitive("off") || Arg == "-")
      MacrosOn = false;
    else
      Diags.error(locOf(Start), "expected {$macro on} or {$macro off}");
    return;
  }

  if (Word.equals_insensitive("modeswitch")) {
    handleModeSwitch(Start, Arg);
    return;
  }

  // {$typedaddress on|off}: the long form of {$T+|-}.
  if (Word.equals_insensitive("typedaddress")) {
    bool On;
    if (Arg.equals_insensitive("on") || Arg == "+")
      On = true;
    else if (Arg.equals_insensitive("off") || Arg == "-")
      On = false;
    else {
      Diags.error(locOf(Start),
                  "expected {$typedaddress on} or {$typedaddress off}");
      return;
    }
    if (Directives)
      Directives->add(locOf(Start), BufBegin, BufEnd,
                      CheckDirectives::TypedAddress, On);
    return;
  }

  // {$longstrings on|off}: the long form of {$H+|-}.
  if (Word.equals_insensitive("longstrings") &&
      (Arg.equals_insensitive("on") || Arg.equals_insensitive("off"))) {
    LongStrings = Arg.equals_insensitive("on");
    return;
  }

  // Switch list: letter followed by + or -, comma separated: $R-,Q+
  llvm::SmallVector<llvm::StringRef, 4> Parts;
  Text.split(Parts, ',');
  for (llvm::StringRef P : Parts) {
    P = P.trim();
    if (P.size() == 2 && llvm::isAlpha(P[0]) && (P[1] == '+' || P[1] == '-')) {
      char Letter = llvm::toUpper(P[0]);
      bool On = P[1] == '+';
      if (Letter == 'H')
        LongStrings = On;
      if (Letter == 'A') // {$A+}: as {$A4} (FPC); {$A-}: no alignment
        PackRecords = On ? 4 : 1;
      if (Directives && (Letter == 'R' || Letter == 'Q' || Letter == 'T'))
        Directives->add(locOf(Start), BufBegin, BufEnd,
                        Letter == 'R' ? CheckDirectives::Range
                        : Letter == 'Q' ? CheckDirectives::Overflow
                                        : CheckDirectives::TypedAddress,
                        On);
      continue; // other switches (listing, boolean evaluation, ...) ignored
    }
    if (P.empty())
      continue;
    // Listing / other compiler switches ({$l-}, {$I file}, ...) are ignored.
    Diags.warning(locOf(Start),
                  "compiler directive '$" + P + "' is not supported; ignored");
    break;
  }
}

//===----------------------------------------------------------------------===//
// Conditional compilation
//===----------------------------------------------------------------------===//

namespace {
/// A value in a {$IF} expression.
struct CondValue {
  enum Kind { Bool, Int, Str } K = Bool;
  bool B = false;
  int64_t I = 0;
  std::string S;
  static CondValue boolean(bool V) {
    CondValue C;
    C.B = V;
    return C;
  }
  static CondValue integer(int64_t V) {
    CondValue C;
    C.K = Int;
    C.I = V;
    return C;
  }
};

/// The size in bytes of the predefined type named Upper (upper case) in
/// tp and objfpc modes, or -1. Must agree with sizeof in the generated code:
/// $IF is evaluated while lexing, before any type is known.
static int64_t predefinedSize(llvm::StringRef Upper) {
  return llvm::StringSwitch<int64_t>(Upper)
      .Cases({"CHAR", "ANSICHAR", "BOOLEAN", "BYTE", "SHORTINT"}, 1)
      .Cases({"WORD", "SMALLINT"}, 2)
      .Cases({"INTEGER", "LONGINT", "CARDINAL", "LONGWORD", "DWORD"}, 4)
      .Case("THANDLE", 4)
      .Cases({"INT64", "REAL", "DOUBLE", "POINTER"}, 8)
      .Cases({"PCHAR", "ANSISTRING", "SIZEINT", "PTRINT"}, 8)
      .Cases({"NATIVEINT", "PANSICHAR", "PGUID"}, 8)
      .Cases({"QWORD", "UINT64", "NATIVEUINT", "PTRUINT", "SIZEUINT"}, 8)
      .Case("TGUID", 16)
      .Case("SHORTSTRING", 256)
      .Default(-1);
}

/// Evaluates the expression of {$IF} / {$ELSEIF}, with Pascal's operator
/// precedence: values (integers, 'strings', true, false, defined(NAME),
/// sizeof(predefined type), macros), not, * div mod and, + - or, and one comparison = <> < > <= >=
/// or `in [values]`. A macro's value is an integer if its text is an
/// integer literal, a string if it is a string literal, the value of the
/// macro it names if it is a macro's name, else its text (a string), as in
/// Free Pascal. The first error is kept.
class ConditionParser {
  llvm::StringRef S;
  size_t P = 0;
  const llvm::StringSet<> &Defines;
  const llvm::StringMap<std::string> &Values; ///< macros (upper case)
  bool HasSizeOf; ///< sizeof exists in this mode (not in iso)
  Lexer *Lex;     ///< for declared(NAME); may be null
  unsigned Depth;

  llvm::StringRef peek() {
    while (P < S.size() && llvm::isSpace(S[P]))
      ++P;
    if (P >= S.size())
      return {};
    size_t E = P;
    char C = S[E];
    if (isIdentStart(C)) {
      while (E < S.size() && isIdentBody(S[E]))
        ++E;
    } else if (llvm::isDigit(C)) {
      while (E < S.size() && llvm::isDigit(S[E]))
        ++E;
    } else if (C == '$') {
      ++E;
      while (E < S.size() && llvm::isHexDigit(S[E]))
        ++E;
    } else if (C == '\'') {
      ++E;
      while (E < S.size()) {
        if (S[E++] != '\'')
          continue;
        if (E < S.size() && S[E] == '\'')
          ++E; // '' stands for '
        else
          break;
      }
    } else if ((C == '<' && E + 1 < S.size() && (S[E + 1] == '=' || S[E + 1] == '>')) ||
               (C == '>' && E + 1 < S.size() && S[E + 1] == '=') ||
               (C == '.' && E + 1 < S.size() && S[E + 1] == '.')) {
      E += 2;
    } else {
      ++E;
    }
    return S.slice(P, E);
  }
  bool accept(llvm::StringRef W) {
    if (!peek().equals_insensitive(W))
      return false;
    P += W.size();
    return true;
  }
  CondValue fail(const std::string &Msg) {
    if (Error.empty())
      Error = Msg;
    return {};
  }
  CondValue invalid() {
    return fail("invalid $IF expression (expected values, macros, "
                "defined(NAME), comparisons, + - * div mod, not, and, or)");
  }
  bool bothAre(const CondValue &L, const CondValue &R, CondValue::Kind K) {
    if (L.K == K && R.K == K)
      return true;
    fail("incompatible types in $IF expression");
    return false;
  }

  /// -1, 0, 1: L compared with R (of the same kind).
  static int compare(const CondValue &L, const CondValue &R) {
    if (L.K == CondValue::Str)
      return L.S < R.S ? -1 : L.S > R.S ? 1 : 0;
    if (L.K == CondValue::Int)
      return L.I < R.I ? -1 : L.I > R.I ? 1 : 0;
    return static_cast<int>(L.B) - static_cast<int>(R.B);
  }

  CondValue parseExpr() {
    CondValue L = parseSimple();
    if (!Error.empty())
      return {};
    llvm::StringRef Op = peek();
    if (Op.equals_insensitive("in")) { // x in [a, b, lo..hi]
      P += Op.size();
      if (!accept("["))
        return invalid();
      bool In = false;
      if (!accept("]")) {
        do {
          CondValue Lo = parseSimple(), Hi;
          bool Range = Error.empty() && accept("..");
          if (Range)
            Hi = parseSimple();
          if (!Error.empty())
            return {};
          if (Lo.K != L.K || (Range && Hi.K != L.K))
            return fail("incompatible types in $IF expression");
          if (Range ? compare(Lo, L) <= 0 && compare(L, Hi) <= 0
                    : compare(Lo, L) == 0)
            In = true;
        } while (accept(","));
        if (!accept("]"))
          return invalid();
      }
      return CondValue::boolean(In);
    }
    if (Op != "=" && Op != "<>" && Op != "<" && Op != ">" && Op != "<=" &&
        Op != ">=")
      return L;
    P += Op.size();
    CondValue R = parseSimple();
    if (!Error.empty())
      return {};
    if (L.K != R.K)
      return fail("incompatible types in $IF expression");
    int C = compare(L, R);
    bool V = Op == "="    ? C == 0
             : Op == "<>" ? C != 0
             : Op == "<"  ? C < 0
             : Op == ">"  ? C > 0
             : Op == "<=" ? C <= 0
                          : C >= 0;
    return CondValue::boolean(V);
  }

  CondValue parseSimple() {
    bool Neg = false;
    if (accept("-"))
      Neg = true;
    else
      accept("+");
    CondValue V = parseTerm();
    if (!Error.empty())
      return {};
    if (Neg) {
      if (V.K != CondValue::Int)
        return fail("incompatible types in $IF expression");
      if (V.I == INT64_MIN)
        return fail("integer overflow in $IF expression");
      V.I = -V.I;
    }
    while (Error.empty()) {
      llvm::StringRef Op = peek();
      if (Op != "+" && Op != "-" && !Op.equals_insensitive("or"))
        break;
      P += Op.size();
      CondValue R = parseTerm();
      if (!Error.empty())
        return {};
      if (Op.equals_insensitive("or")) {
        if (bothAre(V, R, CondValue::Bool))
          V.B = V.B || R.B;
      } else if (Op == "+" && V.K == CondValue::Str && R.K == CondValue::Str) {
        V.S += R.S;
      } else if (bothAre(V, R, CondValue::Int)) {
        int64_t X;
        if (Op == "+" ? llvm::AddOverflow(V.I, R.I, X)
                      : llvm::SubOverflow(V.I, R.I, X))
          return fail("integer overflow in $IF expression");
        V.I = X;
      }
    }
    return Error.empty() ? V : CondValue();
  }

  CondValue parseTerm() {
    CondValue V = parseFactor();
    while (Error.empty()) {
      llvm::StringRef Op = peek();
      if (Op != "*" && !Op.equals_insensitive("div") &&
          !Op.equals_insensitive("mod") && !Op.equals_insensitive("and"))
        break;
      P += Op.size();
      CondValue R = parseFactor();
      if (!Error.empty())
        return {};
      if (Op.equals_insensitive("and")) {
        if (bothAre(V, R, CondValue::Bool))
          V.B = V.B && R.B;
        continue;
      }
      if (!bothAre(V, R, CondValue::Int))
        break;
      if (Op == "*") {
        int64_t X;
        if (llvm::MulOverflow(V.I, R.I, X))
          return fail("integer overflow in $IF expression");
        V.I = X;
        continue;
      }
      if (R.I == 0)
        return fail("division by zero in $IF expression");
      if (V.I == INT64_MIN && R.I == -1)
        return fail("integer overflow in $IF expression");
      V.I = Op.equals_insensitive("div") ? V.I / R.I : V.I % R.I;
    }
    return Error.empty() ? V : CondValue();
  }

  CondValue parseFactor() {
    if (accept("not")) {
      CondValue V = parseFactor();
      if (!Error.empty())
        return {};
      if (V.K != CondValue::Bool)
        return fail("incompatible types in $IF expression");
      V.B = !V.B;
      return V;
    }
    if (accept("(")) {
      CondValue V = parseExpr();
      if (Error.empty() && !accept(")"))
        return invalid();
      return V;
    }
    llvm::StringRef T = peek();
    if (T.empty())
      return invalid();
    if (llvm::isDigit(T[0]) || T[0] == '$') {
      P += T.size();
      uint64_t U;
      if (T[0] == '$' ? T.drop_front().getAsInteger(16, U)
                      : T.getAsInteger(10, U))
        return invalid();
      if (U > static_cast<uint64_t>(INT64_MAX))
        return fail("integer overflow in $IF expression");
      return CondValue::integer(static_cast<int64_t>(U));
    }
    if (T[0] == '\'') {
      P += T.size();
      if (T.size() < 2 || T.back() != '\'')
        return invalid();
      CondValue V;
      V.K = CondValue::Str;
      for (size_t I = 1; I + 1 < T.size(); ++I) {
        V.S += T[I];
        if (T[I] == '\'')
          ++I; // ''
      }
      return V;
    }
    if (accept("true"))
      return CondValue::boolean(true);
    if (accept("false"))
      return CondValue::boolean(false);
    if (accept("defined")) {
      if (!accept("("))
        return invalid();
      llvm::StringRef Name = peek();
      if (Name.empty() || !isIdentStart(Name[0]))
        return invalid();
      P += Name.size();
      if (!accept(")"))
        return invalid();
      return CondValue::boolean(Defines.contains(Name.upper()));
    }
    if (accept("declared")) { // declared(NAME): an identifier declared so far
      if (!accept("("))
        return invalid();
      llvm::StringRef Name = peek();
      if (Name.empty() || !isIdentStart(Name[0]))
        return invalid();
      P += Name.size();
      if (!accept(")"))
        return invalid();
      return CondValue::boolean(Lex && Lex->isDeclared(Name.lower()));
    }
    if (HasSizeOf && !Values.count("SIZEOF") && accept("sizeof")) {
      if (!accept("("))
        return invalid();
      llvm::StringRef Name = peek();
      if (Name.empty() || !isIdentStart(Name[0]))
        return invalid();
      P += Name.size();
      if (!accept(")"))
        return invalid();
      int64_t Size = predefinedSize(Name.upper());
      if (Size < 0)
        return fail("'sizeof' in $IF needs a predefined type, not '" +
                    Name.str() + "'");
      return CondValue::integer(Size);
    }
    if (!isIdentStart(T[0]))
      return invalid();
    P += T.size();
    auto It = Values.find(T.upper());
    if (It == Values.end())
      return fail("'" + T.str() +
                  "' has no value in $IF (a macro needs {$macro on} and "
                  "{$define " + T.str() + " := value})");
    return macroValue(It->second);
  }

  /// The value of a macro whose text is Text (see the class comment).
  CondValue macroValue(llvm::StringRef Text) {
    if (Depth >= 16)
      return fail("macros in $IF expressions are nested too deeply");
    ConditionParser Sub(Text, Defines, Values, HasSizeOf, Lex, Depth + 1);
    llvm::StringRef T = Sub.peek();
    bool Single = !T.empty() && Sub.P + T.size() == Text.size();
    if (Single && (llvm::isDigit(T[0]) || T[0] == '$' || T[0] == '\'')) {
      CondValue V = Sub.parseFactor(); // an integer or string literal
      if (Sub.Error.empty())
        return V;
    } else if (Single && isIdentStart(T[0]) && Values.count(T.upper())) {
      // another macro's value
      CondValue V = Sub.macroValue(Values.find(T.upper())->second);
      if (!Sub.Error.empty())
        return fail(Sub.Error);
      return V;
    }
    CondValue V; // its text
    V.K = CondValue::Str;
    V.S = Text.str();
    return V;
  }

public:
  std::string Error;
  ConditionParser(llvm::StringRef S, const llvm::StringSet<> &Defines,
                  const llvm::StringMap<std::string> &Values, bool HasSizeOf,
                  Lexer *Lex, unsigned Depth = 0)
      : S(S), Defines(Defines), Values(Values), HasSizeOf(HasSizeOf), Lex(Lex),
        Depth(Depth) {}
  bool parse() {
    CondValue V = parseExpr();
    if (Error.empty() && !peek().empty())
      invalid();
    if (Error.empty() && V.K != CondValue::Bool)
      fail("the $IF expression is not boolean");
    return Error.empty() && V.B;
  }
};
} // namespace

bool Lexer::isDeclared(llvm::StringRef Name) {
  if (BeforeDeclared)
    BeforeDeclared();
  for (const llvm::StringSet<> &S : NameScopes)
    if (S.contains(Name))
      return true;
  return Predeclared && Predeclared->contains(Name);
}

bool Lexer::evalCondition(const char *Start, llvm::StringRef Expr) {
  llvm::StringMap<std::string> Values;
  for (const auto &M : Macros)
    Values[M.getKey()] = std::string(M.getValue().Begin, M.getValue().End);
  ConditionParser CP(Expr, Defines, Values, LangOpts.Mode != LangMode::ISO, this);
  bool V = CP.parse();
  if (CP.Error.empty())
    return V;
  Diags.error(locOf(Start), CP.Error);
  return false;
}

/// A use of macro NAME (the identifier token T, starting at Start): lexing
/// continues in its value; false if NAME is not a macro.
bool Lexer::expandMacro(const Token &T, const char *Start) {
  auto It = Macros.find(llvm::StringRef(Start, T.Length).upper());
  if (It == Macros.end())
    return false;
  // A macro used at the end of another: that one is done.
  while (!MacroStack.empty() && Cur >= BufEnd) {
    MacroFrame F = MacroStack.pop_back_val();
    BufEnd = F.BufEnd;
    Cur = F.Cur;
  }
  if (MacroStack.size() >= 16) {
    Diags.warning(locOf(Start), "expanding macros exceeds a depth of 16");
    return false;
  }
  MacroStack.push_back({BufEnd, Cur});
  Cur = It->second.Begin;
  BufEnd = It->second.End;
  return true;
}

bool Lexer::handleConditional(const char *Start, llvm::StringRef Word,
                              llvm::StringRef Arg) {
  enum Kind { If, IfDef, IfNDef, IfOpt, ElseIf, Else, EndIf, Define, Undef };
  static const struct {
    const char *Spelling, *Name;
    Kind K;
  } Table[] = {{"if", "$IF", If},           {"ifdef", "$IFDEF", IfDef},
               {"ifndef", "$IFNDEF", IfNDef}, {"ifopt", "$IFOPT", IfOpt},
               {"elseif", "$ELSEIF", ElseIf}, {"else", "$ELSE", Else},
               {"endif", "$ENDIF", EndIf},    {"ifend", "$IFEND", EndIf},
               {"define", "$DEFINE", Define}, {"undef", "$UNDEF", Undef}};
  const char *Name = nullptr;
  Kind K = If;
  for (const auto &E : Table)
    if (Word.equals_insensitive(E.Spelling)) {
      Name = E.Name;
      K = E.K;
    }
  if (!Name)
    return false;

  bool Active = isActive();
  unsigned Depth = IncludeStack.size();
  // The symbol named by $DEFINE, $UNDEF, $IFDEF and $IFNDEF.
  llvm::StringRef Sym;
  if (!Arg.empty() && isIdentStart(Arg[0]))
    Sym = Arg.take_while(isIdentBody);
  auto NeedSymbol = [&] {
    if (!Sym.empty())
      return true;
    Diags.error(locOf(Start),
                "expected a symbol name after " + llvm::Twine(Name));
    return false;
  };

  switch (K) {
  case Define:
  case Undef:
    if (!Active || !NeedSymbol())
      return true;
    if (K == Undef) {
      Defines.erase(Sym.upper());
      Macros.erase(Sym.upper());
      return true;
    }
    Defines.insert(Sym.upper());
    Macros.erase(Sym.upper());
    if (llvm::StringRef Rest = Arg.drop_front(Sym.size()).ltrim();
        Rest.starts_with(":=")) { // a macro: {$define NAME := value}
      if (!MacrosOn) {
        Diags.warning(locOf(Start),
                      "macro support is off: '" + Sym +
                          "' is defined without a value (use {$macro on})");
        return true;
      }
      llvm::StringRef Value = Rest.drop_front(2).trim();
      Macros[Sym.upper()] = {Value.begin(), Value.end()};
    }
    return true;
  case If:
  case IfDef:
  case IfNDef:
  case IfOpt: {
    bool C = false;
    if (Active) {
      if (K == If)
        C = evalCondition(Start, Arg);
      else if (K == IfOpt)
        Diags.error(locOf(Start), "$IFOPT is not supported");
      else if (NeedSymbol())
        C = Defines.contains(Sym.upper()) == (K == IfDef);
    }
    Conditionals.push_back(
        {locOf(Start), Name, Active, Active && C, Active && C, false, Depth});
    return true;
  }
  case ElseIf:
  case Else:
  case EndIf:
    break;
  }

  // $ELSEIF, $ELSE, $ENDIF: they continue a conditional of the same file.
  if (Conditionals.empty() || Conditionals.back().Depth != Depth) {
    Diags.error(locOf(Start),
                llvm::Twine(Name) + " without a matching $IF or $IFDEF");
    return true;
  }
  Conditional &C = Conditionals.back();
  if (K == EndIf) {
    Conditionals.pop_back();
    return true;
  }
  if (C.SeenElse) {
    Diags.error(locOf(Start), llvm::Twine(Name) + " after $ELSE");
    C.Active = false;
    return true;
  }
  if (K == ElseIf && llvm::StringRef(C.Name) != "$IF") {
    Diags.error(locOf(Start), "$ELSEIF cannot continue " + llvm::Twine(C.Name) +
                                  "; use $ELSE");
    C.Active = false;
    return true;
  }
  if (K == Else) {
    C.SeenElse = true;
    C.Active = C.ParentActive && !C.Taken;
  } else {
    C.Active = C.ParentActive && !C.Taken && evalCondition(Start, Arg);
  }
  C.Taken = C.Taken || C.Active;
  return true;
}

void Lexer::skipInactive() {
  // Only comments (which may be conditional directives), strings and //
  // comments matter here; everything else is skipped unchecked.
  while (Cur < BufEnd && !isActive()) {
    char C = *Cur;
    if (C == '{') {
      skipBlockComment(1, "}");
    } else if (C == '(' && Cur[1] == '*') {
      skipBlockComment(2, "*)");
    } else if (C == '/' && Cur[1] == '/' && LangOpts.Mode != LangMode::ISO) {
      while (Cur < BufEnd && *Cur != '\n')
        ++Cur;
    } else if (C == '\'') {
      ++Cur;
      while (Cur < BufEnd && *Cur != '\'' && *Cur != '\n')
        ++Cur;
      if (Cur < BufEnd && *Cur == '\'')
        ++Cur;
    } else if (C == '`' && LangOpts.Mode != LangMode::ISO) {
      // a template string's text, to its next backtick (which need not be
      // its last: what matters is that braces in it open no comment)
      ++Cur;
      while (Cur < BufEnd && *Cur != '`')
        ++Cur;
      if (Cur < BufEnd)
        ++Cur;
    } else {
      ++Cur;
    }
  }
}

void Lexer::closeConditionals() {
  while (!Conditionals.empty() &&
         Conditionals.back().Depth == IncludeStack.size()) {
    Diags.error(Conditionals.back().Loc,
                llvm::Twine(Conditionals.back().Name) +
                    " is not terminated; expected $ENDIF before the end of "
                    "the file");
    Conditionals.pop_back();
  }
}

void Lexer::skipWhitespaceAndComments() {
  while (Cur < BufEnd) {
    switch (*Cur) {
    case ' ': case '\t': case '\r': case '\n': case '\f': case '\v':
      ++Cur;
      continue;
    case '{':
      skipBlockComment(1, "}");
      if (!isActive())
        skipInactive();
      continue;
    case '(':
      if (Cur[1] == '*') {
        skipBlockComment(2, "*)");
        if (!isActive())
          skipInactive();
        continue;
      }
      return;
    case '/':
      if (LangOpts.Mode != LangMode::ISO && Cur[1] == '/') { // // to end of line
        while (Cur < BufEnd && *Cur != '\n')
          ++Cur;
        continue;
      }
      return;
    default:
      return;
    }
  }
}

//===----------------------------------------------------------------------===//
// Identifiers / keywords
//===----------------------------------------------------------------------===//

void Lexer::lexIdentifier(Token &T) {
  const char *Start = Cur;
  while (isIdentBody(*Cur))
    ++Cur;

  llvm::StringRef Spelling(Start, Cur - Start);
  llvm::SmallString<32> Lower(Spelling);
  for (char &C : Lower)
    C = llvm::toLower(C);

  tok::TokenKind K = tok::getKeywordKind(Lower, LangOpts);
  formToken(T, K, Start);
  if (K == tok::identifier)
    T.Data = Strings.intern(Lower);
}

//===----------------------------------------------------------------------===//
// Numbers
//
//   integer = digit {digit}
//   real    = digit {digit} "." digit {digit} [exponent]
//           | digit {digit} exponent
//   exponent = ("e"|"E") ["+"|"-"] digit {digit}
//
// "1..10" is integer, "..", integer: a '.' only starts a fraction when a
// digit follows it.
//===----------------------------------------------------------------------===//

void Lexer::lexNumber(Token &T) {
  const char *Start = Cur;
  bool IsReal = false;

  while (llvm::isDigit(*Cur))
    ++Cur;

  if (*Cur == '.' && llvm::isDigit(Cur[1])) {
    IsReal = true;
    ++Cur;
    while (llvm::isDigit(*Cur))
      ++Cur;
  }

  if (*Cur == 'e' || *Cur == 'E') {
    const char *P = Cur + 1;
    bool HasSign = *P == '+' || *P == '-';
    if (HasSign)
      ++P;
    if (llvm::isDigit(*P)) {
      IsReal = true;
      Cur = P;
      while (llvm::isDigit(*Cur))
        ++Cur;
    } else if (HasSign) {
      // "1e+" -- clearly a broken exponent rather than "1 e + ...".
      Diags.error(locOf(P), "expected digits in exponent");
      IsReal = true;
      Cur = P;
    }
    // Otherwise "e" begins the next token (the parser will complain).
  }

  llvm::StringRef Text(Start, Cur - Start);

  if (!IsReal) {
    formToken(T, tok::integer_literal, Start);
    uint64_t V;
    if (Text.getAsInteger(10, V)) {
      Diags.error(locOf(Start), "integer literal is too large",
                  {T.getRange()});
      V = 0;
    }
    T.IntValue = V;
    return;
  }

  formToken(T, tok::real_literal, Start);
  llvm::APFloat F(llvm::APFloat::IEEEdouble());
  auto StatusOrErr =
      F.convertFromString(Text, llvm::APFloat::rmNearestTiesToEven);
  if (!StatusOrErr) {
    // Only reachable for malformed exponents reported above.
    llvm::consumeError(StatusOrErr.takeError());
    T.RealValue = 0.0;
    return;
  }
  if (*StatusOrErr & llvm::APFloat::opOverflow)
    Diags.error(locOf(Start), "real literal is too large", {T.getRange()});
  T.RealValue = F.convertToDouble();
}

/// $FF, %1010, &17 (Turbo/FPC). Cur points after the prefix character.
void Lexer::lexBasedNumber(Token &T, const char *Start, unsigned Radix) {
  while (llvm::isAlnum(*Cur))
    ++Cur;
  formToken(T, tok::integer_literal, Start);
  uint64_t V;
  if (llvm::StringRef(Start + 1, Cur - Start - 1).getAsInteger(Radix, V)) {
    Diags.error(locOf(Start), "invalid base-" + llvm::Twine(Radix) + " number",
                {T.getRange()});
    V = 0;
  }
  T.IntValue = V;
  T.Based = true;
}

//===----------------------------------------------------------------------===//
// Strings:  'text'  with '' denoting one quote. May not span lines.
// --mode=tp also accepts control characters #13 / #$0D, glued to quoted
// parts: 'line'#13#10'next'.
//===----------------------------------------------------------------------===//

void Lexer::lexString(Token &T) {
  const char *Start = Cur;
  llvm::SmallString<64> Value;
  bool Terminated = true;
  bool TP = LangOpts.Mode != LangMode::ISO;
  unsigned Parts = 0, WideCode = 0;

  while (Terminated) {
    if (*Cur == '\'') {
      ++Parts;
      ++Cur;
      Terminated = false;
      while (Cur < BufEnd && *Cur != '\n' && *Cur != '\r') {
        if (*Cur == '\'') {
          if (Cur[1] == '\'') {
            Value.push_back('\'');
            Cur += 2;
            continue;
          }
          ++Cur;
          Terminated = true;
          break;
        }
        Value.push_back(*Cur++);
      }
    } else if (TP && *Cur == '#') {
      const char *CodeStart = Cur++;
      ++Parts;
      unsigned Radix = 10;
      if (*Cur == '$') {
        Radix = 16;
        ++Cur;
      }
      const char *Digits = Cur;
      while (llvm::isAlnum(*Cur))
        ++Cur;
      unsigned Code;
      if (llvm::StringRef(Digits, Cur - Digits).getAsInteger(Radix, Code) ||
          Code > 65535) {
        Diags.error(locOf(CodeStart), "invalid character code",
                    {SourceRange(locOf(CodeStart), locOf(Cur))});
        Code = 0;
      }
      // A code above 255 (a widechar, Free Pascal): '?' in a string, as
      // FPC's conversion to its one-byte strings gives.
      if (Code > 255)
        WideCode = Parts == 1 ? Code : 0;
      Value.push_back(Code > 255 ? '?' : static_cast<char>(Code));
    } else {
      break;
    }
    if (!TP)
      break; // ISO: one quoted part per token
  }

  formToken(T, tok::string_literal, Start);
  T.Data = Strings.intern(Value);
  T.WideChar = Parts == 1 ? WideCode : 0; // a lone #code above 255

  if (!Terminated)
    Diags.error(locOf(Start), "unterminated string literal");
  else if (Value.empty() && !LangOpts.EmptyStrings)
    Diags.error(locOf(Start),
                "empty string literal is not allowed in ISO Pascal",
                {T.getRange()});
}

//===----------------------------------------------------------------------===//
// Template strings (tp, objfpc):  `text ${expr} text`
//
// The text may span lines; a line end is one '\n' whatever the source has.
// `` is a backtick and $${ the two characters ${. ${ starts an
// interpolation: the tokens up to the '}' that closes it are lexed as
// usual, between template_head, template_middle and template_tail tokens
// holding the text around them. A template without interpolations is a
// string_literal, and so is a raw one, r`text`, where ${ is just text.
//===----------------------------------------------------------------------===//

void Lexer::dedentTemplate(llvm::MutableArrayRef<std::string> Chunks) {
  auto IsBlank = [](char C) { return C == ' ' || C == '\t'; };
  const size_t None = std::string::npos;
  size_t Min = None;

  // The line of the closing backtick, when only blanks precede it.
  std::string &Last = Chunks.back();
  size_t NL = Last.rfind('\n');
  if (NL != None) {
    size_t I = NL + 1;
    while (I < Last.size() && IsBlank(Last[I]))
      ++I;
    if (I == Last.size()) {
      Min = Last.size() - NL - 1;
      Last.resize(NL);
    }
  }
  // The line break after the opening backtick: the text starts a line.
  bool First = !Chunks[0].empty() && Chunks[0][0] == '\n';
  if (First)
    Chunks[0].erase(0, 1);

  // Pass 0 finds the shared indentation, pass 1 removes it.
  for (int Pass = 0; Pass < 2; ++Pass) {
    for (size_t I = 0; I < Chunks.size(); ++I) {
      std::string &C = Chunks[I];
      std::string Out;
      bool AtStart = I == 0 && First;
      size_t P = 0;
      while (true) {
        if (AtStart) {
          size_t K = P;
          while (K < C.size() && IsBlank(C[K]))
            ++K;
          // (At a chunk's end an interpolation follows: not a blank line.)
          bool Blank = K == C.size() ? I + 1 == Chunks.size() : C[K] == '\n';
          if (Pass == 0) {
            if (!Blank && K - P < Min)
              Min = K - P;
          } else {
            P = Blank ? K : P + Min;
          }
        }
        size_t E = C.find('\n', P);
        if (E == None) {
          if (Pass == 1)
            Out.append(C, P, None);
          break;
        }
        if (Pass == 1)
          Out.append(C, P, E - P + 1);
        P = E + 1;
        AtStart = true;
      }
      if (Pass == 1)
        C = std::move(Out);
    }
    if (Min == None)
      Min = 0;
  }
}

/// The text of a template string from Cur, which is after its opening
/// backtick (Opening) or after the '}' that ends an interpolation, up to
/// the next interpolation or the closing backtick.
void Lexer::lexTemplate(Token &T, const char *Start, bool Raw, bool Opening) {
  std::string Value;
  bool More = false; // stopped at an interpolation
  while (true) {
    if (Cur >= BufEnd) {
      Diags.error(locOf(Start), "unterminated template string");
      break;
    }
    char C = *Cur;
    if (C == '`') {
      if (Cur[1] == '`') {
        Value.push_back('`');
        Cur += 2;
        continue;
      }
      ++Cur;
      break;
    }
    if (C == '\r') { // CR LF and CR: one line end
      Value.push_back('\n');
      ++Cur;
      if (Cur < BufEnd && *Cur == '\n')
        ++Cur;
      continue;
    }
    if (C == '$' && !Raw) {
      if (Cur[1] == '{') {
        Cur += 2;
        More = true;
        break;
      }
      if (Cur[1] == '$' && Cur[2] == '{') {
        Value += "${";
        Cur += 3;
        continue;
      }
    }
    Value.push_back(C);
    ++Cur;
  }

  if (Opening && !More) { // the whole of it: a string
    dedentTemplate(Value);
    formToken(T, tok::string_literal, Start);
    T.Template = true;
  } else {
    formToken(T, Opening ? tok::template_head
                 : More  ? tok::template_middle
                         : tok::template_tail,
              Start);
    if (Opening)
      ++TemplateDepth;
    else if (!More)
      --TemplateDepth;
  }
  T.Data = Strings.intern(Value);
}

//===----------------------------------------------------------------------===//
// Main entry point
//===----------------------------------------------------------------------===//

void Lexer::lex(Token &T) {
  while (true) {
    skipWhitespaceAndComments();
    const char *Start = Cur;

    if (Cur >= BufEnd) {
      if (!MacroStack.empty()) { // the end of a macro's value
        MacroFrame F = MacroStack.pop_back_val();
        BufEnd = F.BufEnd;
        Cur = F.Cur;
        continue;
      }
      closeConditionals();
      if (!IncludeStack.empty()) { // back to the including file
        IncludeFrame F = IncludeStack.pop_back_val();
        BufBegin = F.BufBegin;
        BufEnd = F.BufEnd;
        Cur = F.Cur;
        continue;
      }
      formToken(T, tok::eof, Start);
      return;
    }

    char C = *Cur;
    bool TP = LangOpts.Mode != LangMode::ISO;
    // Template strings: `text`, raw r`text`, and the text after an
    // interpolation's '}'.
    if (TP && C == '`') {
      ++Cur;
      return lexTemplate(T, Start, /*Raw=*/false, /*Opening=*/true);
    }
    if (TP && (C == 'r' || C == 'R') && Cur[1] == '`') {
      Cur += 2;
      return lexTemplate(T, Start, /*Raw=*/true, /*Opening=*/true);
    }
    if (C == '}' && TemplateDepth) {
      ++Cur;
      return lexTemplate(T, Start, /*Raw=*/false, /*Opening=*/false);
    }
    if (isIdentStart(C)) {
      lexIdentifier(T);
      if (MacrosOn && T.is(tok::identifier) && expandMacro(T, Start))
        continue; // the macro's value instead
      return;
    }
    if (llvm::isDigit(C))
      return lexNumber(T);
    if (C == '\'' || (TP && C == '#'))
      return lexString(T);
    if (TP && ((C == '$' && llvm::isHexDigit(Cur[1])) ||
               (C == '%' && (Cur[1] == '0' || Cur[1] == '1')) ||
               (C == '&' && Cur[1] >= '0' && Cur[1] <= '7'))) {
      ++Cur;
      return lexBasedNumber(T, Start, C == '$' ? 16 : C == '%' ? 2 : 8);
    }

    ++Cur;
    tok::TokenKind K = tok::unknown;
    switch (C) {
    case '+': K = tok::plus; break;
    case '-': K = tok::minus; break;
    case '*': K = tok::star; break;
    case '/': K = tok::slash; break;
    case '=':
      K = tok::equal;
      if (*Cur == '>') { ++Cur; K = tok::equal_greater; } // (a lambda's)
      break;
    case ';': K = tok::semi; break;
    case ',': K = tok::comma; break;
    case ')': K = tok::r_paren; break;
    case '[': K = tok::l_square; break;
    case ']': K = tok::r_square; break;
    case '^': K = tok::caret; break;
    case '?': // T?, x?.m, a ?? b (objfpc)
      if (!LangOpts.ObjKeywords)
        break;
      K = tok::question;
      if (*Cur == '.' && Cur[1] != '.') { ++Cur; K = tok::question_period; }
      else if (*Cur == '?') { ++Cur; K = tok::question_question; }
      break;
    case '@':
      // ISO 7185 allows '@' as an alternative spelling of '^'.
      K = LangOpts.TPKeywords ? tok::at : tok::caret;
      break;
    case ':':
      K = tok::colon;
      if (*Cur == '=') { ++Cur; K = tok::colon_equal; }
      break;
    case '<':
      K = tok::less;
      if (*Cur == '=') { ++Cur; K = tok::less_equal; }
      else if (*Cur == '>') { ++Cur; K = tok::not_equal; }
      break;
    case '>':
      K = tok::greater;
      if (*Cur == '=') { ++Cur; K = tok::greater_equal; }
      break;
    case '.':
      K = tok::period;
      if (*Cur == '.') { ++Cur; K = tok::dotdot; }
      else if (*Cur == ')') { ++Cur; K = tok::r_square; } // ".)" == "]"
      break;
    case '(':
      K = tok::l_paren;
      if (*Cur == '.') { ++Cur; K = tok::l_square; }      // "(." == "["
      break;
    default:
      break;
    }

    // {$coperators on}: += -= *= /= (Free Pascal's C-style assignments)
    if (COperators && *Cur == '=' &&
        (K == tok::plus || K == tok::minus || K == tok::star ||
         K == tok::slash)) {
      ++Cur;
      K = K == tok::plus    ? tok::plus_equal
          : K == tok::minus ? tok::minus_equal
          : K == tok::star  ? tok::star_equal
                            : tok::slash_equal;
    }

    if (K != tok::unknown) {
      formToken(T, K, Start);
      return;
    }

    // Invalid character: report, skip it, and keep lexing.
    // Two hex digits. (llvm::utohexstr with a width prints "000" for 0.)
    static const char Hex[] = "0123456789ABCDEF";
    unsigned char U = static_cast<unsigned char>(C);
    std::string Shown = llvm::isPrint(C)
                            ? std::string(1, C)
                            : std::string("\\x") + Hex[U >> 4] + Hex[U & 15];
    Diags.error(locOf(Start), "invalid character '" + Shown + "' in source",
                {SourceRange(locOf(Start), locOf(Cur))});
  }
}
