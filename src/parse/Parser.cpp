#include "pcc/Parse/Parser.h"
#include "pcc/AST/ASTContext.h"
#include "pcc/Basic/Diagnostics.h"
#include "pcc/Lexer/Lexer.h"

using namespace pcc;

Parser::Parser(Lexer &Lex, DiagnosticsEngine &Diags, ASTContext &Ctx)
    : Lex(Lex), Diags(Diags), Ctx(Ctx), LangOpts(Lex.getLangOpts()) {
  Lex.lex(Tok);
}

//===----------------------------------------------------------------------===//
// Token helpers
//===----------------------------------------------------------------------===//

SourceLocation Parser::consumeToken() {
  SourceLocation L = Tok.getLocation();
  PrevTokEnd = Tok.getEndLocation();
  if (HasPeek) {
    Tok = PeekTok;
    HasPeek = false;
  } else {
    Lex.lex(Tok);
  }
  return L;
}

const Token &Parser::peekToken() {
  if (!HasPeek) {
    Lex.lex(PeekTok);
    HasPeek = true;
  }
  return PeekTok;
}

bool Parser::tryConsume(tok::TokenKind K) {
  if (Tok.isNot(K))
    return false;
  consumeToken();
  return true;
}

std::string Parser::describe(tok::TokenKind K) {
  switch (K) {
  case tok::identifier:      return "identifier";
  case tok::integer_literal: return "integer";
  case tok::real_literal:    return "real number";
  case tok::string_literal:  return "string";
  case tok::eof:             return "end of file";
  default:
    if (const char *S = tok::getSpelling(K))
      return std::string("'") + S + "'";
    return tok::getTokenName(K);
  }
}

bool Parser::expect(tok::TokenKind K, llvm::StringRef Context) {
  if (tryConsume(K))
    return true;
  // Point just after the previous token: that is where the missing token
  // belongs (like clang's "expected ';' after expression").
  SourceLocation Where = PrevTokEnd.isValid() ? PrevTokEnd : Tok.getLocation();
  std::string Msg = "expected " + describe(K);
  if (!Context.empty())
    Msg += " " + Context.str();
  Diags.error(Where, Msg);
  return false;
}

void Parser::skipUntil(std::initializer_list<tok::TokenKind> Stop) {
  while (Tok.isNot(tok::eof)) {
    for (tok::TokenKind K : Stop)
      if (Tok.is(K))
        return;
    consumeToken();
  }
}

Identifier Parser::makeIdentifier(const Token &T) const {
  return {T.getIdentifier(), T.getRawText(), T.getLocation()};
}

Identifier Parser::makeLabel(const Token &T) const {
  // Labels are compared by value: `0010` and `10` are the same label.
  llvm::StringRef Name =
      Ctx.getStrings().intern(std::to_string(T.getIntegerValue()));
  return {Name, T.getRawText(), T.getLocation()};
}

bool Parser::expectIdentifier(Identifier &Out, llvm::StringRef What) {
  if (Tok.isNot(tok::identifier)) {
    Diags.error(Tok.getLocation(), "expected " + What);
    return false;
  }
  Out = makeIdentifier(Tok);
  consumeToken();
  return true;
}

bool Parser::parseIdentList(llvm::SmallVectorImpl<Identifier> &Names,
                            llvm::StringRef What) {
  do {
    Identifier Id;
    if (!expectIdentifier(Id, What))
      return false;
    Names.push_back(Id);
  } while (tryConsume(tok::comma));
  return true;
}

void Parser::unsupported(llvm::StringRef What) {
  Diags.error(Tok.getLocation(), What + " not supported yet", {Tok.getRange()});
}

bool Parser::startsStatement() const {
  return Tok.isOneOf(tok::identifier, tok::kw_begin, tok::kw_if,
                     tok::kw_while, tok::kw_repeat, tok::kw_for, tok::kw_case,
                     tok::kw_with, tok::kw_goto, tok::integer_literal,
                     tok::kw_inherited);
}

bool Parser::startsDeclarationPart() const {
  if (LangOpts.ObjKeywords && Tok.isOneOf(tok::kw_constructor, tok::kw_destructor))
    return true; // method implementations
  return Tok.isOneOf(tok::kw_label, tok::kw_const, tok::kw_type, tok::kw_var,
                     tok::kw_procedure, tok::kw_function);
}

bool Parser::enterNesting() {
  if (Nesting >= MaxNesting) {
    if (!Diags.shouldStop())
      Diags.fatal(Tok.getLocation(),
                  "nesting too deep (more than " + llvm::Twine(MaxNesting) +
                      " levels of expressions, statements or types)");
    return false;
  }
  ++Nesting;
  return true;
}

bool Parser::isIdentifierNamed(llvm::StringRef Name) const {
  return Tok.is(tok::identifier) && Tok.getIdentifier() == Name;
}

//===----------------------------------------------------------------------===//
// program = "program" ident [ "(" identList ")" ] ";" block "." .
//===----------------------------------------------------------------------===//

ProgramDecl *Parser::parseProgram() {
  SourceLocation ProgramLoc = Tok.getLocation();
  Identifier Name;
  llvm::SmallVector<Identifier, 4> Params;

  if (!tryConsume(tok::kw_program)) {
    Diags.error(Tok.getLocation(), "expected 'program'");
    // Recover by looking for the program's block.
    while (Tok.isNot(tok::eof) && !Tok.is(tok::kw_begin) &&
           !startsDeclarationPart())
      consumeToken();
  } else {
    if (Tok.is(tok::identifier)) {
      Name = makeIdentifier(Tok);
      consumeToken();
    } else {
      Diags.error(Tok.getLocation(), "expected program name after 'program'");
    }

    if (Tok.is(tok::l_paren)) {
      SourceLocation LParen = consumeToken();
      do {
        if (Tok.is(tok::identifier)) {
          Params.push_back(makeIdentifier(Tok));
          consumeToken();
        } else {
          Diags.error(Tok.getLocation(), "expected program parameter name");
          skipUntil({tok::comma, tok::r_paren, tok::semi, tok::kw_begin});
        }
      } while (tryConsume(tok::comma));
      if (!expect(tok::r_paren, "after program parameters"))
        Diags.note(LParen, "to match this '('");
    }

    if (!expect(tok::semi, "after program heading"))
      skipUntil({tok::semi, tok::kw_begin, tok::kw_var, tok::kw_const,
                 tok::kw_type, tok::kw_label, tok::kw_procedure,
                 tok::kw_function});
    tryConsume(tok::semi);
  }

  llvm::SmallVector<Identifier, 4> Uses;
  if (Tok.is(tok::kw_uses)) {
    parseUsesClause(Uses);
  } else if (isIdentifierNamed("uses") && peekToken().is(tok::identifier)) {
    // `uses` is not reserved in ISO Pascal.
    Diags.error(Tok.getLocation(),
                "units are a Turbo/Free Pascal extension; use --mode=tp",
                {Tok.getRange()});
    skipUntil({tok::semi});
    tryConsume(tok::semi);
  }

  Block *B = parseBlock();

  if (!Diags.shouldStop()) {
    if (Tok.is(tok::period)) {
      consumeToken();
      if (Tok.isNot(tok::eof))
        Diags.warning(Tok.getLocation(), "text after end of program is ignored");
    } else {
      expect(tok::period, "at end of program");
    }
  }

  return Ctx.create<ProgramDecl>(ProgramLoc, Name,
                                 Ctx.copyArray<Identifier>(Params), B,
                                 Ctx.copyArray<Identifier>(Uses));
}

// uses = "uses" ident { "," ident } ";" .
bool Parser::parseUsesClause(llvm::SmallVectorImpl<Identifier> &Uses) {
  consumeToken(); // 'uses'
  if (!parseIdentList(Uses, "unit name") || !expect(tok::semi, "after uses clause")) {
    skipUntil({tok::semi, tok::kw_begin, tok::kw_var, tok::kw_const,
               tok::kw_type, tok::kw_procedure, tok::kw_function});
    tryConsume(tok::semi);
    return false;
  }
  return true;
}

// unit = "unit" ident ";" "interface" [uses] decls
//        "implementation" [uses] decls [("begin"|"initialization") stmts] "end" "." .
UnitDecl *Parser::parseUnit() {
  SourceLocation UnitLoc = Tok.getLocation();
  if (!tryConsume(tok::kw_unit)) {
    Diags.error(Tok.getLocation(), "expected 'unit'");
    return nullptr;
  }
  Identifier Name;
  if (!expectIdentifier(Name, "unit name") || !expect(tok::semi, "after unit name"))
    return nullptr;

  if (!expect(tok::kw_interface, "after unit heading"))
    return nullptr;
  llvm::SmallVector<Identifier, 4> IfaceUses, ImplUses;
  if (Tok.is(tok::kw_uses))
    parseUsesClause(IfaceUses);
  llvm::SmallVector<Decl *, 16> IfaceDecls;
  parseDeclarations(IfaceDecls, /*InInterface=*/true);

  if (!expect(tok::kw_implementation, "after the interface part"))
    return nullptr;
  if (Tok.is(tok::kw_uses))
    parseUsesClause(ImplUses);
  llvm::SmallVector<Decl *, 16> ImplDecls;
  parseDeclarations(ImplDecls, /*InInterface=*/false);

  // Optional initialization part.
  llvm::SmallVector<Stmt *, 8> Init;
  SourceLocation InitLoc = Tok.getLocation();
  if (Tok.isOneOf(tok::kw_begin, tok::kw_initialization)) {
    InitLoc = consumeToken();
    parseStatementSequence(Init, tok::kw_end, InitLoc, "begin");
  }
  SourceLocation EndLoc = Tok.getEndLocation();
  if (!expect(tok::kw_end, "at end of unit") || !expect(tok::period, "after 'end'"))
    return nullptr;

  auto *Empty = Ctx.create<CompoundStmt>(SourceRange(InitLoc), llvm::ArrayRef<Stmt *>());
  auto *Body = Ctx.create<CompoundStmt>(SourceRange(InitLoc, EndLoc),
                                        Ctx.copyArray<Stmt *>(Init));
  return Ctx.create<UnitDecl>(
      UnitLoc, Name, Ctx.copyArray<Identifier>(IfaceUses),
      Ctx.create<Block>(Ctx.copyArray<Decl *>(IfaceDecls), Empty),
      Ctx.copyArray<Identifier>(ImplUses),
      Ctx.create<Block>(Ctx.copyArray<Decl *>(ImplDecls), Body));
}

