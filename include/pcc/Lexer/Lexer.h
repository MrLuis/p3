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
// Later milestones (TODO): {$...} directives (M11); // comments, $hex,
// %binary, &octal and #nn character codes (M12).
//
//===----------------------------------------------------------------------===//
#ifndef PCC_LEXER_LEXER_H
#define PCC_LEXER_LEXER_H

#include "pcc/Basic/LangOptions.h"
#include "pcc/Basic/SourceLocation.h"
#include "pcc/Lexer/Token.h"

namespace pcc {

class DiagnosticsEngine;
class SourceManager;
class StringPool;

class Lexer {
  DiagnosticsEngine &Diags;
  StringPool &Strings;
  LangOptions LangOpts;

  const char *BufEnd;
  const char *Cur;

  void skipWhitespaceAndComments();
  void skipBlockComment(unsigned OpenLen, llvm::StringRef Close);

  void lexIdentifier(Token &T);
  void lexNumber(Token &T);
  void lexString(Token &T);

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
};

} // namespace pcc

#endif // PCC_LEXER_LEXER_H
