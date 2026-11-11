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

  /// Nesting of expressions, statements and types being parsed. Deeper
  /// than MaxNesting is a fatal error (instead of a stack overflow).
  unsigned Nesting = 0;
  static constexpr unsigned MaxNesting = 256;
  /// Enters one nesting level; false (after a fatal error) if too deep.
  bool enterNesting();

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
  /// The value of a typed constant: an expression or (TP) an aggregate.
  Expr *parseTypedConstValue();

  // Declarations
  Block *parseBlock();
  /// Declaration parts; InInterface: routine headings without bodies.
  void parseDeclarations(llvm::SmallVectorImpl<Decl *> &Decls, bool InInterface);
  bool parseUsesClause(llvm::SmallVectorImpl<Identifier> &Uses);
  /// InMembers: a section of a class or advanced record, which ends at a
  /// visibility word (private, public, ...).
  void parseConstPart(llvm::SmallVectorImpl<Decl *> &Decls, bool InMembers = false);
  void parseTypePart(llvm::SmallVectorImpl<Decl *> &Decls, bool InMembers = false);
  bool isVisibilityWord() const;
  /// A type name, qualified by enclosing classes (objfpc): TOuter.TInner.
  TypeSpec *parseNamedType();
  void parseVarPart(llvm::SmallVectorImpl<Decl *> &Decls);
  RoutineDecl *parseRoutineDecl(bool HeadingOnly = false);
  /// InRecord: in an advanced record (`static` allowed); RecordClassMethod:
  /// a record's class method, which must be `static`.
  RoutineDecl *parseMethodHeading(bool InRecord = false,
                                  bool RecordClassMethod = false);
  /// Advanced records (objfpc): `class operator +(...): T;` in a record, the
  /// operator's name (`operator+`), and `class operator TR.+(...)` ... body.
  RoutineDecl *parseOperatorHeading();
  bool parseInlineDirective(SourceLocation Loc);
  /// Free Pascal's hint directives (Decl::HintFlag): written after a
  /// declaration's type or value (Separated: after a routine's or
  /// property's `;`, each one followed by `;`).
  struct Hints {
    unsigned char Flags = 0;
    llvm::StringRef Message;
  };
  unsigned char hintFlagOf(const Token &T) const;
  Hints parseHints();
  bool parseSeparatedHint(Hints &H);
  /// A calling convention word (cdecl, stdcall) at Tok, or Default.
  CallConv callConvOf(const Token &T) const;
  /// Consumes the calling convention word at Tok into CC (warning if one
  /// was already given).
  void takeCallConv(CallConv &CC);
  bool parseOperatorName(Identifier &Name);
  RoutineDecl *parseOperatorDecl(bool HeadingOnly);
  PropertyDecl *parsePropertyDecl();
  /// Close: tok::r_paren, or tok::r_square for an array property's indexes.
  bool parseFormalParams(llvm::SmallVectorImpl<VarDecl *> &Params,
                         tok::TokenKind Close = tok::r_paren);

  // Types
  TypeSpec *parseType();
  TypeSpec *parseTypeImpl();
  TypeSpec *parseSimpleType();
  TypeSpec *parseTypeIdentifier(llvm::StringRef Context);
  TypeSpec *parseArrayType(SourceLocation Start, bool Packed);
  TypeSpec *parseRecordType(SourceLocation Start, bool Packed);
  TypeSpec *parseAdvancedRecord(SourceLocation Start, SourceLocation RecordLoc,
                                bool Packed);
  /// parseFieldList skipped to the record's `end` after an error.
  bool RecordBodySkipped = false;
  TypeSpec *parseClassType(SourceLocation Start);
  TypeSpec *parseInterfaceType(SourceLocation Start);
  FieldList *parseFieldList(tok::TokenKind Close);
  VariantPart *parseVariantPart(tok::TokenKind Close);

  // Statements
  CompoundStmt *parseCompoundStmt();
  /// Parses statements separated by ';' up to (not including) Terminator
  /// (or Alt, if given: the body of a try statement ends at `except` or
  /// `finally`). OpenLoc/OpenWord name the construct for "to match this
  /// ..." notes.
  void parseStatementSequence(llvm::SmallVectorImpl<Stmt *> &Body,
                              tok::TokenKind Terminator,
                              SourceLocation OpenLoc, llvm::StringRef OpenWord,
                              tok::TokenKind Alt = tok::eof);
  Stmt *parseStatement();
  Stmt *parseStatementImpl();
  Stmt *parseAssignmentOrCall();
  Stmt *finishDesignatorStmt(SourceLocation Start, Expr *Target);
  Expr *parseInherited();
  Stmt *parseIfStmt();
  Stmt *parseWhileStmt();
  Stmt *parseRepeatStmt();
  Stmt *parseForStmt();
  Stmt *parseCaseStmt();
  Stmt *parseWithStmt();
  Stmt *parseTryStmt();
  Stmt *parseRaiseStmt();
  bool isOnHandler() const;
  void diagNoGoto();
  CaseLabel *parseCaseLabel(bool AllowRange);
  Expr *parseConstant();
  bool isCaseElse() const;
  /// "(" param { "," param } ")" -- AllowFormat permits e:w:p (write/writeln).
  bool parseActualParams(llvm::SmallVectorImpl<Expr *> &Args, bool AllowFormat);

  // Expressions
  Expr *parseExpression();
  Expr *parseExpressionImpl();
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

  /// Does the input start with `unit` (a unit file, Turbo/FPC)?
  bool startsWithUnit() const { return Tok.is(tok::kw_unit); }
};

} // namespace pcc

#endif // PCC_PARSE_PARSER_H