//===----------------------------------------------------------------------===//
// block = { declaration parts } compoundStmt .
//===----------------------------------------------------------------------===//

/// ISO 7185 6.2.1 order of declaration parts.
static int sectionRank(tok::TokenKind K) {
  switch (K) {
  case tok::kw_label: return 0;
  case tok::kw_const: return 1;
  case tok::kw_type:  return 2;
  case tok::kw_var:   return 3;
  default:            return 4; // procedure / function
  }
}

Block *Parser::parseBlock() {
  llvm::SmallVector<Decl *, 16> Decls;
  parseDeclarations(Decls, /*InInterface=*/false);

  CompoundStmt *Body = nullptr;
  if (Tok.is(tok::kw_begin)) {
    Body = parseCompoundStmt();
  } else {
    Diags.error(Tok.getLocation(), "expected 'begin'");
    skipUntil({tok::kw_begin});
    if (Tok.is(tok::kw_begin))
      Body = parseCompoundStmt();
  }
  if (!Body)
    Body = Ctx.create<CompoundStmt>(SourceRange(Tok.getLocation()),
                                    llvm::ArrayRef<Stmt *>());
  return Ctx.create<Block>(Ctx.copyArray<Decl *>(Decls), Body);
}

void Parser::parseDeclarations(llvm::SmallVectorImpl<Decl *> &Decls,
                               bool InInterface) {
  int LastRank = -1;
  llvm::StringRef LastWord;

  while (startsDeclarationPart() && !Diags.shouldStop()) {
    int Rank = sectionRank(Tok.getKind());
    std::string Word = Tok.getRawText().lower();
    if (LangOpts.Mode == LangMode::ISO) {
      if (Rank < LastRank)
        Diags.error(Tok.getLocation(),
                    "in ISO Pascal, '" + Word +
                        "' declarations must come before '" + LastWord +
                        "' declarations; use --mode=tp to mix them",
                    {Tok.getRange()});
      else if (Rank == LastRank && Rank != 4)
        Diags.error(Tok.getLocation(),
                    "ISO Pascal allows only one '" + Word +
                        "' section per block",
                    {Tok.getRange()});
    }
    if (Rank >= LastRank) {
      LastRank = Rank;
      LastWord = tok::getSpelling(Tok.getKind());
    }

    switch (Tok.getKind()) {
    case tok::kw_const:
      parseConstPart(Decls);
      break;
    case tok::kw_type:
      parseTypePart(Decls);
      break;
    case tok::kw_var:
      parseVarPart(Decls);
      break;
    case tok::kw_procedure:
    case tok::kw_function:
    case tok::kw_constructor:
    case tok::kw_destructor:
      if (RoutineDecl *R = parseRoutineDecl(InInterface))
        Decls.push_back(R);
      break;
    default: { // label 1, 2, 3;
      consumeToken();
      do {
        if (Tok.isNot(tok::integer_literal)) {
          Diags.error(Tok.getLocation(), "expected label number");
          skipUntil({tok::semi, tok::kw_begin});
          break;
        }
        Decls.push_back(Ctx.create<LabelDecl>(makeLabel(Tok)));
        consumeToken();
      } while (tryConsume(tok::comma));
      if (!expect(tok::semi, "after label declaration")) {
        skipUntil({tok::semi, tok::kw_begin});
        tryConsume(tok::semi);
      }
      break;
    }
    }
  }
}

// typedConstValue = expression | aggregate .
// aggregate = "(" typedConstValue { "," typedConstValue } ")"
//           | "(" ident ":" typedConstValue { ";" ident ":" typedConstValue } ")" .
// A parenthesized scalar `(1 + 2)` parses as a one-element aggregate.
Expr *Parser::parseTypedConstValue() {
  if (Tok.isNot(tok::l_paren))
    return parseExpression();
  SourceLocation LParen = consumeToken();
  llvm::SmallVector<AggregateElement *, 8> Elems;
  bool RecordForm = Tok.is(tok::identifier) && peekToken().is(tok::colon);
  do {
    if (RecordForm && Tok.is(tok::r_paren))
      break; // (x: 1; y: 2;) -- trailing ';'
    Identifier Field;
    if (RecordForm && (!expectIdentifier(Field, "field name") ||
                       !expect(tok::colon, "after field name")))
      return nullptr;
    Expr *V = parseTypedConstValue();
    if (!V)
      return nullptr;
    Elems.push_back(Ctx.create<AggregateElement>(Field, V));
  } while (tryConsume(RecordForm ? tok::semi : tok::comma));
  if (!expect(tok::r_paren, "after typed constant value")) {
    Diags.note(LParen, "to match this '('");
    return nullptr;
  }
  return Ctx.create<AggregateExpr>(SourceRange(LParen, PrevTokEnd),
                                   Ctx.copyArray<AggregateElement *>(Elems));
}

// constPart = "const" ident "=" constExpr ";" { ident "=" constExpr ";" } .
void Parser::parseConstPart(llvm::SmallVectorImpl<Decl *> &Decls) {
  consumeToken(); // 'const'
  if (Tok.isNot(tok::identifier)) {
    Diags.error(Tok.getLocation(), "expected constant name after 'const'");
    return;
  }
  while (Tok.is(tok::identifier) && !Diags.shouldStop()) {
    Identifier Name = makeIdentifier(Tok);
    consumeToken();
    if (Tok.is(tok::colon) && LangOpts.Mode != LangMode::ISO) {
      // Typed constant: const x: T = value;
      consumeToken();
      TypeSpec *TS = parseType();
      Expr *Init = nullptr;
      if (TS && expect(tok::equal, "in typed constant") &&
          (Init = parseTypedConstValue())) {
        auto *V = Ctx.create<VarDecl>(Name, TS);
        V->setInit(Init);
        Decls.push_back(V);
      } else {
        skipUntil({tok::semi, tok::kw_begin});
      }
      if (!expect(tok::semi, "after typed constant")) {
        skipUntil({tok::semi, tok::kw_begin});
        tryConsume(tok::semi);
      }
      continue;
    }
    if (Tok.is(tok::colon_equal) || Tok.is(tok::colon)) {
      Diags.error(Tok.getLocation(),
                  "expected '=' in constant declaration (typed constants need "
                  "--mode=tp)",
                  {Tok.getRange()});
      skipUntil({tok::semi, tok::kw_begin});
      tryConsume(tok::semi);
      continue;
    }
    Expr *Init = nullptr;
    if (expect(tok::equal, "after constant name"))
      Init = parseExpression();
    if (Init)
      Decls.push_back(Ctx.create<ConstDecl>(Name.Loc, Name, Init));
    else
      skipUntil({tok::semi, tok::kw_begin});
    if (!expect(tok::semi, "after constant declaration")) {
      skipUntil({tok::semi, tok::kw_begin});
      tryConsume(tok::semi);
    }
  }
}

// typePart = "type" ident "=" type ";" { ident "=" type ";" } .
void Parser::parseTypePart(llvm::SmallVectorImpl<Decl *> &Decls) {
  consumeToken(); // 'type'
  if (Tok.isNot(tok::identifier)) {
    Diags.error(Tok.getLocation(), "expected type name after 'type'");
    return;
  }
  while (Tok.is(tok::identifier) && !Diags.shouldStop()) {
    Identifier Name = makeIdentifier(Tok);
    consumeToken();
    TypeSpec *TS = nullptr;
    if (expect(tok::equal, "after type name"))
      TS = parseType();
    if (TS)
      Decls.push_back(Ctx.create<TypeDecl>(Name.Loc, Name, TS));
    else
      skipUntil({tok::semi, tok::kw_begin});
    if (!expect(tok::semi, "after type declaration")) {
      skipUntil({tok::semi, tok::kw_begin});
      tryConsume(tok::semi);
    }
  }
}

// varPart = "var" identList ":" type ";" { identList ":" type ";" } .
void Parser::parseVarPart(llvm::SmallVectorImpl<Decl *> &Decls) {
  consumeToken(); // 'var'
  if (Tok.isNot(tok::identifier)) {
    Diags.error(Tok.getLocation(), "expected variable name after 'var'");
    return;
  }

  while (Tok.is(tok::identifier) && !Diags.shouldStop()) {
    llvm::SmallVector<Identifier, 4> Names;
    TypeSpec *TS = nullptr;
    if (parseIdentList(Names, "variable name") &&
        expect(tok::colon, "after variable name"))
      TS = parseType();

    if (!TS) {
      skipUntil({tok::semi, tok::kw_begin});
    } else {
      for (const Identifier &N : Names)
        Decls.push_back(Ctx.create<VarDecl>(N, TS));
    }

    if (!expect(tok::semi, "after variable declaration")) {
      skipUntil({tok::semi, tok::kw_begin});
      tryConsume(tok::semi);
    }
  }
}

static MethodKind methodKindOf(tok::TokenKind K) {
  switch (K) {
  case tok::kw_constructor: return MethodKind::Constructor;
  case tok::kw_destructor:  return MethodKind::Destructor;
  default:                  return MethodKind::None;
  }
}

static const char *routineKindWord(tok::TokenKind K) {
  switch (K) {
  case tok::kw_function:    return "function";
  case tok::kw_constructor: return "constructor";
  case tok::kw_destructor:  return "destructor";
  default:                  return "procedure";
  }
}

