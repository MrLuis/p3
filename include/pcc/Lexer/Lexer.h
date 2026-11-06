//===- Lexer.h - Pascal tokenizer ---------------------------------*- C++ -*-===//
//
// Turns one source buffer into a stream of Tokens. Errors are reported through
// the DiagnosticsEngine and the lexer always recovers, so callers simply keep
// calling lex() until tok::eof.
//
// Implemented (M1): whitespace, { } and (* *) comments, identifiers and all
// keywords (case-insensitive), integer and real literals, '...' strings with
// '' escapes, every operator/punctuator incl. (. .) and @.
//
// Compiler directives {$R±} / {$Q±} are recorded (M11). Turbo/FPC lexical
// extensions arrive with M12.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_LEXER_LEXER_H
#define PCC_LEXER_LEXER_H

#include "pcc/Basic/LangOptions.h"
#include "pcc/Basic/SourceLocation.h"
#include "pcc/Lexer/Token.h"

namespace pcc {

class CheckDirectives;
class DiagnosticsEngine;
class SourceManager;
class StringPool;

class Lexer {
  DiagnosticsEngine &Diags;
  StringPool &Strings;
  LangOptions LangOpts;

  const char *BufBegin;
  const char *BufEnd;
  const char *Cur;
  CheckDirectives *Directives = nullptr;

  void handleDirective(const char *Start, llvm::StringRef Text);

  void skipWhitespaceAndComments();
  void skipBlockComment(unsigned OpenLen, llvm::StringRef Close);

  void lexIdentifier(Token &T);
  void lexNumber(Token &T);
  void lexString(Token &T);
  void lexBasedNumber(Token &T, const char *Start, unsigned Radix);

  void formToken(Token &T, tok::TokenKind K, const char *Start);
  SourceLocation locOf(const char *P) const {
    return SourceLocation::fromPointer(P);
  }

public:
  Lexer(const SourceManager &SM, FileID FID, DiagnosticsEngine &Diags,
        StringPool &Strings, const LangOptions &LangOpts);

  /// Lexes the next token. Returns tok::eof (repeatedly) at the end.
  void lex(Token &T);

  const LangOptions &getLangOpts() const { return LangOpts; }

  /// Where to record {$R±} / {$Q±} switches (null: ignore directives).
  void setDirectives(CheckDirectives *D) { Directives = D; }
};

} // namespace pcc

#endif // PCC_LEXER_LEXER_H
