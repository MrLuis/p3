#include "pcc/Lexer/Lexer.h"
#include "pcc/Basic/Diagnostics.h"
#include "pcc/Basic/Directives.h"
#include "pcc/Basic/SourceManager.h"
#include "pcc/Basic/StringPool.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace pcc;

static bool isIdentStart(char C) { return llvm::isAlpha(C) || C == '_'; }
static bool isIdentBody(char C) { return llvm::isAlnum(C) || C == '_'; }

Lexer::Lexer(const SourceManager &SM, FileID FID, DiagnosticsEngine &Diags,
             StringPool &Strings, const LangOptions &LangOpts)
    : Diags(Diags), Strings(Strings), LangOpts(LangOpts) {
  llvm::StringRef Buf = SM.getBufferData(FID);
  Cur = BufBegin = Buf.data();
  BufEnd = Buf.data() + Buf.size();
  // MemoryBuffers are NUL-terminated, so peeking one char past a token
  // (e.g. Cur[1]) is always safe, even at BufEnd.
  Defines.insert("PCC");
  Defines.insert(LangOpts.Mode == LangMode::ISO  ? "PCC_ISO"
                 : LangOpts.Mode == LangMode::TP ? "PCC_TP"
                                                 : "PCC_OBJFPC");
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

  // Switch list: letter followed by + or -, comma separated: $R-,Q+
  llvm::SmallVector<llvm::StringRef, 4> Parts;
  Text.split(Parts, ',');
  for (llvm::StringRef P : Parts) {
    P = P.trim();
    if (P.size() == 2 && llvm::isAlpha(P[0]) && (P[1] == '+' || P[1] == '-')) {
      char Letter = llvm::toUpper(P[0]);
      bool On = P[1] == '+';
      if (Directives && (Letter == 'R' || Letter == 'Q'))
        Directives->add(locOf(Start), BufBegin, BufEnd,
                        Letter == 'R' ? CheckDirectives::Range
                                      : CheckDirectives::Overflow,
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
/// Evaluates the expression of {$IF} / {$ELSEIF}: defined(NAME), true,
/// false, not, and, or and parentheses.
class ConditionParser {
  llvm::StringRef S;
  size_t P = 0;
  const llvm::StringSet<> &Defines;

  llvm::StringRef peek() {
    while (P < S.size() && llvm::isSpace(S[P]))
      ++P;
    if (P >= S.size())
      return {};
    size_t E = P;
    if (isIdentStart(S[E]))
      while (E < S.size() && isIdentBody(S[E]))
        ++E;
    else
      ++E;
    return S.slice(P, E);
  }
  bool accept(llvm::StringRef W) {
    if (!peek().equals_insensitive(W))
      return false;
    P += W.size();
    return true;
  }
  bool parseOr() {
    bool V = parseAnd();
    while (accept("or")) {
      bool R = parseAnd();
      V = V || R;
    }
    return V;
  }
  bool parseAnd() {
    bool V = parseNot();
    while (accept("and")) {
      bool R = parseNot();
      V = V && R;
    }
    return V;
  }
  bool parseNot() {
    if (accept("not"))
      return !parseNot();
    return parsePrimary();
  }
  bool parsePrimary() {
    if (accept("(")) {
      bool V = parseOr();
      if (!accept(")"))
        Bad = true;
      return V;
    }
    if (accept("true"))
      return true;
    if (accept("false"))
      return false;
    if (accept("defined")) {
      if (!accept("(")) {
        Bad = true;
        return false;
      }
      llvm::StringRef Name = peek();
      if (Name.empty() || !isIdentStart(Name[0])) {
        Bad = true;
        return false;
      }
      P += Name.size();
      if (!accept(")"))
        Bad = true;
      return Defines.contains(Name.upper());
    }
    Bad = true;
    return false;
  }

public:
  bool Bad = false;
  ConditionParser(llvm::StringRef S, const llvm::StringSet<> &Defines)
      : S(S), Defines(Defines) {}
  bool parse() {
    bool V = parseOr();
    if (!peek().empty())
      Bad = true;
    return V;
  }
};
} // namespace

bool Lexer::evalCondition(const char *Start, llvm::StringRef Expr) {
  ConditionParser CP(Expr, Defines);
  bool V = CP.parse();
  if (!CP.Bad)
    return V;
  Diags.error(locOf(Start),
              "unsupported $IF expression; expected defined(NAME), true, "
              "false, not, and, or and parentheses");
  return false;
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
    if (Arg.drop_front(Sym.size()).ltrim().starts_with(":=")) {
      Diags.error(locOf(Start), "macros ({$DEFINE NAME := value}) are not "
                                "supported");
      return true;
    }
    if (K == Define)
      Defines.insert(Sym.upper());
    else
      Defines.erase(Sym.upper());
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

  while (Terminated) {
    if (*Cur == '\'') {
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
          Code > 255) {
        Diags.error(locOf(CodeStart), "invalid character code",
                    {SourceRange(locOf(CodeStart), locOf(Cur))});
        Code = 0;
      }
      Value.push_back(static_cast<char>(Code));
    } else {
      break;
    }
    if (!TP)
      break; // ISO: one quoted part per token
  }

  formToken(T, tok::string_literal, Start);
  T.Data = Strings.intern(Value);

  if (!Terminated)
    Diags.error(locOf(Start), "unterminated string literal");
  else if (Value.empty() && !LangOpts.EmptyStrings)
    Diags.error(locOf(Start),
                "empty string literal is not allowed in ISO Pascal",
                {T.getRange()});
}

//===----------------------------------------------------------------------===//
// Main entry point
//===----------------------------------------------------------------------===//

void Lexer::lex(Token &T) {
  while (true) {
    skipWhitespaceAndComments();
    const char *Start = Cur;

    if (Cur >= BufEnd) {
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
    if (isIdentStart(C))
      return lexIdentifier(T);
    if (llvm::isDigit(C))
      return lexNumber(T);
    bool TP = LangOpts.Mode != LangMode::ISO;
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
    case '=': K = tok::equal; break;
    case ';': K = tok::semi; break;
    case ',': K = tok::comma; break;
    case ')': K = tok::r_paren; break;
    case '[': K = tok::l_square; break;
    case ']': K = tok::r_square; break;
    case '^': K = tok::caret; break;
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