// routineDecl = ("procedure" name [params] | "function" name [params]
//               [":" typeIdent]) ";" ( "forward" | block ) ";" .
// name = ident [ "." ident ]   (objfpc: a method implementation, TClass.M;
// constructors and destructors are always methods)
RoutineDecl *Parser::parseRoutineDecl(bool HeadingOnly) {
  bool IsFunction = Tok.is(tok::kw_function);
  MethodKind MK = methodKindOf(Tok.getKind());
  const char *Kind = routineKindWord(Tok.getKind());
  SourceLocation Loc = consumeToken();

  Identifier Name, ClassName;
  if (!expectIdentifier(Name, std::string(Kind) + " name")) {
    skipUntil({tok::semi, tok::kw_begin});
    tryConsume(tok::semi);
  } else if (LangOpts.ObjKeywords && Tok.is(tok::period)) {
    consumeToken();
    ClassName = Name;
    if (!expectIdentifier(Name, "method name after '.'")) {
      Name = Identifier();
      skipUntil({tok::semi, tok::kw_begin});
      tryConsume(tok::semi);
    }
  }
  if (Name.isValid() && MK != MethodKind::None && !ClassName.isValid())
    Diags.error(Name.Loc,
                std::string("a ") + Kind +
                    " must be declared in a class; implement it as '" + Kind +
                    " TClass." + Name.Spelling.str() + "'",
                {Name.getRange()});
  if (ClassName.isValid() && MK == MethodKind::None)
    MK = MethodKind::Method;

  llvm::SmallVector<VarDecl *, 4> Params;
  bool HasParamList = Tok.is(tok::l_paren);
  if (HasParamList && !parseFormalParams(Params))
    skipUntil({tok::colon, tok::semi, tok::kw_begin});

  TypeSpec *ResultTS = nullptr;
  if (tryConsume(tok::colon)) {
    if (!IsFunction)
      Diags.error(PrevTokEnd, std::string("a ") + Kind +
                                  " cannot have a result type");
    ResultTS = parseTypeIdentifier("function result type");
  }

  if (!expect(tok::semi, std::string("after ") + Kind + " heading"))
    skipUntil({tok::semi, tok::kw_begin, tok::kw_var, tok::kw_const,
               tok::kw_type});
  tryConsume(tok::semi);

  // Turbo/FPC directives:  cdecl;  external ['lib'] [name 'symbol'];
  bool IsExternal = false;
  llvm::StringRef ExternalName;
  while (LangOpts.Mode != LangMode::ISO) {
    if (isIdentifierNamed("cdecl")) { // the only calling convention
      consumeToken();
      expect(tok::semi, "after 'cdecl'");
      continue;
    }
    if (!isIdentifierNamed("external"))
      break;
    consumeToken();
    IsExternal = true;
    if (Tok.is(tok::string_literal)) // library name: linking is the user's job
      consumeToken();
    if (isIdentifierNamed("name")) {
      consumeToken();
      if (Tok.is(tok::string_literal)) {
        ExternalName = Tok.getStringValue();
        consumeToken();
      } else {
        Diags.error(Tok.getLocation(), "expected the external symbol name");
      }
    }
    expect(tok::semi, "after 'external'");
  }
  if (IsExternal) {
    if (!Name.isValid())
      return nullptr;
    auto *R = Ctx.create<RoutineDecl>(Loc, Name, IsFunction,
                                      Ctx.copyArray<VarDecl *>(Params),
                                      HasParamList, ResultTS, nullptr, false);
    R->setExternal(ExternalName);
    return R;
  }

  if (HeadingOnly && ClassName.isValid()) {
    Diags.error(ClassName.Loc,
                "methods are implemented in the implementation part of a unit",
                {SourceRange(ClassName.Loc, Name.getRange().getEnd())});
    return nullptr;
  }
  if (HeadingOnly) // interface part of a unit: the body comes later
    return Name.isValid()
               ? Ctx.create<RoutineDecl>(Loc, Name, IsFunction,
                                         Ctx.copyArray<VarDecl *>(Params),
                                         HasParamList, ResultTS, nullptr, true)
               : nullptr;

  if (isIdentifierNamed("forward")) {
    consumeToken();
    expect(tok::semi, "after 'forward'");
    if (!Name.isValid())
      return nullptr;
    return Ctx.create<RoutineDecl>(Loc, Name, IsFunction,
                                   Ctx.copyArray<VarDecl *>(Params),
                                   HasParamList, ResultTS, nullptr, true);
  }

  Block *Body = parseBlock();
  if (!expect(tok::semi, std::string("after ") + Kind + " body")) {
    skipUntil({tok::semi, tok::kw_begin, tok::kw_procedure, tok::kw_function});
    tryConsume(tok::semi);
  }
  if (!Name.isValid())
    return nullptr;
  auto *R = Ctx.create<RoutineDecl>(Loc, Name, IsFunction,
                                    Ctx.copyArray<VarDecl *>(Params),
                                    HasParamList, ResultTS, Body, false);
  R->setMethodKind(MK);
  R->setClassName(ClassName);
  return R;
}

// methodHeading = ("procedure" | "function" | "constructor" | "destructor")
//                 ident [params] [":" typeIdent] ";" { directive ";" } .
// directive     = "virtual" | "override" | "abstract" .
RoutineDecl *Parser::parseMethodHeading() {
  bool IsFunction = Tok.is(tok::kw_function);
  MethodKind MK = methodKindOf(Tok.getKind());
  if (MK == MethodKind::None)
    MK = MethodKind::Method;
  const char *Kind = routineKindWord(Tok.getKind());
  SourceLocation Loc = consumeToken();

  Identifier Name;
  if (!expectIdentifier(Name, std::string(Kind) + " name"))
    return nullptr;
  llvm::SmallVector<VarDecl *, 4> Params;
  bool HasParamList = Tok.is(tok::l_paren);
  if (HasParamList && !parseFormalParams(Params))
    return nullptr;
  TypeSpec *ResultTS = nullptr;
  if (tryConsume(tok::colon)) {
    if (!IsFunction)
      Diags.error(PrevTokEnd, std::string("a ") + Kind +
                                  " cannot have a result type");
    if (!(ResultTS = parseTypeIdentifier("function result type")))
      return nullptr;
  }
  if (!expect(tok::semi, std::string("after ") + Kind + " heading"))
    return nullptr;

  bool Virtual = false, Override = false, Abstract = false;
  while (Tok.is(tok::identifier)) {
    llvm::StringRef W = Tok.getIdentifier();
    if (W == "virtual") {
      Virtual = true;
    } else if (W == "override") {
      Override = true;
    } else if (W == "abstract") {
      Abstract = true;
    } else if (W == "reintroduce" || W == "overload" || W == "dynamic" ||
               W == "static" || W == "message" || W == "final" ||
               W == "cdecl" || W == "external" || W == "forward") {
      unsupported("'" + W.str() + "' on methods is");
      return nullptr;
    } else {
      break; // the next member (a field)
    }
    std::string Word = W.str();
    consumeToken();
    if (!expect(tok::semi, "after '" + Word + "'"))
      return nullptr;
  }
  auto *R = Ctx.create<RoutineDecl>(Loc, Name, IsFunction,
                                    Ctx.copyArray<VarDecl *>(Params),
                                    HasParamList, ResultTS, nullptr, false);
  R->setMethodKind(MK);
  R->setDirectives(Virtual, Override, Abstract);
  return R;
}

// property = "property" ident ":" typeIdent [ "read" ident ] [ "write" ident ] ";" .
PropertyDecl *Parser::parsePropertyDecl() {
  consumeToken(); // 'property'
  Identifier Name;
  if (!expectIdentifier(Name, "property name"))
    return nullptr;
  if (Tok.is(tok::l_square)) {
    unsupported("array properties are");
    return nullptr;
  }
  if (!expect(tok::colon, "after property name"))
    return nullptr;
  TypeSpec *TS = parseTypeIdentifier("property type");
  if (!TS)
    return nullptr;
  Identifier Read, Write;
  while (Tok.is(tok::identifier)) {
    llvm::StringRef W = Tok.getIdentifier();
    if (W == "read" || W == "write") {
      consumeToken();
      if (!expectIdentifier(W == "read" ? Read : Write,
                            "field or method name after '" + W.str() + "'"))
        return nullptr;
      continue;
    }
    if (W == "default" || W == "nodefault" || W == "stored" || W == "index" ||
        W == "implements") {
      unsupported("'" + W.str() + "' in a property is");
      return nullptr;
    }
    break;
  }
  if (!expect(tok::semi, "after property declaration"))
    return nullptr;
  return Ctx.create<PropertyDecl>(Name, TS, Read, Write);
}

