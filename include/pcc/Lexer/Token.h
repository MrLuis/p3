//===- Token.h - One lexed token ----------------------------------*- C++ -*-===//
#ifndef PCC_LEXER_TOKEN_H
#define PCC_LEXER_TOKEN_H

#include "pcc/Basic/SourceLocation.h"
#include "pcc/Lexer/TokenKinds.h"

#include <cstdint>

namespace pcc {

class Token {
  tok::TokenKind Kind = tok::eof;
  SourceLocation Loc;
  unsigned Length = 0;
  bool Based = false; ///< integer_literal written $hex, %binary or &octal
  /// string_literal: a lone character code above 255 (#$D800), a widechar
  /// constant; its value is then "?".
  unsigned WideChar = 0;

  /// identifier:     canonical lowercase name (interned)
  /// string_literal: value with '' unescaped (interned)
  llvm::StringRef Data;

  union {
    uint64_t IntValue; ///< integer_literal
    double RealValue;  ///< real_literal
  };

  friend class Lexer;

public:
  Token() : IntValue(0) {}

  tok::TokenKind getKind() const { return Kind; }
  bool is(tok::TokenKind K) const { return Kind == K; }
  bool isNot(tok::TokenKind K) const { return Kind != K; }
  template <typename... Ts> bool isOneOf(Ts... Ks) const {
    return ((Kind == Ks) || ...);
  }

  SourceLocation getLocation() const { return Loc; }
  SourceLocation getEndLocation() const { return Loc.getLocWithOffset(Length); }
  SourceRange getRange() const { return {Loc, getEndLocation()}; }
  unsigned getLength() const { return Length; }

  /// Exact text as written in the source (original case, quotes included).
  llvm::StringRef getRawText() const {
    return llvm::StringRef(Loc.getPointer(), Length);
  }

  /// Canonical (lowercase) identifier name. Only for tok::identifier.
  llvm::StringRef getIdentifier() const { return Data; }
  /// Cooked string value. Only for tok::string_literal.
  llvm::StringRef getStringValue() const { return Data; }
  uint64_t getIntegerValue() const { return IntValue; }
  /// An integer literal written $FF, %1010 or &17 (Turbo/FPC)?
  bool isBasedInteger() const { return Based; }
  /// A string literal that is one character code above 255: the code.
  unsigned getWideCharCode() const { return WideChar; }
  double getRealValue() const { return RealValue; }

  bool isKeyword() const { return tok::isKeyword(Kind); }
};

} // namespace pcc

#endif // PCC_LEXER_TOKEN_H
