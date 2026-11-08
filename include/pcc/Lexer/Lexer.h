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
// extensions arrive with M12. Conditional compilation ({$DEFINE}, {$UNDEF},
// {$IFDEF}, {$IFNDEF}, {$IF}, {$ELSEIF}, {$ELSE}, {$ENDIF}, {$IFEND}) is
// done here: the text of a branch that is not compiled is skipped, and only
// its comments, strings and conditional directives are recognized.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_LEXER_LEXER_H
#define PCC_LEXER_LEXER_H

#include "pcc/Basic/LangOptions.h"
#include "pcc/Basic/SourceLocation.h"
#include "pcc/Lexer/Token.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"

#include <string>
#include <vector>

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

  // {$I file} (Turbo/FPC): the including buffers, innermost last.
  struct IncludeFrame {
    const char *BufBegin, *BufEnd, *Cur;
  };
  llvm::SmallVector<IncludeFrame, 4> IncludeStack;
  SourceManager *IncludeSM = nullptr; ///< null: includes are not supported
  std::vector<std::string> IncludeDirs;
  std::string PendingInclude;          ///< file named by the last {$I ...}
  SourceLocation PendingIncludeLoc;
  void enterPendingInclude();

  void handleDirective(const char *Start, llvm::StringRef Text);

  // Conditional compilation. Symbols are kept in upper case; a file starts
  // with the predefined and command-line ones, and its include files share
  // them.
  llvm::StringSet<> Defines;
  struct Conditional {
    SourceLocation Loc; ///< the opening {$IF...}
    const char *Name;   ///< its directive, e.g. "$IFDEF"
    bool ParentActive;  ///< the text around the conditional is compiled
    bool Active;        ///< the current branch is compiled
    bool Taken;         ///< some branch has been compiled
    bool SeenElse;
    unsigned Depth;     ///< include depth of the opening directive
  };
  llvm::SmallVector<Conditional, 8> Conditionals;
  bool isActive() const {
    return Conditionals.empty() || Conditionals.back().Active;
  }
  /// Handles Word if it is a conditional-compilation directive.
  bool handleConditional(const char *Start, llvm::StringRef Word,
                         llvm::StringRef Arg);
  bool evalCondition(const char *Start, llvm::StringRef Expr);
  /// Skips the text of a branch that is not compiled.
  void skipInactive();
  /// At the end of a buffer: reports the conditionals it left open.
  void closeConditionals();

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

  /// Defines symbols for conditional compilation (-d NAME).
  void setDefines(llvm::ArrayRef<std::string> Names);

  /// Where to record {$R±} / {$Q±} switches (null: ignore directives).
  void setDirectives(CheckDirectives *D) { Directives = D; }

  /// Enables {$I file}: files are searched next to the including file,
  /// then in Dirs, and loaded into SM.
  void setIncludeSupport(SourceManager &SM, llvm::ArrayRef<std::string> Dirs) {
    IncludeSM = &SM;
    IncludeDirs.assign(Dirs.begin(), Dirs.end());
  }
};

} // namespace pcc

#endif // PCC_LEXER_LEXER_H
