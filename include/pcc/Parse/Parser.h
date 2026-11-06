//===- Parser.h - Recursive-descent Pascal parser -----------------*- C++ -*-===//
//
// Grammar accepted so far (M4):
//
//   program     = "program" ident [ "(" identList ")" ] ";" block "." .
//   block       = [ varPart ] compoundStmt .     (label/const/type/routines later)
//   varPart     = "var" identList ":" type ";" { identList ":" type ";" } .
//   type        = typeIdent .                     (structured types: M7+)
//   compound    = "begin" statement { ";" statement } "end" .
//   statement   = [ assignment | procCall | compound ] .
//   assignment  = ident ":=" expression .
//   procCall    = ident [ "(" actualParam { "," actualParam } ")" ] .
//   actualParam = expression [ ":" expression [ ":" expression ] ] .
//
//   expression  = simpleExpr [ relOp simpleExpr ] .
//   simpleExpr  = [ "+" | "-" ] term { addOp term } .
//   term        = factor { mulOp factor } .
//   factor      = unsignedConst | ident | ident "(" exprList ")"
//               | "(" expression ")" | "not" factor .
//   relOp = "=" | "<>" | "<" | "<=" | ">" | ">=" | "in"
//   addOp = "+" | "-" | "or"       mulOp = "*" | "/" | "div" | "mod" | "and"
//
// Note Pascal's precedence: `and` is a multiplying and `or` an adding
// operator, so `a < b and c < d` means `a < (b and c) < d` -- which is an
// error we diagnose with a hint to add parentheses.
//
// Error recovery: report, then skip to a synchronizing token (';', 'end',
// 'begin', ...) and continue. Constructs from later milestones are reported
// as "not supported yet" rather than as confusing syntax errors.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_PARSE_PARSER_H
#define PCC_PARSE_PARSER_H

#include "pcc/AST/AST.h"
#include "pcc/Basic/LangOptions.h"
#include "pcc/Lexer/Token.h"

#include "llvm/ADT/SmallVector.h"

#include <initializer_list>
#include <optional>

namespace pcc {

class ASTContext;
class DiagnosticsEngine;
class Lexer;

class Parser {
  Lexer &Lex;
  DiagnosticsEngine &Diags;
  ASTContext &Ctx;
  const LangOptions &LangOpts;

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
  void unsupported(llvm::StringRef What);

  bool startsStatement() const;
  bool startsDeclarationPart() const;

  // Declarations
  Block *parseBlock();
  void parseVarPart(llvm::SmallVectorImpl<VarDecl *> &Vars);
  TypeSpec *parseType();

  // Statements
  CompoundStmt *parseCompoundStmt();
  Stmt *parseStatement();
  Stmt *parseAssignmentOrCall();
  /// "(" param { "," param } ")" -- AllowFormat permits e:w:p (write/writeln).
  bool parseActualParams(llvm::SmallVectorImpl<Expr *> &Args, bool AllowFormat);

  // Expressions
  Expr *parseExpression();
  Expr *parseSimpleExpression();
  Expr *parseTerm();
  Expr *parseFactor();
  std::optional<BinaryOp> peekRelOp() const;
  std::optional<BinaryOp> peekAddOp() const;
  std::optional<BinaryOp> peekMulOp() const;

public:
  Parser(Lexer &Lex, DiagnosticsEngine &Diags, ASTContext &Ctx);

  /// Parses a whole program. Always returns a (possibly partial) AST;
  /// check the DiagnosticsEngine for errors.
  ProgramDecl *parseProgram();
};

} // namespace pcc

#endif // PCC_PARSE_PARSER_H
