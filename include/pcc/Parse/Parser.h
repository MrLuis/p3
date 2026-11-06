//===- Parser.h - Recursive-descent Pascal parser -----------------*- C++ -*-===//
//
// Grammar accepted so far (M8):
//
//   program     = "program" ident [ "(" identList ")" ] ";" block "." .
//   block       = { constPart | typePart | varPart | routineDecl } compound .
//                 (ISO: in that order, each part at most once; TP: any order)
//   constPart   = "const" ident "=" constExpr ";" { ident "=" constExpr ";" } .
//   typePart    = "type" ident "=" type ";" { ident "=" type ";" } .
//   varPart     = "var" identList ":" type ";" { identList ":" type ";" } .
//   routineDecl = ("procedure" ident [params] | "function" ident [params]
//                 [":" typeIdent]) ";" ( "forward" | block ) ";" .
//   params      = "(" ["var"] identList ":" typeIdent { ";" ... } ")" .
//
//   type        = simpleType | ["packed"] (arrayType | recordType) .
//   simpleType  = typeIdent | "(" identList ")" | constExpr ".." constExpr .
//   arrayType   = "array" "[" simpleType { "," simpleType } "]" "of" type .
//   recordType  = "record" fieldList "end" .
//   fieldList   = [ identList ":" type { ";" identList ":" type } ]
//                 [ ";" variantPart ] [ ";" ] .
//   variantPart = "case" [ ident ":" ] typeIdent "of"
//                 caseLabels ":" "(" fieldList ")" { ";" ... } .
//
//   statement   = [ assignment | procCall | compound | if | while | repeat
//                 | for | case | with ] .
//   assignment  = designator ":=" expression .
//   designator  = ident { "[" expression { "," expression } "]" | "." ident } .
//   with        = "with" designator { "," designator } "do" statement .
//   (if/while/repeat/for/case: see M5 notes in the implementation)
//
//   expression  = simpleExpr [ relOp simpleExpr ] .
//   simpleExpr  = [ "+" | "-" ] term { addOp term } .
//   term        = factor { mulOp factor } .
//   factor      = unsignedNumber | string | designator | ident "(" args ")"
//               | "(" expression ")" | "not" factor .
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
  Token PeekTok;              ///< the token after Tok, if HasPeek
  bool HasPeek = false;
  SourceLocation PrevTokEnd;  ///< end of the last consumed token

  // Token helpers
  SourceLocation consumeToken();
  const Token &peekToken();
  bool tryConsume(tok::TokenKind K);
  /// If the current token is K, consume it. Otherwise report
  /// "expected 'K' <Context>" right after the previous token and return false.
  bool expect(tok::TokenKind K, llvm::StringRef Context);
  /// Skips tokens until one of Stop (not consumed) or EOF.
  void skipUntil(std::initializer_list<tok::TokenKind> Stop);
  static std::string describe(tok::TokenKind K);
  Identifier makeIdentifier(const Token &T) const;
  Identifier makeLabel(const Token &T) const;
  /// Consumes an identifier into Out, or reports "expected <What>".
  bool expectIdentifier(Identifier &Out, llvm::StringRef What);
  bool parseIdentList(llvm::SmallVectorImpl<Identifier> &Names,
                      llvm::StringRef What);
  void unsupported(llvm::StringRef What);

  bool startsStatement() const;
  bool startsDeclarationPart() const;
  bool isIdentifierNamed(llvm::StringRef Name) const;

  // Declarations
  Block *parseBlock();
  /// Declaration parts; InInterface: routine headings without bodies.
  void parseDeclarations(llvm::SmallVectorImpl<Decl *> &Decls, bool InInterface);
  bool parseUsesClause(llvm::SmallVectorImpl<Identifier> &Uses);
  void parseConstPart(llvm::SmallVectorImpl<Decl *> &Decls);
  void parseTypePart(llvm::SmallVectorImpl<Decl *> &Decls);
  void parseVarPart(llvm::SmallVectorImpl<Decl *> &Decls);
  RoutineDecl *parseRoutineDecl(bool HeadingOnly = false);
  bool parseFormalParams(llvm::SmallVectorImpl<VarDecl *> &Params);

  // Types
  TypeSpec *parseType();
  TypeSpec *parseSimpleType();
  TypeSpec *parseTypeIdentifier(llvm::StringRef Context);
  TypeSpec *parseArrayType(SourceLocation Start, bool Packed);
  TypeSpec *parseRecordType(SourceLocation Start, bool Packed);
  FieldList *parseFieldList(tok::TokenKind Close);
  VariantPart *parseVariantPart(tok::TokenKind Close);

  // Statements
  CompoundStmt *parseCompoundStmt();
  /// Parses statements separated by ';' up to (not including) Terminator.
  /// OpenLoc/OpenWord name the construct for "to match this ..." notes.
  void parseStatementSequence(llvm::SmallVectorImpl<Stmt *> &Body,
                              tok::TokenKind Terminator,
                              SourceLocation OpenLoc, llvm::StringRef OpenWord);
  Stmt *parseStatement();
  Stmt *parseAssignmentOrCall();
  Stmt *parseIfStmt();
  Stmt *parseWhileStmt();
  Stmt *parseRepeatStmt();
  Stmt *parseForStmt();
  Stmt *parseCaseStmt();
  Stmt *parseWithStmt();
  CaseLabel *parseCaseLabel(bool AllowRange);
  Expr *parseConstant();
  bool isCaseElse() const;
  /// "(" param { "," param } ")" -- AllowFormat permits e:w:p (write/writeln).
  bool parseActualParams(llvm::SmallVectorImpl<Expr *> &Args, bool AllowFormat);

  // Expressions
  Expr *parseExpression();
  Expr *parseSimpleExpression();
  Expr *parseTerm();
  Expr *parseFactor();
  /// Applies `[...]` and `.field` selectors to Base.
  Expr *parseSelectors(Expr *Base);
  /// ident { selector }
  Expr *parseDesignator();
  std::optional<BinaryOp> peekRelOp() const;
  std::optional<BinaryOp> peekAddOp() const;
  std::optional<BinaryOp> peekMulOp() const;

public:
  Parser(Lexer &Lex, DiagnosticsEngine &Diags, ASTContext &Ctx);

  /// Parses a whole program. Always returns a (possibly partial) AST;
  /// check the DiagnosticsEngine for errors.
  ProgramDecl *parseProgram();

  /// Parses a unit file (Turbo/FPC). Returns null on a fatal syntax error.
  UnitDecl *parseUnit();
};

} // namespace pcc

#endif // PCC_PARSE_PARSER_H