// classType = "class" ";"                                  (forward)
//           | "class" [ "(" ident ")" ] ( ";"? | { member } "end" ) .
// member    = visibility | identList ":" type ";" | methodHeading | property .
TypeSpec *Parser::parseClassType(SourceLocation Start) {
  SourceLocation ClassLoc = consumeToken(); // 'class'
  if (Tok.is(tok::semi)) // TFoo = class;
    return Ctx.create<ClassTypeSpec>(SourceRange(Start, PrevTokEnd),
                                     Identifier(), llvm::ArrayRef<Decl *>(),
                                     /*Forward=*/true);
  if (Tok.is(tok::kw_of)) {
    unsupported("class reference types are");
    return nullptr;
  }
  Identifier Parent;
  if (Tok.is(tok::l_paren)) {
    SourceLocation LParen = consumeToken();
    if (!expectIdentifier(Parent, "parent class name"))
      return nullptr;
    if (Tok.is(tok::comma)) {
      unsupported("interfaces are");
      return nullptr;
    }
    if (!expect(tok::r_paren, "after parent class name")) {
      Diags.note(LParen, "to match this '('");
      return nullptr;
    }
    if (Tok.is(tok::semi)) // TFoo = class(TBar);
      return Ctx.create<ClassTypeSpec>(SourceRange(Start, PrevTokEnd), Parent,
                                       llvm::ArrayRef<Decl *>(), false);
  }

  llvm::SmallVector<Decl *, 16> Members;
  Visibility Vis = Visibility::Public;
  auto Fail = [&]() -> TypeSpec * {
    skipUntil({tok::kw_end});
    tryConsume(tok::kw_end);
    return nullptr;
  };
  while (Tok.isNot(tok::kw_end) && Tok.isNot(tok::eof) && !Diags.shouldStop()) {
    if (Tok.is(tok::identifier)) {
      llvm::StringRef W = Tok.getIdentifier();
      if (W == "private" || W == "protected" || W == "public" ||
          W == "published") {
        Vis = W == "private"     ? Visibility::Private
              : W == "protected" ? Visibility::Protected
              : W == "public"    ? Visibility::Public
                                 : Visibility::Published;
        consumeToken();
        continue;
      }
      if (W == "strict") {
        unsupported("'strict' visibility is");
        return Fail();
      }
      llvm::SmallVector<Identifier, 4> Names;
      if (!parseIdentList(Names, "field name") ||
          !expect(tok::colon, "after field name"))
        return Fail();
      TypeSpec *TS = parseType();
      if (!TS)
        return Fail();
      for (const Identifier &N : Names) {
        auto *F = Ctx.create<FieldDecl>(N, TS);
        F->setVisibility(Vis);
        Members.push_back(F);
      }
      if (!expect(tok::semi, "after field declaration"))
        return Fail();
      continue;
    }
    if (Tok.isOneOf(tok::kw_procedure, tok::kw_function, tok::kw_constructor,
                    tok::kw_destructor)) {
      RoutineDecl *R = parseMethodHeading();
      if (!R)
        return Fail();
      R->setVisibility(Vis);
      Members.push_back(R);
      continue;
    }
    if (Tok.is(tok::kw_property)) {
      PropertyDecl *P = parsePropertyDecl();
      if (!P)
        return Fail();
      P->setVisibility(Vis);
      Members.push_back(P);
      continue;
    }
    if (Tok.is(tok::kw_class)) {
      unsupported("class methods and class variables are");
      return Fail();
    }
    Diags.error(Tok.getLocation(),
                "expected a field, method or property declaration, or 'end'");
    return Fail();
  }
  if (!expect(tok::kw_end, "at end of class type")) {
    Diags.note(ClassLoc, "to match this 'class'");
    return nullptr;
  }
  return Ctx.create<ClassTypeSpec>(SourceRange(Start, PrevTokEnd), Parent,
                                   Ctx.copyArray<Decl *>(Members), false);
}

