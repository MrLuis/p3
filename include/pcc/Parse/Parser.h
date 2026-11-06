//===- Parser.h - Recursive-descent Pascal parser -----------------*- C++ -*-===//
//
// Grammar accepted so far (M2):
//
//   program    = "program" ident [ "(" identList ")" ] ";" block "." .
//   block      = compoundStmt .                  (declarations: M4+)
//   compound   = "begin" statement { ";" statement } "end" .
//   statement  = [ procCall | compound ] .        (empty allowed)
//   procCall   = ident [ "(" expression { "," expression } ")" ] .
//   expression = integerLiteral | stringLiteral .  (operators etc.: M4)
//
// Error recovery: report, then skip to a synchronizing token (';', 'end',
// 'begin', ...) and continue, so one run reports several independent errors.
// Constructs from later milestones are recognized and reported as
// "not supported yet" rather than as confusing syntax errors.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_PARSE_PARSER_H
#define PCC_PARSE_PARSER_H

#include "pcc/AST/AST.h"
#include "pcc/Lexer/Token.h"

#include <initializer_list>

namespace pcc {

class ASTContext;
class DiagnosticsEngine;
class Lexer;

class Parser {
  Lexer &Lex;
  DiagnosticsEngine &Diags;
  ASTContext &Ctx;

  Token Tok;                  ///< current lookahead token
  SourceLocation PrevTokEnd;  ///< end of the last consumed token

  // Token helpers
  SourceLocation consumeToken();
  bool tryConsume(tok::TokenKind K);
  /// If the current token is K, consume it. Otherwise report
  /// "expected 'K' <Context>" right after the previous token and return false.
  bool expect(tok::TokenKind K, llvm::StringRef Context);
  /// Skips tokens until one of Stop (not consumed) or EOF.
  void skipUntil(std::initializer_list<tok::TokenKind> Stop);
  static std::string describe(tok::TokenKind K);
  Identifier makeIdentifier(const Token &T) const;

  bool startsStatement() const;

  // Grammar rules
  Block *parseBlock();
  CompoundStmt *parseCompoundStmt();
  Stmt *parseStatement();
  Stmt *parseProcCall();
  Expr *parseExpression();

  void unsupported(llvm::StringRef What);

public:
  Parser(Lexer &Lex, DiagnosticsEngine &Diags, ASTContext &Ctx);

  /// Parses a whole program. Always returns a (possibly partial) AST;
  /// check the DiagnosticsEngine for errors.
  ProgramDecl *parseProgram();
};

} // namespace pcc

#endif // PCC_PARSE_PARSER_H
