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
                     tok::kw_inherited, tok::kw_try, tok::kw_raise);
}

bool Parser::startsDeclarationPart() const {
  if (LangOpts.ObjKeywords &&
      Tok.isOneOf(tok::kw_constructor, tok::kw_destructor, tok::kw_class))
    return true; // method implementations (`class procedure TFoo.P`)
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
    case tok::kw_class: { // class procedure TFoo.P ...
      consumeToken();
      if (LangOpts.ObjKeywords && isIdentifierNamed("operator")) {
        if (RoutineDecl *R = parseOperatorDecl(InInterface))
          Decls.push_back(R);
        break;
      }
      if (Tok.isNot(tok::kw_procedure) && Tok.isNot(tok::kw_function)) {
        Diags.error(Tok.getLocation(),
                    "expected 'procedure' or 'function' after 'class'");
        skipUntil({tok::semi, tok::kw_begin});
        tryConsume(tok::semi);
        break;
      }
      if (RoutineDecl *R = parseRoutineDecl(InInterface)) {
        R->setClassMethod();
        Decls.push_back(R);
      }
      break;
    }
    default: { // label 1, 2, 3;
      if (LangOpts.ObjKeywords)
        diagNoGoto();
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
bool Parser::isVisibilityWord() const {
  return isIdentifierNamed("private") || isIdentifierNamed("protected") ||
         isIdentifierNamed("public") || isIdentifierNamed("published") ||
         isIdentifierNamed("strict");
}

void Parser::parseConstPart(llvm::SmallVectorImpl<Decl *> &Decls,
                            bool InMembers) {
  consumeToken(); // 'const'
  if (Tok.isNot(tok::identifier) || (InMembers && isVisibilityWord())) {
    Diags.error(Tok.getLocation(), "expected constant name after 'const'");
    return;
  }
  while (Tok.is(tok::identifier) && !(InMembers && isVisibilityWord()) &&
         !Diags.shouldStop()) {
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
        Hints H = parseHints();
        V->addHints(H.Flags, H.Message);
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
    Hints H = parseHints();
    if (Init) {
      auto *C = Ctx.create<ConstDecl>(Name.Loc, Name, Init);
      C->addHints(H.Flags, H.Message);
      Decls.push_back(C);
    } else
      skipUntil({tok::semi, tok::kw_begin});
    if (!expect(tok::semi, "after constant declaration")) {
      skipUntil({tok::semi, tok::kw_begin});
      tryConsume(tok::semi);
    }
  }
}

// typePart = "type" ident "=" type ";" { ident "=" type ";" } .
void Parser::parseTypePart(llvm::SmallVectorImpl<Decl *> &Decls,
                           bool InMembers) {
  consumeToken(); // 'type'
  if (Tok.isNot(tok::identifier) || (InMembers && isVisibilityWord())) {
    Diags.error(Tok.getLocation(), "expected type name after 'type'");
    return;
  }
  while (Tok.is(tok::identifier) && !(InMembers && isVisibilityWord()) &&
         !Diags.shouldStop()) {
    Identifier Name = makeIdentifier(Tok);
    consumeToken();
    TypeSpec *TS = nullptr;
    if (expect(tok::equal, "after type name"))
      TS = parseType();
    Hints H = parseHints();
    if (TS) {
      auto *TD = Ctx.create<TypeDecl>(Name.Loc, Name, TS);
      TD->addHints(H.Flags, H.Message);
      Decls.push_back(TD);
    } else
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

    // Turbo/FPC: an overlay  x: T absolute y;  (one name only). `absolute`
    // is not a reserved word.
    Expr *Absolute = nullptr;
    if (TS && Tok.is(tok::identifier) && Tok.getIdentifier() == "absolute") {
      SourceLocation AbsLoc = consumeToken();
      if (LangOpts.Mode == LangMode::ISO)
        Diags.error(AbsLoc, "'absolute' is a Turbo/Free Pascal extension; "
                            "use --mode=tp");
      else if (Names.size() > 1)
        Diags.error(AbsLoc, "'absolute' can only be used with one variable",
                    {SourceRange(Names[0].Loc, Names.back().getRange().getEnd())});
      Absolute = parseExpression();
      if (!Absolute)
        TS = nullptr;
    }

    // objfpc: an initialized variable  x: T = value;  (one name only)
    Expr *Init = nullptr;
    if (TS && Tok.is(tok::equal)) {
      SourceLocation EqLoc = consumeToken();
      if (!LangOpts.ObjKeywords)
        Diags.error(EqLoc, "initialized variables are a Free Pascal extension; "
                           "use --mode=objfpc");
      else if (Names.size() > 1)
        Diags.error(EqLoc, "only one variable can be initialized",
                    {SourceRange(Names[0].Loc, Names.back().getRange().getEnd())});
      Init = parseTypedConstValue();
      if (!Init)
        TS = nullptr;
      else if (!LangOpts.ObjKeywords || Names.size() > 1)
        Init = nullptr; // (reported)
    }
    Hints H = parseHints();

    if (!TS) {
      skipUntil({tok::semi, tok::kw_begin});
    } else {
      for (const Identifier &N : Names) {
        auto *V = Ctx.create<VarDecl>(N, TS);
        if (Init)
          V->setInitializedVar(Init);
        V->setAbsolute(Absolute);
        V->addHints(H.Flags, H.Message);
        Decls.push_back(V);
      }
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
  llvm::ArrayRef<Identifier> OuterNames;
  if (!expectIdentifier(Name, std::string(Kind) + " name")) {
    skipUntil({tok::semi, tok::kw_begin});
    tryConsume(tok::semi);
  } else if (LangOpts.ObjKeywords && Tok.is(tok::period)) {
    // TFoo.Bar, or TOuter.TInner.Bar (a nested class or record)
    llvm::SmallVector<Identifier, 2> Outer;
    while (Tok.is(tok::period)) {
      consumeToken();
      if (ClassName.isValid())
        Outer.push_back(ClassName);
      ClassName = Name;
      if (!expectIdentifier(Name, "method name after '.'")) {
        Name = Identifier();
        skipUntil({tok::semi, tok::kw_begin});
        tryConsume(tok::semi);
        break;
      }
    }
    OuterNames = Ctx.copyArray<Identifier>(Outer);
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

  // Turbo/FPC directives:  cdecl;  stdcall;  varargs;
  // external [library] [name symbol];  (library, symbol: string constants)
  // objfpc: overload;
  bool IsExternal = false, Overload = false, Inline = false, VarArgs = false;
  CallConv CC = CallConv::Default;
  SourceLocation InlineLoc, VarArgsLoc;
  llvm::StringRef ExternalName, LibraryName;
  Expr *LibraryExpr = nullptr, *NameExpr = nullptr;
  Hints H;
  while (LangOpts.Mode != LangMode::ISO) {
    if (parseSeparatedHint(H))
      continue;
    if (Tok.is(tok::kw_inline)) {
      InlineLoc = consumeToken();
      Inline = parseInlineDirective(InlineLoc);
      continue;
    }
    if (CallConv C = callConvOf(Tok); C != CallConv::Default) {
      takeCallConv(CC);
      expect(tok::semi, std::string("after '") + callConvName(C) + "'");
      continue;
    }
    if (isIdentifierNamed("varargs")) {
      VarArgsLoc = consumeToken();
      VarArgs = true;
      expect(tok::semi, "after 'varargs'");
      continue;
    }
    if (LangOpts.ObjKeywords && isIdentifierNamed("overload")) {
      consumeToken();
      Overload = true;
      expect(tok::semi, "after 'overload'");
      continue;
    }
    if (!isIdentifierNamed("external"))
      break;
    consumeToken();
    IsExternal = true;
    // The library (linked with the program) and the symbol: string
    // constants (literals here; Sema evaluates the others).
    auto StringConst = [&](llvm::StringRef &Out) {
      Expr *E = parseExpression();
      if (auto *SL = llvm::dyn_cast_or_null<StringLiteralExpr>(E))
        Out = SL->getValue();
      return E;
    };
    if (Tok.isNot(tok::semi) && !isIdentifierNamed("name"))
      LibraryExpr = StringConst(LibraryName);
    if (isIdentifierNamed("name")) {
      consumeToken();
      if (Tok.is(tok::semi))
        Diags.error(Tok.getLocation(), "expected the external symbol name");
      else
        NameExpr = StringConst(ExternalName);
    }
    expect(tok::semi, "after 'external'");
  }
  if (VarArgs && !IsExternal)
    Diags.error(VarArgsLoc, "'varargs' is only allowed on external routines");
  if (IsExternal) {
    if (InlineLoc.isValid())
      Diags.error(InlineLoc, "'inline' cannot be used with 'external'");
    if (!Name.isValid())
      return nullptr;
    auto *R = Ctx.create<RoutineDecl>(Loc, Name, IsFunction,
                                      Ctx.copyArray<VarDecl *>(Params),
                                      HasParamList, ResultTS, nullptr, false);
    R->setExternal(ExternalName);
    R->setLibraryName(LibraryName);
    R->setExternalExprs(LibraryExpr, NameExpr);
    if (VarArgs)
      R->setVarArgs();
    if (Overload)
      R->setOverloadDirective();
    R->setCallConv(CC);
    R->addHints(H.Flags, H.Message);
    return R;
  }

  if (HeadingOnly && ClassName.isValid()) {
    Diags.error(ClassName.Loc,
                "methods are implemented in the implementation part of a unit",
                {SourceRange(ClassName.Loc, Name.getRange().getEnd())});
    return nullptr;
  }
  auto WithDirectives = [&](RoutineDecl *R) {
    if (R && Overload)
      R->setOverloadDirective();
    if (R && Inline)
      R->setInline();
    if (R) {
      R->setCallConv(CC);
      R->addHints(H.Flags, H.Message);
    }
    return R;
  };
  if (HeadingOnly) // interface part of a unit: the body comes later
    return Name.isValid()
               ? WithDirectives(Ctx.create<RoutineDecl>(
                     Loc, Name, IsFunction, Ctx.copyArray<VarDecl *>(Params),
                     HasParamList, ResultTS, nullptr, true))
               : nullptr;

  if (isIdentifierNamed("forward")) {
    SourceLocation ForwardLoc = consumeToken();
    if (InlineLoc.isValid())
      Diags.error(ForwardLoc, "'forward' cannot be used with 'inline'");
    expect(tok::semi, "after 'forward'");
    if (!Name.isValid())
      return nullptr;
    return WithDirectives(Ctx.create<RoutineDecl>(
        Loc, Name, IsFunction, Ctx.copyArray<VarDecl *>(Params), HasParamList,
        ResultTS, nullptr, true));
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
  R->setOuterClassNames(OuterNames);
  return WithDirectives(R);
}

// methodHeading = ("procedure" | "function" | "constructor" | "destructor")
//                 ident [params] [":" typeIdent] ";" { directive ";" } .
// directive     = "virtual" | "override" | "abstract" | "overload"
//               | "reintroduce" .
RoutineDecl *Parser::parseMethodHeading(bool InRecord, bool RecordClassMethod) {
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

  bool Virtual = false, Override = false, Abstract = false, Overload = false;
  bool Static = false, Inline = false;
  CallConv CC = CallConv::Default;
  SourceLocation InlineLoc;
  Hints H;
  while (Tok.is(tok::identifier) || Tok.is(tok::kw_inline)) {
    if (parseSeparatedHint(H))
      continue;
    if (Tok.is(tok::kw_inline)) {
      InlineLoc = consumeToken();
      Inline = parseInlineDirective(InlineLoc);
      continue;
    }
    if (CallConv C = callConvOf(Tok); C != CallConv::Default) {
      takeCallConv(CC);
      if (!expect(tok::semi, std::string("after '") + callConvName(C) + "'"))
        return nullptr;
      continue;
    }
    llvm::StringRef W = Tok.getIdentifier();
    if (W == "static" && InRecord) {
      Static = true;
    } else if (W == "virtual") {
      Virtual = true;
    } else if (W == "override") {
      Override = true;
    } else if (W == "abstract") {
      Abstract = true;
    } else if (W == "overload") {
      Overload = true;
    } else if (W == "reintroduce") {
      // (hides an inherited virtual method without a warning: no effect)
    } else if (W == "dynamic" ||
               W == "static" || W == "message" || W == "final" ||
               W == "external" || W == "forward") {
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
  if (InlineLoc.isValid() && (Virtual || Override || Abstract))
    Diags.error(InlineLoc, std::string("'inline' cannot be used with '") +
                               (Virtual    ? "virtual"
                                : Override ? "override"
                                           : "abstract") +
                               "'");
  if (RecordClassMethod && !Static)
    Diags.error(Name.Loc, "the class methods of a record must be 'static'",
                {Name.getRange()});
  else if (Static && !RecordClassMethod)
    Diags.error(Name.Loc,
                "only the class methods of a record can be 'static'",
                {Name.getRange()});
  auto *R = Ctx.create<RoutineDecl>(Loc, Name, IsFunction,
                                    Ctx.copyArray<VarDecl *>(Params),
                                    HasParamList, ResultTS, nullptr, false);
  R->setMethodKind(MK);
  R->setDirectives(Virtual, Override, Abstract);
  if (Overload)
    R->setOverloadDirective();
  if (Inline && !Virtual && !Override && !Abstract)
    R->setInline();
  R->setCallConv(CC);
  R->addHints(H.Flags, H.Message);
  return R;
}

CallConv Parser::callConvOf(const Token &T) const {
  if (LangOpts.Mode == LangMode::ISO || T.isNot(tok::identifier))
    return CallConv::Default;
  llvm::StringRef W = T.getIdentifier();
  return W == "cdecl"     ? CallConv::CDecl
         : W == "stdcall" ? CallConv::StdCall
                          : CallConv::Default;
}

void Parser::takeCallConv(CallConv &CC) {
  CallConv New = callConvOf(Tok);
  SourceLocation Loc = Tok.getLocation();
  SourceRange Range(Loc, Loc.getLocWithOffset(
                             static_cast<int>(Tok.getIdentifier().size())));
  consumeToken();
  if (CC == New)
    Diags.warning(Loc, llvm::Twine("calling convention '") +
                           callConvName(New) + "' is given twice",
                  {Range});
  else if (CC != CallConv::Default)
    Diags.warning(Loc, llvm::Twine("calling convention '") +
                           callConvName(New) + "' replaces '" +
                           callConvName(CC) + "'",
                  {Range});
  CC = New;
}

unsigned char Parser::hintFlagOf(const Token &T) const {
  if (LangOpts.Mode == LangMode::ISO || T.isNot(tok::identifier))
    return 0;
  llvm::StringRef W = T.getIdentifier();
  return W == "deprecated"      ? Decl::HintDeprecated
         : W == "experimental"  ? Decl::HintExperimental
         : W == "platform"      ? Decl::HintPlatform
         : W == "library"       ? Decl::HintLibrary
         : W == "unimplemented" ? Decl::HintUnimplemented
                                : 0;
}

// hints = { "deprecated" [ string ] | "experimental" | "platform"
//         | "library" | "unimplemented" } .   (Turbo/FPC)
Parser::Hints Parser::parseHints() {
  Hints H;
  while (unsigned char F = hintFlagOf(Tok)) {
    consumeToken();
    H.Flags |= F;
    if (F == Decl::HintDeprecated && Tok.is(tok::string_literal)) {
      H.Message = Tok.getStringValue();
      consumeToken();
    }
  }
  return H;
}

// A hint directive after a routine's or property's `;`:  deprecated 'm';
bool Parser::parseSeparatedHint(Hints &H) {
  unsigned char F = hintFlagOf(Tok);
  if (!F)
    return false;
  std::string Word = Tok.getIdentifier().str();
  consumeToken();
  H.Flags |= F;
  if (F == Decl::HintDeprecated && Tok.is(tok::string_literal)) {
    H.Message = Tok.getStringValue();
    consumeToken();
  }
  expect(tok::semi, "after '" + Word + "'");
  return true;
}

// The `inline` directive (after the keyword at Loc): whether the routine is
// to be inlined. It needs {$inline on}, the default in objfpc mode (as in
// Free Pascal); with {$inline off} there it is accepted and ignored.
bool Parser::parseInlineDirective(SourceLocation Loc) {
  expect(tok::semi, "after 'inline'");
  if (Lex.inlineOn())
    return true;
  if (!LangOpts.ObjKeywords)
    Diags.error(Loc, "the 'inline' directive needs {$inline on}");
  return false;
}

// operatorName = "+" | "-" | "*" | "/" | "=" | "<>" | "<" | ">" | "<=" | ">="
//              | ":=" | "div" | "mod" | "and" | "or" | "xor" | "shl" | "shr"
//              | "in" | "not" | "explicit" .
// The name of the operator's routine: "operator" followed by the symbol
// (lower case), spelled "operator <symbol>".
bool Parser::parseOperatorName(Identifier &Name) {
  static const struct {
    tok::TokenKind K;
    const char *Sym;
  } Ops[] = {{tok::plus, "+"},          {tok::minus, "-"},
             {tok::star, "*"},          {tok::slash, "/"},
             {tok::equal, "="},         {tok::not_equal, "<>"},
             {tok::less, "<"},          {tok::greater, ">"},
             {tok::less_equal, "<="},   {tok::greater_equal, ">="},
             {tok::colon_equal, ":="},  {tok::kw_div, "div"},
             {tok::kw_mod, "mod"},      {tok::kw_and, "and"},
             {tok::kw_or, "or"},        {tok::kw_xor, "xor"},
             {tok::kw_shl, "shl"},      {tok::kw_shr, "shr"},
             {tok::kw_in, "in"},        {tok::kw_not, "not"}};
  std::string Sym;
  for (const auto &O : Ops)
    if (Tok.is(O.K))
      Sym = O.Sym;
  if (Sym.empty() && isIdentifierNamed("explicit"))
    Sym = "explicit";
  if (Sym.empty()) {
    Diags.error(Tok.getLocation(),
                "expected an operator (+, -, *, /, =, <>, <, >, <=, >=, :=, "
                "div, mod, and, or, xor, shl, shr, in, not or Explicit)");
    return false;
  }
  StringPool &S = Ctx.getStrings();
  Name = {S.intern("operator" + Sym),
          S.intern("operator " + Tok.getRawText().str()), Tok.getLocation()};
  consumeToken();
  return true;
}

// operatorHeading = "operator" operatorName "(" params ")" ":" typeIdent ";"
//                   [ "overload" ";" ] .      (after "class", in a record)
RoutineDecl *Parser::parseOperatorHeading() {
  SourceLocation Loc = consumeToken(); // 'operator'
  Identifier Name;
  if (!parseOperatorName(Name))
    return nullptr;
  llvm::SmallVector<VarDecl *, 2> Params;
  if (Tok.isNot(tok::l_paren)) {
    Diags.error(Tok.getLocation(), "expected '(' after the operator");
    return nullptr;
  }
  if (!parseFormalParams(Params) ||
      !expect(tok::colon, "after the operator's parameters"))
    return nullptr;
  TypeSpec *ResultTS = parseTypeIdentifier("operator result type");
  if (!ResultTS || !expect(tok::semi, "after operator heading"))
    return nullptr;
  bool Overload = false, Inline = false;
  while (true) {
    if (isIdentifierNamed("overload")) {
      consumeToken();
      Overload = true;
      if (!expect(tok::semi, "after 'overload'"))
        return nullptr;
      continue;
    }
    if (Tok.is(tok::kw_inline)) {
      Inline = parseInlineDirective(consumeToken());
      continue;
    }
    break;
  }
  auto *R = Ctx.create<RoutineDecl>(Loc, Name, /*IsFunction=*/true,
                                    Ctx.copyArray<VarDecl *>(Params), true,
                                    ResultTS, nullptr, false);
  R->setMethodKind(MethodKind::Operator);
  R->setClassMethod();
  if (Overload)
    R->setOverloadDirective();
  if (Inline)
    R->setInline();
  return R;
}

// operatorDecl = "operator" ident "." operatorName "(" params ")" ":"
//                typeIdent ";" block ";" .        (after "class")
RoutineDecl *Parser::parseOperatorDecl(bool HeadingOnly) {
  SourceLocation Loc = consumeToken(); // 'operator'
  Identifier ClassName, Name;
  llvm::SmallVector<VarDecl *, 2> Params;
  TypeSpec *ResultTS = nullptr;
  if (!expectIdentifier(ClassName, "record name after 'operator'") ||
      !expect(tok::period, "after the record name") ||
      !parseOperatorName(Name) ||
      (Tok.is(tok::l_paren) ? !parseFormalParams(Params)
                            : (Diags.error(Tok.getLocation(),
                                           "expected '(' after the operator"),
                               true)) ||
      !expect(tok::colon, "after the operator's parameters") ||
      !(ResultTS = parseTypeIdentifier("operator result type")) ||
      !expect(tok::semi, "after operator heading")) {
    skipUntil({tok::semi, tok::kw_begin});
    tryConsume(tok::semi);
    return nullptr;
  }
  if (HeadingOnly) {
    Diags.error(ClassName.Loc,
                "methods are implemented in the implementation part of a unit",
                {SourceRange(ClassName.Loc, Name.getRange().getEnd())});
    return nullptr;
  }
  Block *Body = parseBlock();
  if (!expect(tok::semi, "after operator body")) {
    skipUntil({tok::semi, tok::kw_begin, tok::kw_procedure, tok::kw_function});
    tryConsume(tok::semi);
  }
  auto *R = Ctx.create<RoutineDecl>(Loc, Name, /*IsFunction=*/true,
                                    Ctx.copyArray<VarDecl *>(Params), true,
                                    ResultTS, Body, false);
  R->setMethodKind(MethodKind::Operator);
  R->setClassName(ClassName);
  R->setClassMethod();
  return R;
}

// property = "property" ident [ "[" params "]" ] ":" typeIdent
//            [ "index" constant ] [ "read" ident ] [ "write" ident ]
//            [ "default" constant | "nodefault" ] ";" [ "default" ";" ] .
PropertyDecl *Parser::parsePropertyDecl() {
  consumeToken(); // 'property'
  Identifier Name;
  if (!expectIdentifier(Name, "property name"))
    return nullptr;
  llvm::SmallVector<VarDecl *, 2> Params;
  if (Tok.is(tok::l_square)) {
    if (!parseFormalParams(Params, tok::r_square) || Params.empty())
      return nullptr; // (an empty list was reported)
  }
  if (!expect(tok::colon, "after property name"))
    return nullptr;
  TypeSpec *TS = parseTypeIdentifier("property type");
  if (!TS)
    return nullptr;
  Identifier Read, Write;
  Expr *Index = nullptr, *DefaultValue = nullptr;
  bool NoDefault = false;
  while (Tok.is(tok::identifier)) {
    llvm::StringRef W = Tok.getIdentifier();
    if (W == "read" || W == "write") {
      consumeToken();
      if (!expectIdentifier(W == "read" ? Read : Write,
                            "field or method name after '" + W.str() + "'"))
        return nullptr;
      continue;
    }
    if (W == "index" || W == "default") {
      bool IsIndex = W == "index";
      consumeToken();
      Expr *E = parseExpression();
      if (!E)
        return nullptr;
      (IsIndex ? Index : DefaultValue) = E;
      continue;
    }
    if (W == "nodefault") {
      consumeToken();
      NoDefault = true;
      continue;
    }
    if (W == "stored" || W == "implements") {
      unsupported("'" + W.str() + "' in a property is");
      return nullptr;
    }
    break;
  }
  if (!expect(tok::semi, "after property declaration"))
    return nullptr;
  auto *P = Ctx.create<PropertyDecl>(Name, TS, Read, Write);
  P->setParams(Ctx.copyArray<VarDecl *>(Params));
  P->setIndexSpec(Index);
  P->setDefaultValue(DefaultValue);
  if (NoDefault)
    P->setNoDefault();
  // property Items[i: integer]: T read GetItem; default;
  if (Tok.is(tok::identifier) && Tok.getIdentifier() == "default") {
    consumeToken();
    P->setDefault();
    if (!expect(tok::semi, "after 'default'"))
      return nullptr;
  }
  return P;
}

// classType = "class" ";"                                  (forward)
//           | "class" "of" ident                           (class reference)
//           | "class" [ "(" ident ")" ] ( ";"? | { member } "end" ) .
// member    = visibility | ["class"] "var" | identList ":" type ";"
//           | ["class"] methodHeading | property .
// A `class var` section declares class variables up to the next visibility,
// `var`, method or property.
TypeSpec *Parser::parseClassType(SourceLocation Start) {
  SourceLocation ClassLoc = consumeToken(); // 'class'
  if (Tok.is(tok::semi)) // TFoo = class;
    return Ctx.create<ClassTypeSpec>(SourceRange(Start, PrevTokEnd),
                                     Identifier(), llvm::ArrayRef<Decl *>(),
                                     /*Forward=*/true);
  if (tryConsume(tok::kw_of)) { // class of TFoo
    Identifier Name;
    if (!expectIdentifier(Name, "class name after 'class of'"))
      return nullptr;
    return Ctx.create<ClassRefTypeSpec>(SourceRange(Start, PrevTokEnd), Name);
  }
  Identifier Parent;
  llvm::SmallVector<Identifier, 2> Interfaces; // class(TParent, I1, I2)
  auto Spec = [&](llvm::ArrayRef<Decl *> Members) {
    auto *CS = Ctx.create<ClassTypeSpec>(SourceRange(Start, PrevTokEnd), Parent,
                                         Members, false);
    CS->setInterfaces(Ctx.copyArray<Identifier>(Interfaces));
    return CS;
  };
  if (Tok.is(tok::l_paren)) {
    SourceLocation LParen = consumeToken();
    if (!expectIdentifier(Parent, "parent class name"))
      return nullptr;
    while (tryConsume(tok::comma)) {
      Identifier I;
      if (!expectIdentifier(I, "interface name"))
        return nullptr;
      Interfaces.push_back(I);
    }
    if (!expect(tok::r_paren, "after parent class name")) {
      Diags.note(LParen, "to match this '('");
      return nullptr;
    }
    if (Tok.is(tok::semi)) // TFoo = class(TBar);
      return Spec({});
  }

  llvm::SmallVector<Decl *, 16> Members;
  Visibility Vis = Visibility::Public;
  bool ClassVars = false; // in a `class var` section
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
        ClassVars = false;
        consumeToken();
        continue;
      }
      if (W == "strict") { // strict private / strict protected
        consumeToken();
        llvm::StringRef Next = Tok.is(tok::identifier) ? Tok.getIdentifier() : "";
        if (Next != "private" && Next != "protected") {
          Diags.error(Tok.getLocation(),
                      "expected 'private' or 'protected' after 'strict'");
          return Fail();
        }
        Vis = Next == "private" ? Visibility::StrictPrivate
                                : Visibility::StrictProtected;
        ClassVars = false;
        consumeToken();
        continue;
      }
      llvm::SmallVector<Identifier, 4> Names;
      if (!parseIdentList(Names, "field name") ||
          !expect(tok::colon, "after field name"))
        return Fail();
      TypeSpec *TS = parseType();
      if (!TS)
        return Fail();
      Hints H = parseHints();
      for (const Identifier &N : Names) {
        if (ClassVars) {
          auto *V = Ctx.create<VarDecl>(N, TS);
          V->setClassVar();
          V->setVisibility(Vis);
          V->addHints(H.Flags, H.Message);
          Members.push_back(V);
          continue;
        }
        auto *F = Ctx.create<FieldDecl>(N, TS);
        F->setVisibility(Vis);
        F->addHints(H.Flags, H.Message);
        Members.push_back(F);
      }
      if (!expect(tok::semi, "after field declaration"))
        return Fail();
      continue;
    }
    if (tryConsume(tok::kw_var)) { // instance fields again
      ClassVars = false;
      continue;
    }
    if (Tok.isOneOf(tok::kw_procedure, tok::kw_function, tok::kw_constructor,
                    tok::kw_destructor)) {
      RoutineDecl *R = parseMethodHeading();
      if (!R)
        return Fail();
      R->setVisibility(Vis);
      Members.push_back(R);
      ClassVars = false;
      continue;
    }
    if (Tok.is(tok::kw_property)) {
      PropertyDecl *P = parsePropertyDecl();
      if (!P)
        return Fail();
      Hints H;
      while (parseSeparatedHint(H)) {
      }
      P->addHints(H.Flags, H.Message);
      P->setVisibility(Vis);
      Members.push_back(P);
      ClassVars = false;
      continue;
    }
    if (Tok.is(tok::kw_class)) {
      consumeToken();
      if (tryConsume(tok::kw_var)) { // class var X: T; ...
        ClassVars = true;
        continue;
      }
      if (Tok.isOneOf(tok::kw_procedure, tok::kw_function)) {
        RoutineDecl *R = parseMethodHeading();
        if (!R)
          return Fail();
        R->setClassMethod();
        R->setVisibility(Vis);
        Members.push_back(R);
        ClassVars = false;
        continue;
      }
      if (Tok.isOneOf(tok::kw_constructor, tok::kw_destructor, tok::kw_property)) {
        unsupported("class " + Tok.getRawText().lower() + "s are");
        return Fail();
      }
      if (isIdentifierNamed("operator")) {
        Diags.error(Tok.getLocation(),
                    "class operators can only be declared in records");
        return Fail();
      }
      Diags.error(Tok.getLocation(),
                  "expected 'var', 'procedure' or 'function' after 'class'");
      return Fail();
    }
    if (Tok.isOneOf(tok::kw_const, tok::kw_type)) { // nested declarations
      llvm::SmallVector<Decl *, 4> Nested;
      if (Tok.is(tok::kw_const))
        parseConstPart(Nested, /*InMembers=*/true);
      else
        parseTypePart(Nested, /*InMembers=*/true);
      for (Decl *D : Nested) {
        D->setVisibility(Vis);
        if (auto *V = llvm::dyn_cast<VarDecl>(D)) // a typed constant
          V->setClassVar();
        Members.push_back(D);
      }
      ClassVars = false;
      continue;
    }
    Diags.error(Tok.getLocation(),
                "expected a field, method or property declaration, or 'end'");
    return Fail();
  }
  if (!expect(tok::kw_end, "at end of class type")) {
    Diags.note(ClassLoc, "to match this 'class'");
    return nullptr;
  }
  return Spec(Ctx.copyArray<Decl *>(Members));
}

// interfaceType = "interface" ";"                          (forward)
//               | "interface" [ "(" ident ")" ] [ "[" string "]" ]
//                 { methodHeading } "end" .               (objfpc)
// COM or CORBA as set by the last {$interfaces ...} before it.
TypeSpec *Parser::parseInterfaceType(SourceLocation Start) {
  bool Corba = Lex.corbaInterfaces();
  SourceLocation IntfLoc = consumeToken(); // 'interface'
  if (Tok.is(tok::semi)) // IFoo = interface;
    return Ctx.create<InterfaceTypeSpec>(SourceRange(Start, PrevTokEnd),
                                         Identifier(), llvm::ArrayRef<Decl *>(),
                                         /*Forward=*/true, Corba, "");
  Identifier Parent;
  if (Tok.is(tok::l_paren)) {
    SourceLocation LParen = consumeToken();
    if (!expectIdentifier(Parent, "parent interface name"))
      return nullptr;
    if (!expect(tok::r_paren, "after parent interface name")) {
      Diags.note(LParen, "to match this '('");
      return nullptr;
    }
  }
  llvm::StringRef GUID;
  if (tryConsume(tok::l_square)) { // ['{...}']
    if (Tok.isNot(tok::string_literal)) {
      Diags.error(Tok.getLocation(), "expected the interface's GUID string");
      return nullptr;
    }
    GUID = Tok.getStringValue();
    consumeToken();
    if (!expect(tok::r_square, "after the GUID"))
      return nullptr;
  }
  llvm::SmallVector<Decl *, 8> Members;
  while (Tok.isNot(tok::kw_end) && Tok.isNot(tok::eof) && !Diags.shouldStop()) {
    if (Tok.isOneOf(tok::kw_procedure, tok::kw_function)) {
      RoutineDecl *R = parseMethodHeading();
      if (!R) {
        skipUntil({tok::kw_end});
        tryConsume(tok::kw_end);
        return nullptr;
      }
      Members.push_back(R);
      continue;
    }
    if (Tok.is(tok::kw_property))
      unsupported("properties in interfaces are");
    else
      Diags.error(Tok.getLocation(),
                  "expected a method heading or 'end' in an interface");
    skipUntil({tok::kw_end});
    tryConsume(tok::kw_end);
    return nullptr;
  }
  if (!expect(tok::kw_end, "at end of interface type")) {
    Diags.note(IntfLoc, "to match this 'interface'");
    return nullptr;
  }
  return Ctx.create<InterfaceTypeSpec>(SourceRange(Start, PrevTokEnd), Parent,
                                       Ctx.copyArray<Decl *>(Members), false,
                                       Corba, GUID);
}

// params = "(" section { ";" section } ")" ;
// section = ["var" | "const"] identList ":" typeIdent .   ("const": TP)
bool Parser::parseFormalParams(llvm::SmallVectorImpl<VarDecl *> &Params,
                               tok::TokenKind Close) {
  SourceLocation LParen = consumeToken(); // '(' or '['
  bool SeenDefault = false;
  // Once a parameter has a default value, the following ones need one.
  auto NeedsDefault = [&](const Identifier &N) {
    if (SeenDefault)
      Diags.error(N.Loc,
                  "parameter '" + N.Spelling +
                      "' needs a default value, as a parameter before it has one",
                  {N.getRange()});
  };
  do {
    if (Tok.isOneOf(tok::kw_procedure, tok::kw_function)) {
      // procedure p(params) / function f(params): T
      bool IsFunction = Tok.is(tok::kw_function);
      SourceLocation Start = consumeToken();
      Identifier Name;
      if (!expectIdentifier(Name, "parameter name")) {
        skipUntil({tok::semi, Close});
        continue;
      }
      llvm::SmallVector<VarDecl *, 4> Inner;
      if (Tok.is(tok::l_paren) && !parseFormalParams(Inner)) {
        skipUntil({tok::semi, Close});
        continue;
      }
      TypeSpec *Result = nullptr;
      if (IsFunction) {
        if (!expect(tok::colon, "before the result type of a functional parameter") ||
            !(Result = parseTypeIdentifier("function result type"))) {
          skipUntil({tok::semi, Close});
          continue;
        }
      }
      auto *TS = Ctx.create<RoutineTypeSpec>(SourceRange(Start, PrevTokEnd),
                                             IsFunction,
                                             Ctx.copyArray<VarDecl *>(Inner),
                                             Result);
      NeedsDefault(Name);
      Params.push_back(Ctx.create<VarDecl>(Name, TS, VarDecl::ValueParam));
      continue;
    }
    bool IsVar = tryConsume(tok::kw_var);
    bool IsConst = false;
    if (!IsVar && Tok.is(tok::kw_const) && LangOpts.Mode != LangMode::ISO) {
      consumeToken();
      IsConst = true;
    }
    // objfpc: `out`, not a reserved word, is a var parameter here.
    bool IsOut = !IsVar && !IsConst && LangOpts.ObjKeywords &&
                 Close == tok::r_paren && Tok.is(tok::identifier) &&
                 Tok.getIdentifier() == "out";
    if (IsOut) {
      consumeToken();
      IsVar = true;
    }
    llvm::SmallVector<Identifier, 4> Names;
    if (!parseIdentList(Names, "parameter name")) {
      skipUntil({tok::semi, Close});
      continue;
    }
    TypeSpec *TS;
    bool Untyped = false;
    // Turbo/FPC untyped parameters:  var x;  const a, b
    if ((IsVar || IsConst) && LangOpts.Mode != LangMode::ISO &&
        Tok.isNot(tok::colon)) {
      Untyped = true;
      TS = Ctx.create<UntypedTypeSpec>(
          SourceRange(Names.back().Loc, Names.back().getRange().getEnd()));
    } else if (!expect(tok::colon, "after parameter name")) {
      skipUntil({tok::semi, Close});
      continue;
    } else
      TS = parseTypeIdentifier("parameter type");
    if (!TS) {
      skipUntil({tok::semi, Close});
      continue;
    }
    // objfpc: a default value  b: T = value  (one name only)
    Expr *Default = nullptr;
    bool DefaultOK = false;
    if (Tok.is(tok::equal) && Close == tok::r_paren) {
      SourceLocation EqLoc = consumeToken();
      if (!LangOpts.ObjKeywords)
        Diags.error(EqLoc, "default parameter values are a Free Pascal "
                           "extension; use --mode=objfpc");
      else if (IsVar || Untyped)
        Diags.error(EqLoc, "only value and const parameters can have a default value");
      else if (Names.size() > 1)
        Diags.error(EqLoc, "only one parameter can have a default value",
                    {SourceRange(Names[0].Loc, Names.back().getRange().getEnd())});
      else
        DefaultOK = true;
      Default = parseExpression();
      if (!Default) {
        skipUntil({tok::semi, Close});
        continue;
      }
    }
    for (const Identifier &N : Names) {
      if (!Default)
        NeedsDefault(N);
      auto *V = Ctx.create<VarDecl>(
          N, TS,
          IsVar ? VarDecl::VarParam
                : IsConst ? VarDecl::ConstParam : VarDecl::ValueParam);
      if (DefaultOK)
        V->setDefault(Default);
      if (IsOut)
        V->setOutParam();
      Params.push_back(V);
    }
    SeenDefault |= DefaultOK;
  } while (tryConsume(tok::semi));

  if (!expect(Close, "after parameters")) {
    Diags.note(LParen, Close == tok::r_paren ? "to match this '('"
                                             : "to match this '['");
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
  case tok::kw_interface: // objfpc: an interface type
    if (Packed || !LangOpts.ObjKeywords)
      break;
    return parseInterfaceType(Start);
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
    return Ctx.create<StringTypeSpec>(SourceRange(Start, PrevTokEnd), MaxLen,
                                      !MaxLen && Lex.longStrings());
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
    auto *RS = Ctx.create<RoutineTypeSpec>(SourceRange(Start, PrevTokEnd),
                                           IsFunction,
                                           Ctx.copyArray<VarDecl *>(Params),
                                           Result);
    if (LangOpts.ObjKeywords && Tok.is(tok::kw_of)) { // a method pointer
      consumeToken();
      if (!expect(tok::kw_object, "after 'of'"))
        return nullptr;
      RS->setOfObject();
    }
    // A calling convention:  procedure(x: integer) cdecl  or, after a
    // semicolon,  procedure(x: integer); cdecl  (Turbo/FPC).
    CallConv CC = CallConv::Default;
    while (true) {
      if (callConvOf(Tok) != CallConv::Default)
        takeCallConv(CC);
      else if (Tok.is(tok::semi) &&
               callConvOf(peekToken()) != CallConv::Default) {
        consumeToken();
        takeCallConv(CC);
      } else {
        break;
      }
    }
    RS->setCallConv(CC);
    return RS;
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
    auto *TS = Ctx.create<StringTypeSpec>(Tok.getRange(), nullptr,
                                          Lex.longStrings());
    consumeToken();
    return TS;
  }
  if (Tok.is(tok::identifier))
    return parseNamedType();
  if (Tok.is(tok::kw_array) && Context == "parameter type" && LangOpts.Mode != LangMode::ISO &&
      peekToken().is(tok::kw_of)) { // Turbo/FPC: array of const, array of T
    SourceLocation Start = Tok.getLocation();
    consumeToken(); // array
    consumeToken(); // of
    if (Tok.is(tok::kw_const)) {
      consumeToken();
      return Ctx.create<ArrayOfConstTypeSpec>(SourceRange(Start, PrevTokEnd));
    }
    TypeSpec *Element = parseTypeIdentifier("the elements of an open array");
    if (!Element)
      return nullptr;
    return Ctx.create<OpenArrayTypeSpec>(SourceRange(Start, PrevTokEnd), Element);
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
static bool continuesConstExpr(tok::TokenKind K);

TypeSpec *Parser::parseNamedType() {
  Identifier Name = makeIdentifier(Tok);
  SourceLocation Start = consumeToken();
  if (!LangOpts.ObjKeywords || Tok.isNot(tok::period) ||
      peekToken().isNot(tok::identifier))
    return Ctx.create<NamedTypeSpec>(Name);
  llvm::SmallVector<Identifier, 2> Qualifiers;
  while (Tok.is(tok::period) && peekToken().is(tok::identifier)) {
    consumeToken(); // '.'
    Qualifiers.push_back(Name);
    Name = makeIdentifier(Tok);
    consumeToken();
  }
  if (continuesConstExpr(Tok.getKind()))
    Diags.error(Tok.getLocation(),
                "a subrange bound cannot be a qualified constant yet; declare "
                "an unqualified constant for it");
  return Ctx.create<NamedTypeSpec>(SourceRange(Start, PrevTokEnd),
                                   Ctx.copyArray<Identifier>(Qualifiers), Name);
}

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

  if (Tok.is(tok::identifier) && !continuesConstExpr(peekToken().getKind()))
    return parseNamedType();

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
  if (Tok.is(tok::kw_of) && LangOpts.Mode != LangMode::ISO) { // Turbo/FPC
    consumeToken();
    TypeSpec *Elem = parseType();
    if (!Elem)
      return nullptr;
    return Ctx.create<DynArrayTypeSpec>(SourceRange(Start, PrevTokEnd), Elem);
  }
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
  if (LangOpts.ObjKeywords && Lex.advancedRecords())
    return parseAdvancedRecord(Start, RecordLoc, Packed);
  FieldList *Fields = parseFieldList(tok::kw_end);
  if (!Fields) {
    if (RecordBodySkipped) // (methods without the mode switch)
      tryConsume(tok::kw_end);
    RecordBodySkipped = false;
    return nullptr;
  }
  if (!expect(tok::kw_end, "at end of record type")) {
    Diags.note(RecordLoc, "to match this 'record'");
    return nullptr;
  }
  return Ctx.create<RecordTypeSpec>(SourceRange(Start, PrevTokEnd), Fields,
                                    Packed);
}

// advancedRecord = "record" { visibility | "var" | identList ":" type ";"
//                   | methodHeading | "class" methodHeading
//                   | "class" operatorHeading | property }
//                  [ variantPart ] "end" .
// (objfpc, {$modeswitch advancedrecords}). Visibility: private, public,
// strict private. Fields cannot follow a method or property in the same
// section (as in Free Pascal).
TypeSpec *Parser::parseAdvancedRecord(SourceLocation Start,
                                      SourceLocation RecordLoc, bool Packed) {
  llvm::SmallVector<FieldDecl *, 8> Fixed;
  llvm::SmallVector<Decl *, 8> Members;
  VariantPart *VP = nullptr;
  Visibility Vis = Visibility::Public;
  bool AfterMethods = false;
  auto Fail = [&]() -> TypeSpec * {
    skipUntil({tok::kw_end});
    tryConsume(tok::kw_end);
    return nullptr;
  };
  auto AddMember = [&](Decl *D) {
    D->setVisibility(Vis);
    Members.push_back(D);
    AfterMethods = true;
  };
  while (Tok.isNot(tok::kw_end) && Tok.isNot(tok::eof) && !Diags.shouldStop()) {
    if (Tok.is(tok::identifier)) {
      llvm::StringRef W = Tok.getIdentifier();
      if (W == "private" || W == "public") {
        Vis = W == "private" ? Visibility::Private : Visibility::Public;
        AfterMethods = false;
        consumeToken();
        continue;
      }
      if (W == "protected" || W == "published") {
        Diags.error(Tok.getLocation(),
                    "'" + W.str() + "' sections are not allowed in records");
        return Fail();
      }
      if (W == "strict") {
        consumeToken();
        if (!isIdentifierNamed("private")) {
          Diags.error(Tok.getLocation(),
                      "expected 'private' after 'strict' (records have no "
                      "protected members)");
          return Fail();
        }
        consumeToken();
        Vis = Visibility::StrictPrivate;
        AfterMethods = false;
        continue;
      }
      if (AfterMethods) {
        Diags.error(Tok.getLocation(),
                    "fields cannot follow a method or property; start a new "
                    "visibility section or 'var' first");
        return Fail();
      }
      llvm::SmallVector<Identifier, 4> Names;
      if (!parseIdentList(Names, "field name") ||
          !expect(tok::colon, "after field name"))
        return Fail();
      TypeSpec *TS = parseType();
      if (!TS)
        return Fail();
      Hints H = parseHints();
      for (const Identifier &N : Names) {
        auto *F = Ctx.create<FieldDecl>(N, TS);
        F->setVisibility(Vis);
        F->addHints(H.Flags, H.Message);
        Fixed.push_back(F);
      }
      if (!tryConsume(tok::semi) && Tok.isNot(tok::kw_end)) {
        expect(tok::semi, "after field declaration");
        return Fail();
      }
      continue;
    }
    if (tryConsume(tok::kw_var)) {
      AfterMethods = false;
      continue;
    }
    if (Tok.is(tok::kw_case)) {
      VP = parseVariantPart(tok::kw_end);
      if (!VP)
        return Fail();
      tryConsume(tok::semi);
      break; // the variant part is last
    }
    if (Tok.is(tok::kw_destructor)) {
      Diags.error(Tok.getLocation(), "a record cannot have a destructor");
      return Fail();
    }
    if (Tok.isOneOf(tok::kw_procedure, tok::kw_function, tok::kw_constructor)) {
      RoutineDecl *R = parseMethodHeading(/*InRecord=*/true);
      if (!R)
        return Fail();
      AddMember(R);
      continue;
    }
    if (Tok.is(tok::kw_property)) {
      PropertyDecl *P = parsePropertyDecl();
      if (!P)
        return Fail();
      Hints H;
      while (parseSeparatedHint(H)) {
      }
      P->addHints(H.Flags, H.Message);
      AddMember(P);
      continue;
    }
    if (Tok.is(tok::kw_class)) {
      consumeToken();
      if (isIdentifierNamed("operator")) {
        RoutineDecl *R = parseOperatorHeading();
        if (!R)
          return Fail();
        AddMember(R);
        continue;
      }
      if (Tok.isOneOf(tok::kw_procedure, tok::kw_function)) {
        RoutineDecl *R = parseMethodHeading(/*InRecord=*/true,
                                            /*RecordClassMethod=*/true);
        if (!R)
          return Fail();
        R->setClassMethod();
        AddMember(R);
        continue;
      }
      if (Tok.is(tok::kw_var)) {
        unsupported("class variables in records are");
        return Fail();
      }
      Diags.error(Tok.getLocation(),
                  "expected 'procedure', 'function' or 'operator' after "
                  "'class'");
      return Fail();
    }
    if (Tok.isOneOf(tok::kw_type, tok::kw_const)) { // nested declarations
      llvm::SmallVector<Decl *, 4> Nested;
      if (Tok.is(tok::kw_const))
        parseConstPart(Nested, /*InMembers=*/true);
      else
        parseTypePart(Nested, /*InMembers=*/true);
      for (Decl *D : Nested) {
        if (llvm::isa<VarDecl>(D)) {
          Diags.error(D->getLocation(),
                      "typed constants in records are not supported yet",
                      {D->getName().getRange()});
          continue;
        }
        AddMember(D);
      }
      continue;
    }
    Diags.error(Tok.getLocation(),
                "expected a field, method or property declaration, or 'end'");
    return Fail();
  }
  if (!expect(tok::kw_end, "at end of record type")) {
    Diags.note(RecordLoc, "to match this 'record'");
    return nullptr;
  }
  auto *FL = Ctx.create<FieldList>();
  FL->Fixed = Ctx.copyArray<FieldDecl *>(Fixed);
  FL->Variant = VP;
  auto *RS = Ctx.create<RecordTypeSpec>(SourceRange(Start, PrevTokEnd), FL,
                                        Packed);
  RS->setMembers(Ctx.copyArray<Decl *>(Members));
  return RS;
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
    Hints H = parseHints();
    for (const Identifier &N : Names) {
      auto *F = Ctx.create<FieldDecl>(N, TS);
      F->addHints(H.Flags, H.Message);
      Fixed.push_back(F);
    }
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
    if (LangOpts.ObjKeywords && Close == tok::kw_end &&
        Tok.isOneOf(tok::kw_procedure, tok::kw_function, tok::kw_constructor,
                    tok::kw_destructor, tok::kw_property, tok::kw_class)) {
      Diags.error(Tok.getLocation(),
                  "methods and properties in records need "
                  "{$modeswitch advancedrecords}");
      skipUntil({tok::kw_end}); // the record's end: continue after it
      RecordBodySkipped = true;
      return nullptr;
    }
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
                                    llvm::StringRef OpenWord,
                                    tok::TokenKind Alt) {
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
    if (Tok.is(Terminator) || (Alt != tok::eof && Tok.is(Alt)))
      return;
    // "'end'", or "'except' or 'finally'"
    std::string Expected = describe(Terminator);
    if (Alt != tok::eof)
      Expected += " or " + describe(Alt);
    if (Tok.is(tok::eof) || (Alt != tok::eof && Tok.is(tok::kw_end))) {
      Diags.error(Tok.getLocation(), "expected " + Expected);
      NoteOpen();
      return;
    }
    if (startsStatement()) {
      // Most likely a forgotten ';' -- report it and carry on parsing.
      Diags.error(PrevTokEnd, "expected ';' between statements");
      continue;
    }
    Diags.error(Tok.getLocation(),
                (Alt != tok::eof ? "expected ';', " : "expected ';' or ") +
                    Expected + " after statement");
    skipUntil({tok::semi, Terminator, Alt});
    if (tryConsume(tok::semi))
      continue;
    if (Tok.isNot(Terminator) && (Alt == tok::eof || Tok.isNot(Alt))) {
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
  case tok::kw_try:
    return parseTryStmt();
  case tok::kw_raise:
    return parseRaiseStmt();
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
  case tok::kw_except:
  case tok::kw_finally:
  case tok::eof:
    return Ctx.create<EmptyStmt>(Tok.getLocation());

  case tok::kw_goto: {
    if (LangOpts.ObjKeywords)
      diagNoGoto();
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

  // Turbo/FPC: a typecast assigned to, longint(x) := 1, or with selectors,
  // TFoo(x).Show; f(1)^.n := 2; f(x)(y);
  if (HasParens && LangOpts.Mode != LangMode::ISO &&
      Tok.isOneOf(tok::l_square, tok::period, tok::caret, tok::colon_equal,
                  tok::l_paren)) {
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
  if (Tok.is(tok::r_paren) && LangOpts.ObjKeywords) { // objfpc: f()
    consumeToken();
    return true;
  }
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

  if (Tok.is(tok::kw_in)) { // for x in collection do (Turbo/FPC)
    SourceLocation InLoc = consumeToken();
    if (LangOpts.Mode == LangMode::ISO)
      Diags.error(InLoc, "'for ... in' is a Turbo/Free Pascal extension; use "
                         "--mode=tp or --mode=objfpc");
    Expr *Collection = parseExpression();
    if (!Collection)
      return Fail();
    if (!expect(tok::kw_do, "in 'for' statement") && !startsStatement())
      return Fail();
    Stmt *Body = parseStatement();
    return Ctx.create<ForInStmt>(SourceRange(ForLoc, PrevTokEnd), Var, InLoc,
                                 Collection, Body);
  }

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
  bool Name = Tok.is(tok::identifier);
  consumeToken();
  // TOuter.C (objfpc): a constant declared in a class or record
  while (Name && LangOpts.ObjKeywords && Tok.is(tok::period) &&
         peekToken().is(tok::identifier)) {
    consumeToken(); // '.'
    E = Ctx.create<FieldExpr>(SourceRange(E->getBeginLoc(), Tok.getEndLocation()),
                              E, makeIdentifier(Tok));
    consumeToken();
  }
  return E;
}

// with = "with" designator { "," designator } "do" statement .
// tryStmt   = "try" stmtSeq ( "except" exceptPart | "finally" stmtSeq ) "end" .
// exceptPart = handler { ";" handler } [ ";" ] [ "else" stmtSeq ] | stmtSeq .
// handler   = "on" [ ident ":" ] ident "do" statement .          (objfpc)
Stmt *Parser::parseTryStmt() {
  SourceLocation TryLoc = consumeToken();
  llvm::SmallVector<Stmt *, 8> Body;
  parseStatementSequence(Body, tok::kw_except, TryLoc, "try", tok::kw_finally);
  if (Tok.isNot(tok::kw_except) && Tok.isNot(tok::kw_finally)) {
    skipUntil({tok::kw_end});
    tryConsume(tok::kw_end);
    return Ctx.create<EmptyStmt>(TryLoc);
  }
  bool IsFinally = Tok.is(tok::kw_finally);
  SourceLocation PartLoc = consumeToken();
  llvm::SmallVector<ExceptHandler *, 4> Handlers;
  llvm::SmallVector<Stmt *, 8> Part;
  SourceLocation ElseLoc;
  bool OK = true;
  if (!IsFinally && isOnHandler()) {
    while (!Diags.shouldStop() && isOnHandler()) {
      SourceLocation OnLoc = consumeToken();
      Identifier VarName, ClassName;
      if (!expectIdentifier(ClassName, "exception class name after 'on'")) {
        OK = false;
        skipUntil({tok::semi, tok::kw_end});
        if (tryConsume(tok::semi))
          continue;
        break;
      }
      if (tryConsume(tok::colon)) {
        VarName = ClassName;
        if (!expectIdentifier(ClassName, "exception class name after ':'")) {
          OK = false;
          skipUntil({tok::semi, tok::kw_end});
          if (tryConsume(tok::semi))
            continue;
          break;
        }
      }
      if (!expect(tok::kw_do, "after the exception class")) {
        OK = false;
        skipUntil({tok::semi, tok::kw_end});
        if (tryConsume(tok::semi))
          continue;
        break;
      }
      Stmt *HBody = parseStatement();
      Handlers.push_back(
          Ctx.create<ExceptHandler>(OnLoc, VarName, ClassName, HBody));
      if (!tryConsume(tok::semi))
        break;
    }
    if (Tok.is(tok::kw_else)) {
      ElseLoc = consumeToken();
      parseStatementSequence(Part, tok::kw_end, TryLoc, "try");
    } else if (Tok.isNot(tok::kw_end) && OK) {
      Diags.error(Tok.getLocation(),
                  "expected 'on', 'else' or 'end' in the 'except' part");
      OK = false;
      skipUntil({tok::kw_end});
    }
  } else {
    parseStatementSequence(Part, tok::kw_end, TryLoc, "try");
  }
  SourceLocation EndLoc = Tok.getEndLocation();
  if (!tryConsume(tok::kw_end)) {
    if (Tok.isNot(tok::eof)) // at eof, the sequence reported it
      Diags.error(Tok.getLocation(), "expected 'end' at end of 'try' statement");
    Diags.note(TryLoc, "to match this 'try'");
    return Ctx.create<EmptyStmt>(TryLoc);
  }
  if (!OK)
    return Ctx.create<EmptyStmt>(TryLoc);
  return Ctx.create<TryStmt>(SourceRange(TryLoc, EndLoc),
                             Ctx.copyArray<Stmt *>(Body), IsFinally, PartLoc,
                             Ctx.copyArray<ExceptHandler *>(Handlers), ElseLoc,
                             Ctx.copyArray<Stmt *>(Part));
}

/// objfpc: as in Free Pascal (without -Sg), at `label` and at `goto`. The
/// labels and gotos are parsed as usual, so no other errors follow.
void Parser::diagNoGoto() {
  Diags.error(Tok.getLocation(),
              "labels and 'goto' are not supported with --mode=objfpc",
              {Tok.getRange()});
}

/// `on` starts an exception handler at the start of an except part and
/// after a handler (it is not reserved).
bool Parser::isOnHandler() const {
  return Tok.is(tok::identifier) && Tok.getIdentifier() == "on";
}

// raiseStmt = "raise" [ expression ] .                          (objfpc)
Stmt *Parser::parseRaiseStmt() {
  SourceLocation RaiseLoc = consumeToken();
  Expr *Object = nullptr;
  if (!Tok.isOneOf(tok::semi, tok::kw_end, tok::kw_else, tok::kw_until,
                   tok::kw_except, tok::kw_finally, tok::eof)) {
    Object = parseExpression();
    if (!Object) {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(RaiseLoc);
    }
  }
  return Ctx.create<RaiseStmt>(SourceRange(RaiseLoc, PrevTokEnd), Object);
}

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
  // string(x): a typecast to the type `string` now means.
  if (Tok.is(tok::kw_string) && LangOpts.Mode != LangMode::ISO &&
      peekToken().is(tok::l_paren)) {
    Identifier Name{Ctx.getStrings().intern(Lex.longStrings() ? "ansistring"
                                                              : "shortstring"),
                    Tok.getRawText(), Tok.getLocation()};
    consumeToken();
    llvm::SmallVector<Expr *, 4> Args;
    if (!parseActualParams(Args, /*AllowFormat=*/false) && Args.empty())
      return nullptr;
    return parseSelectors(Ctx.create<CallExpr>(SourceRange(Name.Loc, PrevTokEnd),
                                               Name, Ctx.copyArray<Expr *>(Args)));
  }
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
