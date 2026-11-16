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
#include "pcc/Basic/Directives.h"
#include "pcc/Basic/LangOptions.h"
#include "pcc/Lexer/Token.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"

#include <functional>
#include <initializer_list>
#include <optional>

namespace pcc {

class ASTContext;
class DiagnosticsEngine;
class Lexer;

/// The tokens of a generic routine (objfpc), parsed again for each
/// specialization, and the lexer switches in effect where it was declared.
struct GenericTokens {
  llvm::ArrayRef<Token> Tokens;
  bool LongStrings = false, AdvancedRecords = false, CorbaInterfaces = false,
       InlineOn = false, TypeHelpers = false, ScopedEnums = false,
       ResultSwitch = false, OutSwitch = false, DefaultParamsSwitch = false;
  unsigned PackRecords = 0;
  /// A generic type's: the next of its method implementations (a list
  /// that starts at the type's tokens), or null.
  GenericTokens *NextMethod = nullptr;
};

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
  /// Generic routines: the tokens consumed are recorded here; or tokens are
  /// replayed from a generic's instead of being lexed.
  llvm::SmallVectorImpl<Token> *Recording = nullptr;
  const GenericTokens *Replay = nullptr;
  size_t ReplayPos = 0;
  /// Parsing a generic routine: type parameters may follow its name
  /// (GenericTypeParams receives them).
  bool ParsingGeneric = false;
  /// The generic types declared so far, by name: a method implementation
  /// for one of them (TList.Add) is a template.
  llvm::StringMap<TypeDecl *> GenericTypes;
  /// specialize TList<integer>.Create in an expression: the type is given
  /// a hidden name ($spec1 = specialize TList<integer>), declared in the
  /// block being parsed, which gets these declarations when it ends.
  /// (Also the hidden routines of lambdas, $lambda1: see parseLambda.)
  llvm::SmallVector<Decl *, 4> HiddenTypeDecls;
  unsigned HiddenTypes = 0;
  unsigned HiddenLambdas = 0;
  Expr *parseLambda();
  GenericTokens *captureSwitches();
  TypeDecl *parseTypeDecl(bool InMembers, bool Generic);
  TypeSpec *parseSpecializeType();
  llvm::ArrayRef<Identifier> GenericTypeParams;
  /// declared(NAME) in $IF: the used units' names (their own, and those of
  /// their interfaces), given the units of the uses clauses read since the
  /// last call and the unit being parsed (if any); set by the driver.
  std::function<std::vector<std::string>(llvm::ArrayRef<Identifier>,
                                         llvm::StringRef)>
      UsedUnitNames;
  llvm::SmallVector<Identifier, 4> PendingUses;
  llvm::StringRef UnitName;
  void loadPendingUses();
  /// declared(NAME): Name is declared from here on.
  void declareName(const Identifier &Name);
  void nextToken(Token &T);
  // The lexer's switches (a replayed generic's: as where it was declared).
  bool longStrings() const;
  bool advancedRecords() const;
  bool typeHelpers() const;
  bool scopedEnums() const;
  unsigned packRecords() const;
  bool resultSwitch() const;
  bool outSwitch() const;
  bool defaultParamsSwitch() const;
  bool corbaInterfaces() const;
  bool inlineOn() const;
  bool startsGeneric();
  bool startsSpecialize();
  RoutineDecl *parseGenericRoutine(bool HeadingOnly);
  /// specialize Name<T1, ...>, after which a call's arguments may follow.
  bool parseSpecialization(Identifier &Name,
                           llvm::SmallVectorImpl<TypeSpec *> &TypeArgs);
  bool HasPeek = false;
  SourceLocation PrevTokEnd;  ///< end of the last consumed token
  /// The body of a lambda that copies everything it uses ([=]): the names
  /// consumed (not after a '.'), or null. LastKind: the last token consumed.
  std::vector<Identifier> *LambdaNames = nullptr;
  tok::TokenKind LastKind = tok::unknown;

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
  /// `class constructor Name;` / `class destructor Name;` in a class or an
  /// advanced record (after `class`).
  RoutineDecl *parseClassInitHeading();
  bool ParsingClassInit = false; ///< in parseRoutineDecl: `class constructor`
  /// Makes R a class constructor or destructor (a static class procedure).
  void markClassInit(RoutineDecl *R, ClassInitKind K);
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
  TypeSpec *parseRecordTypeBody(SourceLocation Start, bool Packed);
  TypeSpec *parseAdvancedRecord(SourceLocation Start, SourceLocation RecordLoc,
                                bool Packed);
  /// parseFieldList skipped to the record's `end` after an error.
  bool RecordBodySkipped = false;
  TypeSpec *parseClassType(SourceLocation Start);
  TypeSpec *parseInterfaceType(SourceLocation Start);
  /// After `type` (`record`), at `helper`: a type (record) helper (objfpc).
  TypeSpec *parseTypeHelper(SourceLocation Start, bool Record = false);
  bool parseClassVarNames(llvm::SmallVectorImpl<Decl *> &Members,
                          Visibility Vis);
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
  bool startsCheckedBlock();
  Stmt *parseCheckedBlock();
  bool startsCheckedExpr();
  Expr *parseCheckedExpr();
  void setCheckedSwitches(SourceLocation L, bool Checked,
                          CheckDirectives::State &Before,
                          CheckDirectives::State &WrapBefore);
  Stmt *parseAssignmentOrCall();
  Stmt *finishDesignatorStmt(SourceLocation Start, Expr *Target);
  /// The statement starting with a designator being parsed: its tokens so
  /// far (for += -= *= /=, which read the designator again), or null.
  llvm::SmallVectorImpl<Token> *StmtTokens = nullptr;
  /// A designator statement, with StmtTokens recording its tokens.
  template <typename ParseFn> Stmt *recordingDesignator(ParseFn Parse);
  bool isAssignOp() const;
  /// Target := Value, or Target op= Value (Free Pascal's C operators:
  /// Target := Target op (Value), Target parsed again from StmtTokens).
  Stmt *parseAssignment(SourceLocation Start, Expr *Target);
  /// The designator of Toks parsed again (as a factor).
  Expr *reparseDesignator(llvm::ArrayRef<Token> Toks);
  Expr *parseInherited();
  Stmt *parseVarDeclStmt();
  Stmt *parseDestructure(SourceLocation Start, llvm::ArrayRef<Expr *> Targets,
                         llvm::ArrayRef<VarDecl *> Vars);
  TypeSpec *parseTupleType(bool Always);
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
  /// First: the expression's first factor, already parsed (no sign).
  Expr *parseSimpleExpression(Expr *First = nullptr);
  Expr *parseTerm(Expr *First = nullptr);
  Expr *parseFactor();
  /// Applies `[...]` and `.field` selectors to Base.
  Expr *parseSelectors(Expr *Base);
  /// Is the token the start of a template string?
  bool atTemplate() const {
    return Tok.is(tok::template_head) ||
           (Tok.is(tok::string_literal) && Tok.isTemplate());
  }
  bool parseTemplateParts(llvm::SmallVectorImpl<Expr *> &Texts,
                          llvm::SmallVectorImpl<Expr *> &Values);
  Expr *parseTemplate();
  bool parseTemplateArgs(llvm::SmallVectorImpl<Expr *> &Args);
  /// ident { selector }
  Expr *parseDesignator();
  std::optional<BinaryOp> peekRelOp() const;
  std::optional<BinaryOp> peekAddOp() const;
  std::optional<BinaryOp> peekMulOp() const;

public:
  Parser(Lexer &Lex, DiagnosticsEngine &Diags, ASTContext &Ctx);

  /// A specialization of generic routine G: its declaration parsed again
  /// from its tokens (an ordinary routine, named as G).
  RoutineDecl *reparseGeneric(const RoutineDecl *G);
  /// A specialization of generic type G: its declaration parsed again (an
  /// ordinary type declaration, named as G), and in Methods the
  /// implementations of its methods.
  TypeDecl *reparseGenericType(const TypeDecl *G,
                               llvm::SmallVectorImpl<Decl *> &Methods);
  Lexer &getLexer() { return Lex; }
  ~Parser();
  /// Makes declared(NAME) in $IF see the used units' declarations (F
  /// loads the units named and returns their names).
  void enableDeclared(
      std::function<std::vector<std::string>(llvm::ArrayRef<Identifier>,
                                             llvm::StringRef)>
          F);

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
