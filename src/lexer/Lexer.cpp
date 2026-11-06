#include "pcc/Lexer/Lexer.h"
#include "pcc/Basic/Diagnostics.h"
#include "pcc/Basic/SourceManager.h"
#include "pcc/Basic/StringPool.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"

using namespace pcc;

static bool isIdentStart(char C) { return llvm::isAlpha(C) || C == '_'; }
static bool isIdentBody(char C) { return llvm::isAlnum(C) || C == '_'; }

Lexer::Lexer(const SourceManager &SM, FileID FID, DiagnosticsEngine &Diags,
             StringPool &Strings, const LangOptions &LangOpts)
    : Diags(Diags), Strings(Strings), LangOpts(LangOpts) {
  llvm::StringRef Buf = SM.getBufferData(FID);
  Cur = Buf.data();
  BufEnd = Buf.data() + Buf.size();
  // MemoryBuffers are NUL-terminated, so peeking one char past a token
  // (e.g. Cur[1]) is always safe, even at BufEnd.
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
  if (Pos == llvm::StringRef::npos) {
    Diags.error(locOf(Start), "unterminated comment",
                {SourceRange(locOf(Start), locOf(Start + OpenLen))});
    Cur = BufEnd;
    return;
  }
  // TODO(M11): a comment starting with '$' is a compiler directive.
  Cur = Rest.data() + Pos + Close.size();
}

void Lexer::skipWhitespaceAndComments() {
  while (Cur < BufEnd) {
    switch (*Cur) {
    case ' ': case '\t': case '\r': case '\n': case '\f': case '\v':
      ++Cur;
      continue;
    case '{':
      skipBlockComment(1, "}");
      continue;
    case '(':
      if (Cur[1] == '*') {
        skipBlockComment(2, "*)");
        continue;
      }
      return;
    // TODO(M12): '//' line comments in --mode=tp.
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

//===----------------------------------------------------------------------===//
// Strings:  'text'  with '' denoting one quote. May not span lines.
//===----------------------------------------------------------------------===//

void Lexer::lexString(Token &T) {
  const char *Start = Cur++;
  llvm::SmallString<64> Value;
  bool Terminated = false;

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
      formToken(T, tok::eof, Start);
      return;
    }

    char C = *Cur;
    if (isIdentStart(C))
      return lexIdentifier(T);
    if (llvm::isDigit(C))
      return lexNumber(T);
    if (C == '\'')
      return lexString(T);

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
    std::string Shown =
        llvm::isPrint(C) ? std::string(1, C)
                         : "\\x" + llvm::utohexstr((unsigned char)C, false, 2);
    Diags.error(locOf(Start), "invalid character '" + Shown + "' in source",
                {SourceRange(locOf(Start), locOf(Cur))});
  }
}