// params = "(" section { ";" section } ")" ;
// section = ["var" | "const"] identList ":" typeIdent .   ("const": TP)
bool Parser::parseFormalParams(llvm::SmallVectorImpl<VarDecl *> &Params) {
  SourceLocation LParen = consumeToken(); // '('
  do {
    if (Tok.isOneOf(tok::kw_procedure, tok::kw_function)) {
      // procedure p(params) / function f(params): T
      bool IsFunction = Tok.is(tok::kw_function);
      SourceLocation Start = consumeToken();
      Identifier Name;
      if (!expectIdentifier(Name, "parameter name")) {
        skipUntil({tok::semi, tok::r_paren});
        continue;
      }
      llvm::SmallVector<VarDecl *, 4> Inner;
      if (Tok.is(tok::l_paren) && !parseFormalParams(Inner)) {
        skipUntil({tok::semi, tok::r_paren});
        continue;
      }
      TypeSpec *Result = nullptr;
      if (IsFunction) {
        if (!expect(tok::colon, "before the result type of a functional parameter") ||
            !(Result = parseTypeIdentifier("function result type"))) {
          skipUntil({tok::semi, tok::r_paren});
          continue;
        }
      }
      auto *TS = Ctx.create<RoutineTypeSpec>(SourceRange(Start, PrevTokEnd),
                                             IsFunction,
                                             Ctx.copyArray<VarDecl *>(Inner),
                                             Result);
      Params.push_back(Ctx.create<VarDecl>(Name, TS, VarDecl::ValueParam));
      continue;
    }
    bool IsVar = tryConsume(tok::kw_var);
    bool IsConst = false;
    if (!IsVar && Tok.is(tok::kw_const) && LangOpts.Mode != LangMode::ISO) {
      consumeToken();
      IsConst = true;
    }
    llvm::SmallVector<Identifier, 4> Names;
    if (!parseIdentList(Names, "parameter name") ||
        !expect(tok::colon, "after parameter name")) {
      skipUntil({tok::semi, tok::r_paren});
      continue;
    }
    TypeSpec *TS = parseTypeIdentifier("parameter type");
    if (!TS) {
      skipUntil({tok::semi, tok::r_paren});
      continue;
    }
    for (const Identifier &N : Names)
      Params.push_back(Ctx.create<VarDecl>(
          N, TS,
          IsVar ? VarDecl::VarParam
                : IsConst ? VarDecl::ConstParam : VarDecl::ValueParam));
  } while (tryConsume(tok::semi));

  if (!expect(tok::r_paren, "after parameters")) {
    Diags.note(LParen, "to match this '('");
    return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

TypeSpec *Parser::parseType() {
  if (!enterNesting())
    return nullptr;
  TypeSpec *TS = parseTypeImpl();
  --Nesting;
  return TS;
}

// type = simpleType | ["packed"] (arrayType | recordType) .
TypeSpec *Parser::parseTypeImpl() {
  SourceLocation Start = Tok.getLocation();
  bool Packed = tryConsume(tok::kw_packed);
  switch (Tok.getKind()) {
  case tok::kw_array:
    return parseArrayType(Start, Packed);
  case tok::kw_record:
    return parseRecordType(Start, Packed);
  case tok::kw_class:
    if (Packed)
      break;
    return parseClassType(Start);
  case tok::kw_set: {
    consumeToken();
    if (!expect(tok::kw_of, "after 'set'"))
      return nullptr;
    TypeSpec *Elem = parseSimpleType();
    if (!Elem)
      return nullptr;
    return Ctx.create<SetTypeSpec>(SourceRange(Start, PrevTokEnd), Elem);
  }
  case tok::kw_string: { // string [ '[' N ']' ]
    consumeToken();
    Expr *MaxLen = nullptr;
    if (tryConsume(tok::l_square)) {
      if (!(MaxLen = parseExpression()) || !expect(tok::r_square, "after string length"))
        return nullptr;
    }
    return Ctx.create<StringTypeSpec>(SourceRange(Start, PrevTokEnd), MaxLen);
  }
  case tok::kw_file: {
    consumeToken();
    if (!expect(tok::kw_of, "after 'file'"))
      return nullptr;
    TypeSpec *Elem = parseType();
    if (!Elem)
      return nullptr;
    return Ctx.create<FileTypeSpec>(SourceRange(Start, PrevTokEnd), Elem);
  }
  case tok::kw_procedure:
  case tok::kw_function: {
    // Procedural type (Turbo/FPC): procedure(params) / function(params): T
    if (Packed || LangOpts.Mode == LangMode::ISO)
      break;
    bool IsFunction = Tok.is(tok::kw_function);
    consumeToken();
    llvm::SmallVector<VarDecl *, 4> Params;
    if (Tok.is(tok::l_paren) && !parseFormalParams(Params))
      return nullptr;
    TypeSpec *Result = nullptr;
    if (IsFunction && (!expect(tok::colon, "before the result type") ||
                       !(Result = parseTypeIdentifier("function result type"))))
      return nullptr;
    return Ctx.create<RoutineTypeSpec>(SourceRange(Start, PrevTokEnd),
                                       IsFunction,
                                       Ctx.copyArray<VarDecl *>(Params), Result);
  }
  case tok::caret: {
    if (Packed)
      break;
    consumeToken();
    Identifier Target;
    if (!expectIdentifier(Target, "type name after '^'"))
      return nullptr;
    return Ctx.create<PointerTypeSpec>(SourceRange(Start, PrevTokEnd), Target);
  }
  default:
    break;
  }
  {
    if (Packed) {
      Diags.error(Tok.getLocation(),
                  "'packed' must be followed by 'array', 'record' or 'set'");
      return nullptr;
    }
    // `class` is not reserved without classes: TFoo = class ... end
    if (isIdentifierNamed("class") &&
        peekToken().isOneOf(tok::l_paren, tok::identifier, tok::kw_end,
                            tok::kw_procedure, tok::kw_function)) {
      Diags.error(Tok.getLocation(),
                  "classes are an Object Pascal extension; use --mode=objfpc",
                  {Tok.getRange()});
      skipUntil({tok::kw_end}); // skip the class body
      tryConsume(tok::kw_end);
      return nullptr;
    }
    return parseSimpleType();
  }
}

// typeIdent (ISO requires these for parameters and function results)
TypeSpec *Parser::parseTypeIdentifier(llvm::StringRef Context) {
  if (Tok.is(tok::kw_string)) { // Turbo/FPC: `string` names a type here
    auto *TS = Ctx.create<StringTypeSpec>(Tok.getRange(), nullptr);
    consumeToken();
    return TS;
  }
  if (Tok.is(tok::identifier)) {
    auto *TS = Ctx.create<NamedTypeSpec>(makeIdentifier(Tok));
    consumeToken();
    return TS;
  }
  if (Tok.isOneOf(tok::kw_array, tok::kw_packed) && Context == "parameter type")
    unsupported("conformant array parameters are");
  else
    Diags.error(Tok.getLocation(),
                "expected type name for " + Context +
                    " (declare the type in a 'type' section)");
  return nullptr;
}

/// Tokens that can continue a constant expression after an identifier.
static bool continuesConstExpr(tok::TokenKind K) {
  switch (K) {
  case tok::dotdot: case tok::plus: case tok::minus: case tok::star:
  case tok::kw_div: case tok::kw_mod:
    return true;
  default:
    return false;
  }
}

// simpleType = typeIdent | "(" identList ")" | constExpr ".." constExpr .
TypeSpec *Parser::parseSimpleType() {
  SourceLocation Start = Tok.getLocation();

  if (Tok.is(tok::l_paren)) {
    consumeToken();
    llvm::SmallVector<Identifier, 8> Names;
    if (!parseIdentList(Names, "enumeration value name"))
      return nullptr;
    if (!expect(tok::r_paren, "after enumeration values"))
      return nullptr;
    return Ctx.create<EnumTypeSpec>(SourceRange(Start, PrevTokEnd),
                                    Ctx.copyArray<Identifier>(Names));
  }

  if (Tok.is(tok::identifier) && !continuesConstExpr(peekToken().getKind())) {
    auto *TS = Ctx.create<NamedTypeSpec>(makeIdentifier(Tok));
    consumeToken();
    return TS;
  }

  if (!Tok.isOneOf(tok::identifier, tok::integer_literal, tok::string_literal,
                   tok::plus, tok::minus)) {
    Diags.error(Tok.getLocation(), "expected type");
    return nullptr;
  }
  // Subrange: constant bounds (constant expressions in --mode=tp; Sema
  // enforces the ISO form).
  Expr *Lo = parseSimpleExpression();
  if (!Lo)
    return nullptr;
  if (!expect(tok::dotdot, "in subrange type"))
    return nullptr;
  Expr *Hi = parseSimpleExpression();
  if (!Hi)
    return nullptr;
  return Ctx.create<SubrangeTypeSpec>(SourceRange(Start, PrevTokEnd), Lo, Hi);
}

// arrayType = "array" "[" simpleType { "," simpleType } "]" "of" type .
TypeSpec *Parser::parseArrayType(SourceLocation Start, bool Packed) {
  consumeToken(); // 'array'
  SourceLocation LBrack = Tok.getLocation();
  if (!expect(tok::l_square, "after 'array'"))
    return nullptr;
  llvm::SmallVector<TypeSpec *, 2> Indexes;
  do {
    TypeSpec *Idx = parseSimpleType();
    if (!Idx)
      return nullptr;
    Indexes.push_back(Idx);
  } while (tryConsume(tok::comma));
  if (!expect(tok::r_square, "after array index types")) {
    Diags.note(LBrack, "to match this '['");
    return nullptr;
  }
  if (!expect(tok::kw_of, "after array index types"))
    return nullptr;
  TypeSpec *Elem = parseType();
  if (!Elem)
    return nullptr;
  return Ctx.create<ArrayTypeSpec>(SourceRange(Start, PrevTokEnd),
                                   Ctx.copyArray<TypeSpec *>(Indexes), Elem,
                                   Packed);
}

// recordType = "record" fieldList "end" .
TypeSpec *Parser::parseRecordType(SourceLocation Start, bool Packed) {
  SourceLocation RecordLoc = consumeToken(); // 'record'
  FieldList *Fields = parseFieldList(tok::kw_end);
  if (!Fields)
    return nullptr;
  if (!expect(tok::kw_end, "at end of record type")) {
    Diags.note(RecordLoc, "to match this 'record'");
    return nullptr;
  }
  return Ctx.create<RecordTypeSpec>(SourceRange(Start, PrevTokEnd), Fields,
                                    Packed);
}

// fieldList = [ identList ":" type { ";" identList ":" type } ]
//             [ ";" variantPart ] [ ";" ] .
FieldList *Parser::parseFieldList(tok::TokenKind Close) {
  llvm::SmallVector<FieldDecl *, 8> Fixed;
  while (Tok.is(tok::identifier)) {
    llvm::SmallVector<Identifier, 4> Names;
    if (!parseIdentList(Names, "field name") ||
        !expect(tok::colon, "after field name"))
      return nullptr;
    TypeSpec *TS = parseType();
    if (!TS)
      return nullptr;
    for (const Identifier &N : Names)
      Fixed.push_back(Ctx.create<FieldDecl>(N, TS));
    if (!tryConsume(tok::semi))
      break;
  }

  VariantPart *VP = nullptr;
  if (Tok.is(tok::kw_case)) {
    VP = parseVariantPart(Close);
    if (!VP)
      return nullptr;
    tryConsume(tok::semi);
  }

  if (Tok.isNot(Close)) {
    Diags.error(Tok.getLocation(),
                "expected field name, 'case' or " + describe(Close) +
                    " in record");
    return nullptr;
  }
  auto *FL = Ctx.create<FieldList>();
  FL->Fixed = Ctx.copyArray<FieldDecl *>(Fixed);
  FL->Variant = VP;
  return FL;
}

// variantPart = "case" [ ident ":" ] typeIdent "of" variant { ";" variant } .
// variant     = caseLabel { "," caseLabel } ":" "(" fieldList ")" .
VariantPart *Parser::parseVariantPart(tok::TokenKind Close) {
  auto *VP = Ctx.create<VariantPart>();
  VP->CaseLoc = consumeToken(); // 'case'

  Identifier First;
  if (!expectIdentifier(First, "variant tag or tag type after 'case'"))
    return nullptr;
  if (tryConsume(tok::colon)) {
    TypeSpec *TagTS = parseTypeIdentifier("variant tag type");
    if (!TagTS)
      return nullptr;
    VP->Tag = Ctx.create<FieldDecl>(First, TagTS);
    VP->TagTypeSpec = TagTS;
  } else {
    VP->TagTypeSpec = Ctx.create<NamedTypeSpec>(First);
  }
  if (!expect(tok::kw_of, "after variant tag"))
    return nullptr;

  llvm::SmallVector<Variant *, 4> Variants;
  while (Tok.isNot(Close) && Tok.isNot(tok::eof)) {
    llvm::SmallVector<CaseLabel *, 4> Labels;
    do {
      CaseLabel *L = parseCaseLabel(/*AllowRange=*/false);
      if (!L)
        return nullptr;
      Labels.push_back(L);
    } while (tryConsume(tok::comma));
    if (!expect(tok::colon, "after variant label"))
      return nullptr;
    SourceLocation LParen = Tok.getLocation();
    if (!expect(tok::l_paren, "before variant fields"))
      return nullptr;
    FieldList *Fields = parseFieldList(tok::r_paren);
    if (!Fields)
      return nullptr;
    if (!expect(tok::r_paren, "after variant fields")) {
      Diags.note(LParen, "to match this '('");
      return nullptr;
    }
    auto *V = Ctx.create<Variant>();
    V->Labels = Ctx.copyArray<CaseLabel *>(Labels);
    V->Fields = Fields;
    Variants.push_back(V);
    if (!tryConsume(tok::semi))
      break;
  }
  if (Variants.empty()) {
    Diags.error(Tok.getLocation(), "expected at least one variant");
    return nullptr;
  }
  VP->Variants = Ctx.copyArray<Variant *>(Variants);
  return VP;
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

CompoundStmt *Parser::parseCompoundStmt() {
  SourceLocation BeginLoc = consumeToken(); // 'begin'
  llvm::SmallVector<Stmt *, 8> Body;
  parseStatementSequence(Body, tok::kw_end, BeginLoc, "begin");
  SourceLocation EndLoc = Tok.getEndLocation();
  tryConsume(tok::kw_end);
  return Ctx.create<CompoundStmt>(SourceRange(BeginLoc, EndLoc),
                                  Ctx.copyArray<Stmt *>(Body));
}

void Parser::parseStatementSequence(llvm::SmallVectorImpl<Stmt *> &Body,
                                    tok::TokenKind Terminator,
                                    SourceLocation OpenLoc,
                                    llvm::StringRef OpenWord) {
  auto NoteOpen = [&] {
    Diags.note(OpenLoc, "to match this '" + OpenWord + "'");
  };

  while (!Diags.shouldStop()) {
    Body.push_back(parseStatement());

    if (Tok.is(tok::semi)) {
      SourceLocation SemiLoc = consumeToken();
      // `if c then s; else t` -- the ';' already ended the if statement.
      if (Tok.is(tok::kw_else)) {
        Diags.error(SemiLoc, "';' before 'else' ends the 'if' statement; "
                             "remove the ';'");
        consumeToken(); // recover by treating the else branch as a statement
      }
      continue;
    }
    if (Tok.is(Terminator))
      return;
    if (Tok.is(tok::eof)) {
      Diags.error(Tok.getLocation(), "expected " + describe(Terminator));
      NoteOpen();
      return;
    }
    if (startsStatement()) {
      // Most likely a forgotten ';' -- report it and carry on parsing.
      Diags.error(PrevTokEnd, "expected ';' between statements");
      continue;
    }
    Diags.error(Tok.getLocation(), "expected ';' or " + describe(Terminator) +
                                       " after statement");
    skipUntil({tok::semi, Terminator});
    if (tryConsume(tok::semi))
      continue;
    if (Tok.isNot(Terminator)) {
      NoteOpen();
      return;
    }
  }
}

Stmt *Parser::parseStatement() {
  if (!enterNesting())
    return Ctx.create<EmptyStmt>(Tok.getLocation());
  Stmt *S = parseStatementImpl();
  --Nesting;
  return S;
}

Stmt *Parser::parseStatementImpl() {
  switch (Tok.getKind()) {
  case tok::identifier:
    return parseAssignmentOrCall();
  case tok::kw_begin:
    return parseCompoundStmt();
  case tok::kw_if:
    return parseIfStmt();
  case tok::kw_while:
    return parseWhileStmt();
  case tok::kw_repeat:
    return parseRepeatStmt();
  case tok::kw_for:
    return parseForStmt();
  case tok::kw_case:
    return parseCaseStmt();
  case tok::kw_with:
    return parseWithStmt();
  case tok::l_paren: { // objfpc: (obj as TFoo).Show;
    if (!LangOpts.ObjKeywords) {
      Diags.error(Tok.getLocation(), "expected statement");
      break;
    }
    SourceLocation Start = Tok.getLocation();
    Expr *Target = parseFactor();
    if (!Target) {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(Start);
    }
    return finishDesignatorStmt(Start, Target);
  }
  case tok::kw_inherited: {
    SourceLocation Start = Tok.getLocation();
    Expr *Target = parseInherited();
    if (!Target) {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(Start);
    }
    return finishDesignatorStmt(Start, Target);
  }

  // The empty statement: nothing before a separator or terminator.
  case tok::semi:
  case tok::kw_end:
  case tok::kw_else:
  case tok::kw_until:
  case tok::eof:
    return Ctx.create<EmptyStmt>(Tok.getLocation());

  case tok::kw_goto: {
    SourceLocation GotoLoc = consumeToken();
    if (Tok.isNot(tok::integer_literal)) {
      Diags.error(Tok.getLocation(), "expected label number after 'goto'");
      break;
    }
    Identifier Label = makeLabel(Tok);
    consumeToken();
    return Ctx.create<GotoStmt>(SourceRange(GotoLoc, PrevTokEnd), Label);
  }
  case tok::integer_literal: {
    Identifier Label = makeLabel(Tok);
    SourceLocation Start = consumeToken();
    if (!expect(tok::colon, "after statement label"))
      break;
    Stmt *Body = parseStatement();
    return Ctx.create<LabeledStmt>(SourceRange(Start, PrevTokEnd), Label, Body);
  }
  default:
    Diags.error(Tok.getLocation(), "expected statement");
    break;
  }

  SourceLocation L = Tok.getLocation();
  skipUntil({tok::semi, tok::kw_end});
  return Ctx.create<EmptyStmt>(L);
}

// assignment = designator ":=" expression .
// procCall   = ident [ "(" actualParam { "," actualParam } ")" ] .
Stmt *Parser::parseAssignmentOrCall() {
  Identifier Name = makeIdentifier(Tok);
  SourceLocation Start = consumeToken();

  Expr *Target = Ctx.create<NameRefExpr>(Name);
  bool HasSelectors = Tok.isOneOf(tok::l_square, tok::period, tok::caret);
  if (HasSelectors) {
    Target = parseSelectors(Target);
    if (!Target) {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(Start);
    }
    return finishDesignatorStmt(Start, Target);
  }

  // Common slip: '=' where ':=' was meant. Report it, then parse the
  // statement as an assignment anyway.
  if (Tok.is(tok::equal))
    Diags.error(Tok.getLocation(), "expected ':=' in assignment, found '='",
                {Tok.getRange()});

  if (Tok.isOneOf(tok::colon_equal, tok::equal)) {
    SourceLocation AssignLoc = consumeToken();
    Expr *Value = parseExpression();
    if (!Value) {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(Start);
    }
    return Ctx.create<AssignStmt>(SourceRange(Start, PrevTokEnd), Target,
                                  AssignLoc, Value);
  }

  llvm::SmallVector<Expr *, 4> Args;
  bool HasParens = Tok.is(tok::l_paren);
  if (HasParens)
    parseActualParams(Args, /*AllowFormat=*/true);

  // objfpc: selectors after a call or typecast, TFoo(x).Show; f(1)^.n := 2;
  if (HasParens && LangOpts.ObjKeywords &&
      Tok.isOneOf(tok::l_square, tok::period, tok::caret)) {
    Expr *Call = Ctx.create<CallExpr>(SourceRange(Start, PrevTokEnd), Name,
                                      Ctx.copyArray<Expr *>(Args));
    Target = parseSelectors(Call);
    if (!Target) {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(Start);
    }
    return finishDesignatorStmt(Start, Target);
  }

  return Ctx.create<ProcCallStmt>(SourceRange(Start, PrevTokEnd), Name,
                                  Ctx.copyArray<Expr *>(Args), HasParens);
}

// The rest of a statement that starts with a designator with selectors:
// an assignment, or (Turbo/FPC) a call through a procedural value or a
// method call: r.p(x); a[i]; obj.Show; inherited Create(x);
Stmt *Parser::finishDesignatorStmt(SourceLocation Start, Expr *Target) {
  if (Tok.is(tok::equal))
    Diags.error(Tok.getLocation(), "expected ':=' in assignment, found '='",
                {Tok.getRange()});

  if (Tok.isOneOf(tok::colon_equal, tok::equal)) {
    SourceLocation AssignLoc = consumeToken();
    Expr *Value = parseExpression();
    if (!Value) {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(Start);
    }
    return Ctx.create<AssignStmt>(SourceRange(Start, PrevTokEnd), Target,
                                  AssignLoc, Value);
  }

  if (LangOpts.Mode != LangMode::ISO &&
      Tok.isOneOf(tok::semi, tok::kw_end, tok::kw_else, tok::kw_until, tok::eof)) {
    auto *Call = llvm::dyn_cast<IndirectCallExpr>(Target);
    if (!Call) // r.p without arguments
      Call = Ctx.create<IndirectCallExpr>(Target->getRange(), Target,
                                          llvm::ArrayRef<Expr *>());
    return Ctx.create<IndirectCallStmt>(SourceRange(Start, PrevTokEnd), Call);
  }

  Diags.error(PrevTokEnd, "expected ':=' after variable");
  skipUntil({tok::semi, tok::kw_end});
  return Ctx.create<EmptyStmt>(Start);
}

// inherited [ ident ] { selector }   (objfpc)
Expr *Parser::parseInherited() {
  SourceLocation Loc = consumeToken(); // 'inherited'
  Identifier Name;
  if (Tok.is(tok::identifier)) {
    Name = makeIdentifier(Tok);
    consumeToken();
  }
  return parseSelectors(
      Ctx.create<InheritedExpr>(SourceRange(Loc, PrevTokEnd), Name));
}

bool Parser::parseActualParams(llvm::SmallVectorImpl<Expr *> &Args,
                               bool AllowFormat) {
  SourceLocation LParen = consumeToken(); // '('
  if (Tok.is(tok::r_paren)) {
    Diags.error(Tok.getLocation(), "expected expression",
                {SourceRange(LParen, Tok.getEndLocation())});
    Diags.note(LParen, "an empty argument list '()' is not allowed; "
                       "omit the parentheses");
    consumeToken();
    return false;
  }

  do {
    Expr *E = parseExpression();
    if (!E) {
      skipUntil({tok::comma, tok::r_paren, tok::semi, tok::kw_end});
      continue;
    }
    if (AllowFormat && Tok.is(tok::colon)) {
      consumeToken();
      Expr *Width = parseExpression();
      Expr *Precision = nullptr;
      bool OK = Width != nullptr;
      if (OK && tryConsume(tok::colon)) {
        Precision = parseExpression();
        OK = Precision != nullptr;
      }
      if (!OK) {
        skipUntil({tok::comma, tok::r_paren, tok::semi, tok::kw_end});
        continue;
      }
      E = Ctx.create<FormattedExpr>(SourceRange(E->getBeginLoc(), PrevTokEnd),
                                    E, Width, Precision);
    }
    Args.push_back(E);
  } while (tryConsume(tok::comma));

  if (!expect(tok::r_paren, "after arguments")) {
    Diags.note(LParen, "to match this '('");
    return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Structured statements
//===----------------------------------------------------------------------===//

// if = "if" expression "then" statement [ "else" statement ] .
// The dangling else binds to the nearest if: the inner parseIfStmt sees the
// 'else' first.
Stmt *Parser::parseIfStmt() {
  SourceLocation IfLoc = consumeToken();
  Expr *Cond = parseExpression();
  if (!Cond) {
    skipUntil({tok::kw_then, tok::semi, tok::kw_end});
    if (Tok.isNot(tok::kw_then))
      return Ctx.create<EmptyStmt>(IfLoc);
  }
  if (!expect(tok::kw_then, "after 'if' condition") && !startsStatement()) {
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(IfLoc);
  }
  Stmt *Then = parseStatement();
  Stmt *Else = nullptr;
  if (tryConsume(tok::kw_else))
    Else = parseStatement();
  if (!Cond)
    return Ctx.create<EmptyStmt>(IfLoc);
  return Ctx.create<IfStmt>(SourceRange(IfLoc, PrevTokEnd), Cond, Then, Else);
}

// while = "while" expression "do" statement .
Stmt *Parser::parseWhileStmt() {
  SourceLocation WhileLoc = consumeToken();
  Expr *Cond = parseExpression();
  if (!Cond)
    skipUntil({tok::kw_do, tok::semi, tok::kw_end});
  if (!expect(tok::kw_do, "after 'while' condition") && !startsStatement()) {
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(WhileLoc);
  }
  Stmt *Body = parseStatement();
  if (!Cond)
    return Ctx.create<EmptyStmt>(WhileLoc);
  return Ctx.create<WhileStmt>(SourceRange(WhileLoc, PrevTokEnd), Cond, Body);
}

// repeat = "repeat" stmtSeq "until" expression .
Stmt *Parser::parseRepeatStmt() {
  SourceLocation RepeatLoc = consumeToken();
  llvm::SmallVector<Stmt *, 8> Body;
  parseStatementSequence(Body, tok::kw_until, RepeatLoc, "repeat");
  if (!tryConsume(tok::kw_until))
    return Ctx.create<EmptyStmt>(RepeatLoc);
  Expr *Cond = parseExpression();
  if (!Cond)
    return Ctx.create<EmptyStmt>(RepeatLoc);
  return Ctx.create<RepeatStmt>(SourceRange(RepeatLoc, PrevTokEnd),
                                Ctx.copyArray<Stmt *>(Body), Cond);
}

// for = "for" ident ":=" expression ("to"|"downto") expression "do" statement .
Stmt *Parser::parseForStmt() {
  SourceLocation ForLoc = consumeToken();
  auto Fail = [&] {
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(ForLoc);
  };

  if (Tok.isNot(tok::identifier)) {
    Diags.error(Tok.getLocation(), "expected control variable after 'for'");
    return Fail();
  }
  auto *Var = Ctx.create<NameRefExpr>(makeIdentifier(Tok));
  consumeToken();

  if (Tok.is(tok::equal)) {
    Diags.error(Tok.getLocation(), "expected ':=' in 'for' statement, found '='",
                {Tok.getRange()});
    consumeToken();
  } else if (!expect(tok::colon_equal, "after 'for' control variable")) {
    return Fail();
  }

  Expr *Init = parseExpression();
  if (!Init)
    return Fail();

  bool Downto = Tok.is(tok::kw_downto);
  if (!Tok.isOneOf(tok::kw_to, tok::kw_downto)) {
    Diags.error(PrevTokEnd, "expected 'to' or 'downto' in 'for' statement");
    return Fail();
  }
  consumeToken();

  Expr *Final = parseExpression();
  if (!Final)
    return Fail();
  if (!expect(tok::kw_do, "in 'for' statement") && !startsStatement())
    return Fail();

  Stmt *Body = parseStatement();
  return Ctx.create<ForStmt>(SourceRange(ForLoc, PrevTokEnd), Var, Init, Final,
                             Downto, Body);
}

bool Parser::isCaseElse() const {
  return Tok.is(tok::kw_else) || isIdentifierNamed("otherwise");
}

// case = "case" expression "of" caseArm { ";" caseArm } [ ";" ]
//        [ ("else"|"otherwise") stmtSeq ] "end" .
Stmt *Parser::parseCaseStmt() {
  SourceLocation CaseLoc = consumeToken();
  Expr *Selector = parseExpression();
  if (!Selector)
    skipUntil({tok::kw_of, tok::kw_end});
  if (!expect(tok::kw_of, "after 'case' selector")) {
    skipUntil({tok::kw_end});
    tryConsume(tok::kw_end);
    return Ctx.create<EmptyStmt>(CaseLoc);
  }

  llvm::SmallVector<CaseArm *, 8> Arms;
  llvm::SmallVector<Stmt *, 4> ElseBody;
  SourceLocation ElseLoc;
  bool OK = Selector != nullptr;

  while (!Diags.shouldStop() && Tok.isNot(tok::kw_end) &&
         Tok.isNot(tok::eof)) {
    if (isCaseElse()) {
      if (LangOpts.Mode == LangMode::ISO)
        Diags.error(Tok.getLocation(),
                    "'" + Tok.getRawText() +
                        "' in a 'case' statement is a Turbo/Free Pascal "
                        "extension; use --mode=tp",
                    {Tok.getRange()});
      ElseLoc = consumeToken();
      tryConsume(tok::colon); // some dialects write `otherwise:`
      parseStatementSequence(ElseBody, tok::kw_end, CaseLoc, "case");
      break;
    }

    llvm::SmallVector<CaseLabel *, 4> Labels;
    do {
      if (CaseLabel *L = parseCaseLabel(/*AllowRange=*/true))
        Labels.push_back(L);
      else
        OK = false;
    } while (tryConsume(tok::comma));

    if (!expect(tok::colon, "after case label")) {
      OK = false;
      skipUntil({tok::semi, tok::kw_end});
      if (tryConsume(tok::semi))
        continue;
      break;
    }
    Stmt *Body = parseStatement();
    Arms.push_back(Ctx.create<CaseArm>(Ctx.copyArray<CaseLabel *>(Labels), Body));

    if (tryConsume(tok::semi))
      continue;
    if (Tok.is(tok::kw_end) || isCaseElse())
      continue;
    Diags.error(PrevTokEnd, "expected ';' or 'end' after case arm");
    OK = false;
    skipUntil({tok::semi, tok::kw_end});
    tryConsume(tok::semi);
  }

  if (Arms.empty() && OK) {
    Diags.error(Tok.getLocation(), "a 'case' statement needs at least one "
                                   "case label");
    OK = false;
  }
  SourceLocation EndLoc = Tok.getEndLocation();
  if (!tryConsume(tok::kw_end)) {
    Diags.error(Tok.getLocation(), "expected 'end' at end of 'case' statement");
    Diags.note(CaseLoc, "to match this 'case'");
    return Ctx.create<EmptyStmt>(CaseLoc);
  }
  if (!OK)
    return Ctx.create<EmptyStmt>(CaseLoc);
  return Ctx.create<CaseStmt>(SourceRange(CaseLoc, EndLoc), Selector,
                              Ctx.copyArray<CaseArm *>(Arms), ElseLoc,
                              Ctx.copyArray<Stmt *>(ElseBody));
}

// caseLabel = constant [ ".." constant ] .
CaseLabel *Parser::parseCaseLabel(bool AllowRange) {
  Expr *Lo = parseConstant();
  if (!Lo)
    return nullptr;
  Expr *Hi = nullptr;
  if (Tok.is(tok::dotdot)) {
    if (!AllowRange)
      Diags.error(Tok.getLocation(), "ranges are not allowed in variant labels",
                  {Tok.getRange()});
    else if (LangOpts.Mode == LangMode::ISO)
      Diags.error(Tok.getLocation(),
                  "case label ranges are a Turbo/Free Pascal extension; use "
                  "--mode=tp",
                  {Tok.getRange()});
    consumeToken();
    Hi = parseConstant();
    if (!Hi)
      return nullptr;
  }
  return Ctx.create<CaseLabel>(Lo, Hi);
}

// constant = [ "+" | "-" ] ( unsignedNumber | ident ) | string .
Expr *Parser::parseConstant() {
  if (Tok.isOneOf(tok::plus, tok::minus)) {
    UnaryOp Op = Tok.is(tok::plus) ? UnaryOp::Plus : UnaryOp::Minus;
    SourceLocation OpLoc = consumeToken();
    Expr *Operand = nullptr;
    if (Tok.is(tok::integer_literal)) {
      Operand = Ctx.create<IntegerLiteralExpr>(Tok.getRange(),
                                               Tok.getIntegerValue());
    } else if (Tok.is(tok::real_literal)) {
      Operand = Ctx.create<RealLiteralExpr>(Tok.getRange(), Tok.getRealValue());
    } else if (Tok.is(tok::identifier)) {
      Operand = Ctx.create<NameRefExpr>(makeIdentifier(Tok));
    } else {
      Diags.error(Tok.getLocation(), "expected constant after sign");
      return nullptr;
    }
    consumeToken();
    return Ctx.create<UnaryExpr>(
        SourceRange(OpLoc, Operand->getRange().getEnd()), Op, Operand);
  }

  Expr *E = nullptr;
  switch (Tok.getKind()) {
  case tok::integer_literal:
    E = Ctx.create<IntegerLiteralExpr>(Tok.getRange(), Tok.getIntegerValue());
    break;
  case tok::real_literal:
    E = Ctx.create<RealLiteralExpr>(Tok.getRange(), Tok.getRealValue());
    break;
  case tok::string_literal:
    E = Ctx.create<StringLiteralExpr>(Tok.getRange(), Tok.getStringValue());
    break;
  case tok::identifier:
    E = Ctx.create<NameRefExpr>(makeIdentifier(Tok));
    break;
  default:
    Diags.error(Tok.getLocation(), "expected constant");
    return nullptr;
  }
  consumeToken();
  return E;
}

// with = "with" designator { "," designator } "do" statement .
Stmt *Parser::parseWithStmt() {
  SourceLocation WithLoc = consumeToken();
  llvm::SmallVector<WithContext *, 2> Records;
  do {
    Expr *R = parseDesignator();
    if (!R) {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(WithLoc);
    }
    Records.push_back(Ctx.create<WithContext>(R));
  } while (tryConsume(tok::comma));
  if (!expect(tok::kw_do, "after 'with' records") && !startsStatement()) {
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(WithLoc);
  }
  Stmt *Body = parseStatement();
  return Ctx.create<WithStmt>(SourceRange(WithLoc, PrevTokEnd),
                              Ctx.copyArray<WithContext *>(Records), Body);
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

std::optional<BinaryOp> Parser::peekRelOp() const {
  switch (Tok.getKind()) {
  case tok::equal:         return BinaryOp::Eq;
  case tok::not_equal:     return BinaryOp::Ne;
  case tok::less:          return BinaryOp::Lt;
  case tok::less_equal:    return BinaryOp::Le;
  case tok::greater:       return BinaryOp::Gt;
  case tok::greater_equal: return BinaryOp::Ge;
  case tok::kw_in:         return BinaryOp::In;
  case tok::kw_is:         return BinaryOp::Is;
  default:                 return std::nullopt;
  }
}

std::optional<BinaryOp> Parser::peekAddOp() const {
  switch (Tok.getKind()) {
  case tok::plus:  return BinaryOp::Add;
  case tok::minus: return BinaryOp::Sub;
  case tok::kw_or: return BinaryOp::Or;
  case tok::kw_xor: return BinaryOp::Xor;
  default:         return std::nullopt;
  }
}

std::optional<BinaryOp> Parser::peekMulOp() const {
  switch (Tok.getKind()) {
  case tok::star:   return BinaryOp::Mul;
  case tok::slash:  return BinaryOp::RealDiv;
  case tok::kw_div: return BinaryOp::IntDiv;
  case tok::kw_mod: return BinaryOp::Mod;
  case tok::kw_and: return BinaryOp::And;
  case tok::kw_shl: return BinaryOp::Shl;
  case tok::kw_shr: return BinaryOp::Shr;
  case tok::kw_as:  return BinaryOp::As;
  default:          return std::nullopt;
  }
}

static Expr *makeBinary(ASTContext &Ctx, BinaryOp Op, SourceLocation OpLoc,
                        Expr *L, Expr *R) {
  SourceRange Range(L->getBeginLoc(), R->getRange().getEnd());
  return Ctx.create<BinaryExpr>(Range, Op, OpLoc, L, R);
}

Expr *Parser::parseExpression() {
  if (!enterNesting())
    return nullptr;
  Expr *E = parseExpressionImpl();
  --Nesting;
  return E;
}

// expression = simpleExpr [ relOp simpleExpr ] .
Expr *Parser::parseExpressionImpl() {
  Expr *LHS = parseSimpleExpression();
  if (!LHS)
    return nullptr;

  auto Op = peekRelOp();
  if (!Op)
    return LHS;
  SourceLocation OpLoc = consumeToken();
  Expr *RHS = parseSimpleExpression();
  if (!RHS)
    return nullptr;
  Expr *Result = makeBinary(Ctx, *Op, OpLoc, LHS, RHS);

  // `a < b and c < d` parses as `a < (b and c)` followed by `< d`.
  if (peekRelOp()) {
    Diags.error(Tok.getLocation(), "comparison operators cannot be chained",
                {Tok.getRange()});
    if (auto *B = llvm::dyn_cast<BinaryExpr>(RHS);
        B && (B->getOp() == BinaryOp::And || B->getOp() == BinaryOp::Or))
      Diags.note(B->getOpLoc(),
                 llvm::Twine("'") + getOpSpelling(B->getOp()) +
                     "' binds more tightly than comparison operators; use "
                     "parentheses, e.g. '(a < b) " + getOpSpelling(B->getOp()) +
                     " (c < d)'");
    // Recover: consume the rest of the chain.
    while (peekRelOp()) {
      consumeToken();
      if (!parseSimpleExpression())
        return nullptr;
    }
  }
  return Result;
}

// simpleExpr = [ "+" | "-" ] term { addOp term } .
Expr *Parser::parseSimpleExpression() {
  Expr *LHS;
  if (Tok.isOneOf(tok::plus, tok::minus)) {
    // ISO: the sign applies to the whole first term: -a*b == -(a*b).
    UnaryOp Op = Tok.is(tok::plus) ? UnaryOp::Plus : UnaryOp::Minus;
    SourceLocation OpLoc = consumeToken();
    Expr *Operand = parseTerm();
    if (!Operand)
      return nullptr;
    LHS = Ctx.create<UnaryExpr>(SourceRange(OpLoc, Operand->getRange().getEnd()),
                                Op, Operand);
  } else {
    LHS = parseTerm();
  }
  if (!LHS)
    return nullptr;

  while (auto Op = peekAddOp()) {
    SourceLocation OpLoc = consumeToken();
    Expr *RHS = parseTerm();
    if (!RHS)
      return nullptr;
    LHS = makeBinary(Ctx, *Op, OpLoc, LHS, RHS);
  }
  return LHS;
}

// term = factor { mulOp factor } .
Expr *Parser::parseTerm() {
  Expr *LHS = parseFactor();
  if (!LHS)
    return nullptr;
  while (auto Op = peekMulOp()) {
    SourceLocation OpLoc = consumeToken();
    Expr *RHS = parseFactor();
    if (!RHS)
      return nullptr;
    LHS = makeBinary(Ctx, *Op, OpLoc, LHS, RHS);
  }
  return LHS;
}

// designator = ident { "[" expression { "," expression } "]" | "." ident } .
Expr *Parser::parseDesignator() {
  Identifier Name;
  if (!expectIdentifier(Name, "variable name"))
    return nullptr;
  return parseSelectors(Ctx.create<NameRefExpr>(Name));
}

Expr *Parser::parseSelectors(Expr *Base) {
  while (true) {
    if (Tok.is(tok::l_square)) {
      SourceLocation LBrack = consumeToken();
      do {
        Expr *Idx = parseExpression();
        if (!Idx)
          return nullptr;
        Base = Ctx.create<IndexExpr>(SourceRange(Base->getBeginLoc(),
                                                 Idx->getRange().getEnd()),
                                     Base, Idx);
      } while (tryConsume(tok::comma));
      if (!expect(tok::r_square, "after array index")) {
        Diags.note(LBrack, "to match this '['");
        return nullptr;
      }
      continue;
    }
    if (Tok.is(tok::period)) {
      consumeToken();
      Identifier Field;
      if (!expectIdentifier(Field, "field name after '.'"))
        return nullptr;
      Base = Ctx.create<FieldExpr>(SourceRange(Base->getBeginLoc(), PrevTokEnd),
                                   Base, Field);
      continue;
    }
    if (Tok.is(tok::caret)) {
      consumeToken();
      Base = Ctx.create<DerefExpr>(SourceRange(Base->getBeginLoc(), PrevTokEnd),
                                   Base);
      continue;
    }
    // Turbo/FPC: a call through a procedural value, passes[i].run(v).
    // (A plain name followed by '(' is a CallExpr / ProcCallStmt.)
    if (Tok.is(tok::l_paren) && LangOpts.Mode != LangMode::ISO &&
        !llvm::isa<NameRefExpr>(Base)) {
      llvm::SmallVector<Expr *, 4> Args;
      if (!parseActualParams(Args, /*AllowFormat=*/false) && Args.empty())
        return nullptr;
      Base = Ctx.create<IndirectCallExpr>(
          SourceRange(Base->getBeginLoc(), PrevTokEnd), Base,
          Ctx.copyArray<Expr *>(Args));
      continue;
    }
    return Base;
  }
}

// factor = unsignedConst | designator | ident "(" args ")"
//        | "(" expression ")" | "not" factor .
Expr *Parser::parseFactor() {
  switch (Tok.getKind()) {
  case tok::integer_literal: {
    auto *E = Ctx.create<IntegerLiteralExpr>(Tok.getRange(),
                                             Tok.getIntegerValue());
    consumeToken();
    return E;
  }
  case tok::real_literal: {
    auto *E = Ctx.create<RealLiteralExpr>(Tok.getRange(), Tok.getRealValue());
    consumeToken();
    return E;
  }
  case tok::string_literal: {
    auto *E = Ctx.create<StringLiteralExpr>(Tok.getRange(),
                                            Tok.getStringValue());
    consumeToken();
    return E;
  }
  case tok::identifier: {
    Identifier Name = makeIdentifier(Tok);
    consumeToken();
    if (Tok.is(tok::l_paren)) {
      llvm::SmallVector<Expr *, 4> Args;
      if (!parseActualParams(Args, /*AllowFormat=*/false) && Args.empty())
        return nullptr;
      Expr *Call = Ctx.create<CallExpr>(SourceRange(Name.Loc, PrevTokEnd), Name,
                                        Ctx.copyArray<Expr *>(Args));
      // Turbo/FPC: a call (or typecast) can be dereferenced and selected
      // from: PNode(p)^.next, f(x)[1].
      if (LangOpts.Mode != LangMode::ISO)
        return parseSelectors(Call);
      return Call;
    }
    return parseSelectors(Ctx.create<NameRefExpr>(Name));
  }
  case tok::l_paren: {
    SourceLocation LParen = consumeToken();
    Expr *Inner = parseExpression();
    if (!Inner)
      return nullptr;
    if (!expect(tok::r_paren, "after expression")) {
      Diags.note(LParen, "to match this '('");
      return nullptr;
    }
    Expr *Paren = Ctx.create<ParenExpr>(SourceRange(LParen, PrevTokEnd), Inner);
    // objfpc: (obj as TFoo).Show
    if (LangOpts.ObjKeywords && Tok.isOneOf(tok::period, tok::caret, tok::l_square))
      return parseSelectors(Paren);
    return Paren;
  }
  case tok::kw_inherited:
    return parseInherited();
  case tok::kw_not: {
    SourceLocation OpLoc = consumeToken();
    Expr *Operand = parseFactor();
    if (!Operand)
      return nullptr;
    return Ctx.create<UnaryExpr>(
        SourceRange(OpLoc, Operand->getRange().getEnd()), UnaryOp::Not, Operand);
  }
  case tok::plus:
  case tok::minus: {
    // A sign inside a term (`a * -b`). ISO only allows a sign at the start of
    // a simple expression; Turbo/FPC accept it anywhere.
    if (LangOpts.Mode == LangMode::ISO)
      Diags.error(Tok.getLocation(),
                  "a sign is not allowed here in ISO Pascal; use parentheses",
                  {Tok.getRange()});
    UnaryOp Op = Tok.is(tok::plus) ? UnaryOp::Plus : UnaryOp::Minus;
    SourceLocation OpLoc = consumeToken();
    Expr *Operand = parseFactor();
    if (!Operand)
      return nullptr;
    return Ctx.create<UnaryExpr>(
        SourceRange(OpLoc, Operand->getRange().getEnd()), Op, Operand);
  }
  case tok::at: { // @x (Turbo/FPC)
    SourceLocation AtLoc = consumeToken();
    Expr *Operand = parseDesignator();
    if (!Operand)
      return nullptr;
    return Ctx.create<AddrOfExpr>(SourceRange(AtLoc, PrevTokEnd), Operand);
  }
  case tok::kw_nil: {
    auto *E = Ctx.create<NilExpr>(Tok.getRange());
    consumeToken();
    return E;
  }
  case tok::l_square: {
    // set constructor: "[" [ elem { "," elem } ] "]" ; elem = expr [".." expr]
    SourceLocation LBrack = consumeToken();
    llvm::SmallVector<SetElement, 8> Elems;
    if (Tok.isNot(tok::r_square)) {
      do {
        Expr *Lo = parseExpression();
        if (!Lo)
          return nullptr;
        Expr *Hi = nullptr;
        if (tryConsume(tok::dotdot) && !(Hi = parseExpression()))
          return nullptr;
        Elems.push_back({Lo, Hi});
      } while (tryConsume(tok::comma));
    }
    if (!expect(tok::r_square, "after set elements")) {
      Diags.note(LBrack, "to match this '['");
      return nullptr;
    }
    return Ctx.create<SetCtorExpr>(SourceRange(LBrack, PrevTokEnd),
                                   Ctx.copyArray<SetElement>(Elems));
  }
  default:
    Diags.error(Tok.getLocation(), "expected expression");
    return nullptr;
  }
}
