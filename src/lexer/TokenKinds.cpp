// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "p3/Lexer/TokenKinds.h"
#include "p3/Basic/LangOptions.h"

#include "llvm/ADT/StringMap.h"

using namespace p3;

static const char *const TokNames[] = {
#define TOK(ID) #ID,
#include "p3/Lexer/TokenKinds.def"
};

static const char *const TokSpellings[] = {
#define TOK(ID) nullptr,
#define PUNCT(ID, SP) SP,
#define KEYWORD(ID, SP, FLAGS) SP,
#include "p3/Lexer/TokenKinds.def"
};

const char *tok::getTokenName(TokenKind K) {
  return K < NUM_TOKENS ? TokNames[K] : "<invalid>";
}

const char *tok::getSpelling(TokenKind K) {
  return K < NUM_TOKENS ? TokSpellings[K] : nullptr;
}

bool tok::isKeyword(TokenKind K) {
  switch (K) {
#define KEYWORD(ID, SP, FLAGS) case ID:
#include "p3/Lexer/TokenKinds.def"
    return true;
  default:
    return false;
  }
}

bool tok::isPunctuator(TokenKind K) {
  switch (K) {
#define PUNCT(ID, SP) case ID:
#include "p3/Lexer/TokenKinds.def"
    return true;
  default:
    return false;
  }
}

bool tok::isLiteral(TokenKind K) {
  return K == integer_literal || K == real_literal || K == string_literal;
}

namespace {
struct KeywordInfo {
  tok::TokenKind Kind;
  unsigned Flags;
};

const llvm::StringMap<KeywordInfo> &keywordTable() {
  static const llvm::StringMap<KeywordInfo> Table = [] {
    using namespace tok;
    llvm::StringMap<KeywordInfo> M;
#define KEYWORD(ID, SP, FLAGS) M[SP] = {ID, FLAGS};
#include "p3/Lexer/TokenKinds.def"
    return M;
  }();
  return Table;
}
} // namespace

tok::TokenKind tok::getKeywordKind(llvm::StringRef LowerName,
                                   const LangOptions &LangOpts) {
  auto It = keywordTable().find(LowerName);
  if (It == keywordTable().end())
    return identifier;
  unsigned Flags = It->second.Flags;
  if ((Flags & KEYISO) || ((Flags & KEYTP) && LangOpts.TPKeywords) ||
      ((Flags & KEYOBJ) && LangOpts.ObjKeywords))
    return It->second.Kind;
  return identifier;
}
