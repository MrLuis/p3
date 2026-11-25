// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- TokenKinds.h - Token kind enum and helpers -----------------*- C++ -*-===//
#ifndef P3_LEXER_TOKENKINDS_H
#define P3_LEXER_TOKENKINDS_H

#include "llvm/ADT/StringRef.h"

namespace p3 {

struct LangOptions;

namespace tok {

enum TokenKind : unsigned short {
#define TOK(ID) ID,
#include "p3/Lexer/TokenKinds.def"
  NUM_TOKENS
};

/// Keyword availability flags (see TokenKinds.def).
enum KeywordFlags : unsigned { KEYISO = 0x1, KEYTP = 0x2, KEYOBJ = 0x4 };

/// Enum name, e.g. "kw_program", "colon_equal", "identifier".
const char *getTokenName(TokenKind K);

/// Source spelling of a punctuator or keyword ("+", "program"), else nullptr.
const char *getSpelling(TokenKind K);

bool isKeyword(TokenKind K);
bool isPunctuator(TokenKind K);
bool isLiteral(TokenKind K);

/// Maps a lowercase word to its keyword kind under LangOpts, or
/// tok::identifier if it is not reserved in that dialect.
TokenKind getKeywordKind(llvm::StringRef LowerName, const LangOptions &LangOpts);

} // namespace tok
} // namespace p3

#endif // P3_LEXER_TOKENKINDS_H
