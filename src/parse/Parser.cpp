#include "p3/Parse/Parser.h"
#include "p3/AST/ASTContext.h"
#include "p3/Basic/Diagnostics.h"
#include "p3/Lexer/Lexer.h"

using namespace p3;

Parser::Parser(Lexer &Lex, DiagnosticsEngine &Diags, ASTContext &Ctx)
    : Lex(Lex), Diags(Diags), Ctx(Ctx), LangOpts(Lex.getLangOpts()) {
  Lex.lex(Tok);
}

//===----------------------------------------------------------------------===//
// Token helpers
//===----------------------------------------------------------------------===//

Parser::~Parser() {
  if (UsedUnitNames)
    Lex.setBeforeDeclared(nullptr);
}

void Parser::enableDeclared(
    std::function<std::vector<std::string>(llvm::ArrayRef<Identifier>,
                                           llvm::StringRef)>
        F) {
  UsedUnitNames = std::move(F);
  Lex.setBeforeDeclared([this] { loadPendingUses(); });
}

void Parser::declareName(const Identifier &Name) {
  if (Name.isValid())
    Lex.declareName(Name.Name);
}

void Parser::loadPendingUses() {
  if (PendingUses.empty() || !UsedUnitNames)
    return;
  llvm::SmallVector<Identifier, 4> Uses = std::move(PendingUses);
  PendingUses.clear();
  for (const std::string &N : UsedUnitNames(Uses, UnitName))
    Lex.declareName(N, /*Global=*/true);
}

void Parser::nextToken(Token &T) {
  if (!Replay) {
    Lex.lex(T);
    return;
  }
  T = ReplayPos < Replay->Tokens.size() ? Replay->Tokens[ReplayPos++] : Token();
}

bool Parser::longStrings() const {
  return Replay ? Replay->LongStrings : Lex.longStrings();
}
bool Parser::advancedRecords() const {
  return Replay ? Replay->AdvancedRecords : Lex.advancedRecords();
}
bool Parser::typeHelpers() const {
  return Replay ? Replay->TypeHelpers : Lex.typeHelpers();
}
bool Parser::raceChecks() const {
  return Replay ? Replay->RaceChecks : Lex.raceChecks();
}
bool Parser::nullSafe() const {
  return Replay ? Replay->NullSafe : Lex.nullSafe();
}
bool Parser::corbaInterfaces() const {
  return Replay ? Replay->CorbaInterfaces : Lex.corbaInterfaces();
}
bool Parser::resultSwitch() const {
  return Replay ? Replay->ResultSwitch : Lex.resultSwitch();
}

bool Parser::outSwitch() const {
  return Replay ? Replay->OutSwitch : Lex.outSwitch();
}

bool Parser::defaultParamsSwitch() const {
  return Replay ? Replay->DefaultParamsSwitch : Lex.defaultParamsSwitch();
}

unsigned Parser::packRecords() const {
  return Replay ? Replay->PackRecords : Lex.packRecords();
}

bool Parser::scopedEnums() const {
  return Replay ? Replay->ScopedEnums : Lex.scopedEnums();
}

bool Parser::inlineOn() const {
  return Replay ? Replay->InlineOn : Lex.inlineOn();
}

SourceLocation Parser::consumeToken() {
  if (LambdaNames && Tok.is(tok::identifier) && LastKind != tok::period)
    LambdaNames->push_back(makeIdentifier(Tok));
  LastKind = Tok.getKind();
  if (Recording)
    Recording->push_back(Tok);
  if (StmtTokens)
    StmtTokens->push_back(Tok);
  SourceLocation L = Tok.getLocation();
  PrevTokEnd = Tok.getEndLocation();
  if (HasPeek) {
    Tok = PeekTok;
    HasPeek = false;
  } else {
    nextToken(Tok);
  }
  return L;
}

const Token &Parser::peekToken() {
  if (!HasPeek) {
    nextToken(PeekTok);
    HasPeek = true;
  }
  return PeekTok;
}

// objfpc: generic function F<T, U>(...): ...; (a template)
bool Parser::startsGeneric() {
  return LangOpts.ObjKeywords && isIdentifierNamed("generic") &&
         peekToken().isOneOf(tok::kw_function, tok::kw_procedure, tok::kw_class);
}

bool Parser::startsSpecialize() {
  return LangOpts.ObjKeywords && isIdentifierNamed("specialize") &&
         peekToken().is(tok::identifier);
}

/// The lexer switches in effect here, for tokens recorded from here on.
GenericTokens *Parser::captureSwitches() {
  auto *Toks = Ctx.create<GenericTokens>();
  Toks->LongStrings = longStrings();
  Toks->AdvancedRecords = advancedRecords();
  Toks->CorbaInterfaces = corbaInterfaces();
  Toks->InlineOn = inlineOn();
  Toks->TypeHelpers = typeHelpers();
  Toks->RaceChecks = raceChecks();
  Toks->NullSafe = nullSafe();
  Toks->ScopedEnums = scopedEnums();
  Toks->PackRecords = packRecords();
  Toks->ResultSwitch = resultSwitch();
  Toks->OutSwitch = outSwitch();
  Toks->DefaultParamsSwitch = defaultParamsSwitch();
  return Toks;
}

TypeDecl *Parser::reparseGenericType(const TypeDecl *G,
                                     llvm::SmallVectorImpl<Decl *> &Methods) {
  auto *Toks = static_cast<const GenericTokens *>(G->getGenericTokens());
  Replay = Toks;
  ReplayPos = 0;
  HasPeek = false;
  nextToken(Tok);
  TypeDecl *TD = parseTypeDecl(/*InMembers=*/false, /*Generic=*/true);
  for (const GenericTokens *M = Toks->NextMethod; M && TD; M = M->NextMethod) {
    Replay = M;
    ReplayPos = 0;
    HasPeek = false;
    nextToken(Tok);
    parseDeclarations(Methods, /*InInterface=*/false);
  }
  Replay = nullptr;
  return TD;
}

// lambda = ( "function" | "procedure" ) [ "(" [ lparams ] ")" ] [ ":" type ]
//          "=>" ( expression | statement ) .
// lparams = lparam { ( ";" | "," ) lparam } .
// lparam = [ "var" | "const" ] identList [ ":" type ] .
// A function's body is an expression, its result, or a `begin ... end`
// block that sets Result (the lambda's own); a procedure's is a statement.
// The lambda becomes a hidden routine ($lambda1) of the block being parsed,
// whose body is `$lambda1 := expression` or the statement, and the
// expression @$lambda1. Types left out are for Sema to find.
Expr *Parser::parseLambda() {
  bool IsFunction = Tok.is(tok::kw_function);
  SourceLocation Loc = consumeToken();
  // [k, s]: what it copies when it is made; [=]: everything it uses
  llvm::SmallVector<Identifier, 4> Copies;
  bool CopyAll = false;
  if (Tok.is(tok::l_square)) {
    consumeToken();
    if (Tok.is(tok::equal) && peekToken().is(tok::r_square)) {
      consumeToken();
      consumeToken();
      CopyAll = true;
    } else if (!parseIdentList(Copies, "name of a variable to copy") ||
               !expect(tok::r_square, "after the variables a lambda copies")) {
      return nullptr;
    }
  }
  llvm::SmallVector<VarDecl *, 4> Params;
  bool HasParamList = Tok.is(tok::l_paren);
  if (HasParamList) {
    consumeToken();
    if (Tok.isNot(tok::r_paren)) {
      do {
        VarDecl::VarKind Kind = VarDecl::ValueParam;
        if (tryConsume(tok::kw_var))
          Kind = VarDecl::VarParam;
        else if (tryConsume(tok::kw_const))
          Kind = VarDecl::ConstParam;
        llvm::SmallVector<Identifier, 4> Names;
        if (!parseIdentList(Names, "parameter name"))
          return nullptr;
        TypeSpec *TS = nullptr;
        if (tryConsume(tok::colon)) {
          TS = parseTypeIdentifier("parameter type");
          if (!TS)
            return nullptr;
        }
        for (const Identifier &N : Names)
          Params.push_back(Ctx.create<VarDecl>(N, TS, Kind));
      } while (tryConsume(tok::semi) || tryConsume(tok::comma));
    }
    if (!expect(tok::r_paren, "after the parameters of a lambda"))
      return nullptr;
  }
  TypeSpec *ResultTS = nullptr;
  if (Tok.is(tok::colon)) {
    if (!IsFunction)
      Diags.error(Tok.getLocation(), "a procedure cannot have a result type");
    consumeToken();
    ResultTS = parseTypeIdentifier("function result type");
    if (!ResultTS)
      return nullptr;
  }
  if (!expect(tok::equal_greater, "before the body of a lambda"))
    return nullptr;
  std::string HN = "$lambda" + std::to_string(++HiddenLambdas);
  llvm::StringRef N = Ctx.getStrings().intern(HN);
  Identifier Name{N, N, Loc};
  // What its body declares by the way (lambdas in it) is its own.
  size_t HiddenMark = HiddenTypeDecls.size();
  // [=]: the names its body uses (a lambda's in it too)
  std::vector<Identifier> Used;
  std::vector<Identifier> *SavedNames = LambdaNames;
  if (CopyAll)
    LambdaNames = &Used;
  Stmt *BodyStmt;
  bool StatementBody = IsFunction && Tok.is(tok::kw_begin);
  if (StatementBody) { // function(x) => begin ... Result := ...; end
    BodyStmt = parseStatement();
    if (!BodyStmt) {
      LambdaNames = SavedNames;
      return nullptr;
    }
  } else if (IsFunction) {
    Expr *E = parseExpression();
    if (!E) {
      LambdaNames = SavedNames;
      return nullptr;
    }
    BodyStmt = Ctx.create<AssignStmt>(E->getRange(),
                                      Ctx.create<NameRefExpr>(Name),
                                      E->getBeginLoc(), E);
  } else {
    BodyStmt = parseStatement();
  }
  if (CopyAll) {
    LambdaNames = SavedNames;
    if (SavedNames)
      SavedNames->insert(SavedNames->end(), Used.begin(), Used.end());
    // Each name once, in order; not its parameters, Result or Self.
    llvm::StringSet<> Seen{"result", "self"};
    for (VarDecl *P : Params)
      Seen.insert(P->getName().Name);
    for (const Identifier &Id : Used)
      if (Seen.insert(Id.Name).second)
        Copies.push_back(Id);
  }
  auto *Body = Ctx.create<CompoundStmt>(BodyStmt->getRange(),
                                        Ctx.copyArray<Stmt *>({BodyStmt}));
  llvm::SmallVector<Decl *, 2> Decls(HiddenTypeDecls.begin() + HiddenMark,
                                     HiddenTypeDecls.end());
  HiddenTypeDecls.resize(HiddenMark);
  auto *R = Ctx.create<RoutineDecl>(
      Loc, Name, IsFunction, Ctx.copyArray<VarDecl *>(Params), HasParamList,
      ResultTS, Ctx.create<Block>(Ctx.copyArray<Decl *>(Decls), Body), false);
  R->getBody()->setNullSafe(nullSafe());
  R->setLambda();
  if (StatementBody) // Result: the lambda's ({$modeswitch result})
    R->setResultName(resultSwitch());
  HiddenTypeDecls.push_back(R);
  auto *A = Ctx.create<AddrOfExpr>(SourceRange(Loc, PrevTokEnd),
                                   Ctx.create<NameRefExpr>(Name));
  A->setLambda(R);
  if (Copies.empty() && !CopyAll)
    return A;
  // The copies: function $copyN(k, s) (its types Sema's) with the body
  // $copyN := @$lambdaN, the lambda its own; the expression $copyN(k, s).
  HiddenTypeDecls.pop_back();
  R->setCopied();
  std::string FN = "$copy" + std::to_string(++HiddenLambdas);
  llvm::StringRef FNI = Ctx.getStrings().intern(FN);
  Identifier FName{FNI, FNI, Loc};
  llvm::SmallVector<VarDecl *, 4> FParams;
  llvm::SmallVector<Expr *, 4> Args;
  for (const Identifier &Id : Copies) {
    FParams.push_back(Ctx.create<VarDecl>(Id, nullptr, VarDecl::ValueParam));
    Args.push_back(Ctx.create<NameRefExpr>(Id));
  }
  Stmt *Set = Ctx.create<AssignStmt>(A->getRange(), Ctx.create<NameRefExpr>(FName),
                                     A->getBeginLoc(), A);
  auto *FBody = Ctx.create<CompoundStmt>(Set->getRange(),
                                         Ctx.copyArray<Stmt *>({Set}));
  auto *F = Ctx.create<RoutineDecl>(
      Loc, FName, /*IsFunction=*/true, Ctx.copyArray<VarDecl *>(FParams), true,
      nullptr, Ctx.create<Block>(Ctx.copyArray<Decl *>({R}), FBody), false);
  F->getBody()->setNullSafe(nullSafe());
  F->setLambda();
  F->setCopyFactory();
  if (CopyAll)
    F->setCopyAll();
  HiddenTypeDecls.push_back(F);
  auto *C = Ctx.create<CallExpr>(SourceRange(Loc, PrevTokEnd), FName,
                                 Ctx.copyArray<Expr *>(Args));
  C->setCopyLambda(R);
  C->setDecl(F); // (declared by Sema when it completes it)
  return C;
}

// specialize TList<integer>, a type (objfpc)
TypeSpec *Parser::parseSpecializeType() {
  SourceLocation Start = Tok.getLocation();
  Identifier Name;
  llvm::SmallVector<TypeSpec *, 2> TypeArgs;
  if (!parseSpecialization(Name, TypeArgs))
    return nullptr;
  return Ctx.create<SpecializeTypeSpec>(SourceRange(Start, PrevTokEnd), Name,
                                        Ctx.copyArray<TypeSpec *>(TypeArgs));
}

void Parser::parseTypeParamList() {
  consumeToken(); // '<'
  llvm::SmallVector<Identifier, 2> TypeParams;
  do {
    Identifier P;
    if (!expectIdentifier(P, "type parameter name"))
      break;
    TypeParams.push_back(P);
    if (Tok.is(tok::colon)) {
      Diags.error(Tok.getLocation(),
                  "constraints on type parameters are not supported yet");
      skipUntil({tok::greater, tok::semi});
    }
  } while (tryConsume(tok::comma) || tryConsume(tok::semi));
  expect(tok::greater, "after the type parameters");
  GenericTypeParams = Ctx.copyArray<Identifier>(TypeParams);
}

bool Parser::parseHeadingType(Identifier &Name, TypeSpec *&Spec,
                              llvm::StringRef What) {
  Spec = nullptr;
  if (startsSpecialize()) { // class(specialize TBase<T>)
    TypeSpec *S = parseSpecializeType();
    if (!S)
      return false;
    Spec = S;
    Name = llvm::cast<SpecializeTypeSpec>(S)->getName();
    return true;
  }
  return expectIdentifier(Name, What);
}

// genericMethod = "generic" [ "class" ] ( "function" | "procedure" ) name
//                 "<" typeParams ">" ... (a method heading, in a class)
RoutineDecl *Parser::parseGenericMethodHeading(bool InRecord) {
  consumeToken(); // 'generic'
  bool ClassMethod = tryConsume(tok::kw_class);
  if (Tok.isNot(tok::kw_function) && Tok.isNot(tok::kw_procedure)) {
    Diags.error(Tok.getLocation(),
                "expected 'function' or 'procedure' after 'generic'");
    return nullptr;
  }
  // (in a generic type, whose own type parameters are being collected)
  bool SavedParsing = ParsingGeneric;
  llvm::ArrayRef<Identifier> SavedParams = GenericTypeParams;
  ParsingGeneric = true;
  GenericTypeParams = {};
  // (in an advanced record: as its methods, a class method static)
  RoutineDecl *R = InRecord ? parseMethodHeading(/*InRecord=*/true, ClassMethod)
                            : parseMethodHeading();
  llvm::ArrayRef<Identifier> Params = GenericTypeParams;
  ParsingGeneric = SavedParsing;
  GenericTypeParams = SavedParams;
  if (!R)
    return nullptr;
  if (Params.empty()) {
    Diags.error(R->getName().Loc,
                "expected '<' and type parameters after the name of generic "
                "method '" + R->getName().Spelling + "'",
                {R->getName().getRange()});
    return nullptr;
  }
  R->setGeneric(Params, nullptr);
  R->setMemberKey(Ctx.getStrings().intern(
      (R->getName().Name + "<" + llvm::Twine(Params.size()) + ">").str()));
  if (ClassMethod)
    R->setClassMethod();
  return R;
}

RoutineDecl *Parser::parseGenericRoutine(bool HeadingOnly) {
  Token GenericTok = Tok;
  consumeToken(); // 'generic'
  // generic class function TFoo.M<T> (the implementation of a generic
  // class method)
  bool ClassMethod = false;
  Token ClassTok = Tok;
  if (Tok.is(tok::kw_class)) {
    consumeToken();
    ClassMethod = true;
    if (Tok.isNot(tok::kw_function) && Tok.isNot(tok::kw_procedure)) {
      Diags.error(Tok.getLocation(),
                  "expected 'function' or 'procedure' after 'generic class'");
      skipUntil({tok::semi, tok::kw_begin});
      tryConsume(tok::semi);
      return nullptr;
    }
  }
  GenericTokens *Toks = captureSwitches();
  llvm::SmallVector<Token, 64> Rec;
  auto *SavedRec = Recording;
  Recording = &Rec;
  ParsingGeneric = true;
  GenericTypeParams = {};
  RoutineDecl *R = parseRoutineDecl(HeadingOnly);
  ParsingGeneric = false;
  Recording = SavedRec;
  if (!R)
    return nullptr;
  if (ClassMethod) {
    if (!R->getClassName().isValid()) {
      Diags.error(R->getName().Loc,
                  "only a method can be a generic class " +
                      llvm::Twine(R->getKindName()),
                  {R->getName().getRange()});
      return nullptr;
    }
    R->setClassMethod();
  }
  // A generic method of a generic type (TList.Map<R>): a template of the
  // type, parsed again for each of its specializations (and then a
  // generic method of that specialization).
  if (R->getClassName().isValid() && R->getOuterClassNames().empty() &&
      !Replay && !SavedRec) {
    auto It = GenericTypes.find(R->getClassName().Name);
    if (It != GenericTypes.end()) {
      TypeDecl *G = It->second;
      llvm::SmallVector<Token, 64> All{GenericTok};
      if (ClassMethod)
        All.push_back(ClassTok);
      All.append(Rec.begin(), Rec.end());
      Toks->Tokens = Ctx.copyArray<Token>(All);
      auto *Last = static_cast<GenericTokens *>(G->getGenericTokens());
      while (Last->NextMethod)
        Last = Last->NextMethod;
      Last->NextMethod = Toks;
      R->setGeneric(G->getTypeParams(), Toks);
      R->setTemplateMember();
      return R;
    }
  }
  if (GenericTypeParams.empty()) {
    Diags.error(R->getName().Loc,
                "expected '<' and type parameters after the name of generic " +
                    llvm::Twine(R->getKindName()) + " '" +
                    R->getName().Spelling + "'",
                {R->getName().getRange()});
    return nullptr;
  }
  Toks->Tokens = Ctx.copyArray<Token>(Rec);
  R->setGeneric(GenericTypeParams, Toks);
  return R;
}

RoutineDecl *Parser::reparseGeneric(const RoutineDecl *G) {
  Replay = static_cast<const GenericTokens *>(G->getGenericTokens());
  ReplayPos = 0;
  HasPeek = false;
  nextToken(Tok);
  ParsingGeneric = true;
  RoutineDecl *R = parseRoutineDecl(/*HeadingOnly=*/false);
  ParsingGeneric = false;
  Replay = nullptr;
  return R;
}

bool Parser::parseSpecialization(Identifier &Name,
                                 llvm::SmallVectorImpl<TypeSpec *> &TypeArgs) {
  consumeToken(); // 'specialize'
  if (!expectIdentifier(Name, "generic name after 'specialize'") ||
      !expect(tok::less, "after the name of a generic"))
    return false;
  do {
    TypeSpec *T = parseTypeIdentifier("type argument");
    if (!T)
      return false;
    TypeArgs.push_back(T);
  } while (tryConsume(tok::comma));
  return expect(tok::greater, "after the type arguments");
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
  bool Listed = parseIdentList(Uses, "unit name");
  // declared(NAME) right after the clause sees the units' names.
  PendingUses.append(Uses.begin(), Uses.end());
  if (!Listed || !expect(tok::semi, "after uses clause")) {
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
  UnitName = Name.Name;

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
  bool InitNullSafe = nullSafe();
  if (Tok.isOneOf(tok::kw_begin, tok::kw_initialization)) {
    InitLoc = consumeToken();
    parseStatementSequence(Init, tok::kw_end, InitLoc, "begin");
  }
  SourceLocation EndLoc = Tok.getEndLocation();
  if (!expect(tok::kw_end, "at end of unit") || !expect(tok::period, "after 'end'"))
    return nullptr;

  // The types the initialization statements specialized by the way.
  ImplDecls.append(HiddenTypeDecls.begin(), HiddenTypeDecls.end());
  HiddenTypeDecls.clear();
  auto *Empty = Ctx.create<CompoundStmt>(SourceRange(InitLoc), llvm::ArrayRef<Stmt *>());
  auto *Body = Ctx.create<CompoundStmt>(SourceRange(InitLoc, EndLoc),
                                        Ctx.copyArray<Stmt *>(Init));
  auto *ImplBlock = Ctx.create<Block>(Ctx.copyArray<Decl *>(ImplDecls), Body);
  ImplBlock->setNullSafe(InitNullSafe);
  return Ctx.create<UnitDecl>(
      UnitLoc, Name, Ctx.copyArray<Identifier>(IfaceUses),
      Ctx.create<Block>(Ctx.copyArray<Decl *>(IfaceDecls), Empty),
      Ctx.copyArray<Identifier>(ImplUses), ImplBlock);
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
  size_t HiddenMark = HiddenTypeDecls.size();
  parseDeclarations(Decls, /*InInterface=*/false);

  CompoundStmt *Body = nullptr;
  bool NullSafe = nullSafe(); // (where its statements start)
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
  // The types its statements specialized by the way.
  Decls.append(HiddenTypeDecls.begin() + HiddenMark, HiddenTypeDecls.end());
  HiddenTypeDecls.resize(HiddenMark);
  auto *B = Ctx.create<Block>(Ctx.copyArray<Decl *>(Decls), Body);
  B->setNullSafe(NullSafe);
  return B;
}

void Parser::parseDeclarations(llvm::SmallVectorImpl<Decl *> &Decls,
                               bool InInterface) {
  int LastRank = -1;
  llvm::StringRef LastWord;

  while ((startsDeclarationPart() || startsGeneric()) && !Diags.shouldStop()) {
    if (Tok.is(tok::identifier)) { // generic function F<T>...
      if (RoutineDecl *R = parseGenericRoutine(InInterface))
        Decls.push_back(R);
      continue;
    }
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

    // A routine, while generic types are known: its tokens are kept, for
    // it may be the implementation of a generic type's method.
    llvm::SmallVector<Token, 64> MethodToks;
    GenericTokens *MethodSwitches = nullptr;
    size_t FirstDecl = Decls.size();
    if (!GenericTypes.empty() && !Replay && !Recording &&
        Tok.isOneOf(tok::kw_procedure, tok::kw_function, tok::kw_constructor,
                    tok::kw_destructor, tok::kw_class)) {
      MethodSwitches = captureSwitches();
      Recording = &MethodToks;
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
      if (Tok.isOneOf(tok::kw_constructor, tok::kw_destructor)) {
        // class constructor TFoo.Create; ... (a static class procedure)
        ClassInitKind K = Tok.is(tok::kw_constructor)
                              ? ClassInitKind::Constructor
                              : ClassInitKind::Destructor;
        ParsingClassInit = true;
        RoutineDecl *R = parseRoutineDecl(InInterface);
        ParsingClassInit = false;
        if (R) {
          markClassInit(R, K);
          Decls.push_back(R);
        }
        break;
      }
      if (Tok.isNot(tok::kw_procedure) && Tok.isNot(tok::kw_function)) {
        Diags.error(Tok.getLocation(),
                    "expected 'procedure', 'function', 'constructor' or "
                    "'destructor' after 'class'");
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
    if (MethodSwitches) {
      Recording = nullptr;
      auto *R = Decls.size() == FirstDecl + 1
                    ? llvm::dyn_cast<RoutineDecl>(Decls.back())
                    : nullptr;
      auto It = R && R->getClassName().isValid() &&
                        R->getOuterClassNames().empty()
                    ? GenericTypes.find(R->getClassName().Name)
                    : GenericTypes.end();
      if (It != GenericTypes.end()) { // TList.Add: a template
        TypeDecl *G = It->second;
        MethodSwitches->Tokens = Ctx.copyArray<Token>(MethodToks);
        auto *Last = static_cast<GenericTokens *>(G->getGenericTokens());
        while (Last->NextMethod)
          Last = Last->NextMethod;
        Last->NextMethod = MethodSwitches;
        R->setGeneric(G->getTypeParams(), MethodSwitches);
        R->setTemplateMember();
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
         !startsGeneric() && !Diags.shouldStop()) {
    Identifier Name = makeIdentifier(Tok);
    if (!InMembers)
      declareName(Name);
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
         !startsGeneric() && !Diags.shouldStop()) {
    // generic TList<T> = ...: a template, kept as its tokens
    if (LangOpts.ObjKeywords && !InMembers && isIdentifierNamed("generic") &&
        peekToken().is(tok::identifier)) {
      consumeToken();
      GenericTokens *Toks = captureSwitches();
      llvm::SmallVector<Token, 64> Rec;
      auto *SavedRec = Recording;
      Recording = &Rec;
      TypeDecl *TD = parseTypeDecl(InMembers, /*Generic=*/true);
      Recording = SavedRec;
      if (TD) {
        Toks->Tokens = Ctx.copyArray<Token>(Rec);
        TD->setGeneric(GenericTypeParams, Toks);
        GenericTypes[TD->getName().Name] = TD;
        Decls.push_back(TD);
      }
      continue;
    }
    if (TypeDecl *TD = parseTypeDecl(InMembers, /*Generic=*/false))
      Decls.push_back(TD);
  }
}

// typeDecl = ident [ "<" ident { "," ident } ">" ] "=" type hints ";" .
// (the type parameters: of a generic type, Generic; GenericTypeParams
// receives them). Null after an error; the ";" is consumed either way.
TypeDecl *Parser::parseTypeDecl(bool InMembers, bool Generic) {
  TypeDecl *TD = nullptr;
  {
    Identifier Name = makeIdentifier(Tok);
    if (!InMembers)
      declareName(Name);
    consumeToken();
    bool ParamsOK = true;
    if (Generic) {
      llvm::SmallVector<Identifier, 2> TypeParams;
      if (expect(tok::less, "and type parameters after the name of a "
                            "generic type")) {
        do {
          Identifier P;
          if (!expectIdentifier(P, "type parameter name"))
            break;
          TypeParams.push_back(P);
          if (Tok.is(tok::colon)) {
            Diags.error(Tok.getLocation(),
                        "constraints on type parameters are not supported yet");
            skipUntil({tok::greater, tok::semi});
          }
        } while (tryConsume(tok::comma));
        ParamsOK = expect(tok::greater, "after the type parameters") &&
                   !TypeParams.empty();
      } else {
        ParamsOK = false;
      }
      GenericTypeParams = Ctx.copyArray<Identifier>(TypeParams);
    }
    TypeSpec *TS = nullptr;
    if (ParamsOK && expect(tok::equal, "after type name")) {
      // T = type X (Free Pascal): a distinct type, here an alias of X; or
      // T = type helper for X (objfpc)
      if (Tok.is(tok::kw_type)) {
        SourceLocation TypeLoc = consumeToken();
        if (isIdentifierNamed("helper") &&
            peekToken().isOneOf(tok::kw_for, tok::l_paren)) {
          TS = parseTypeHelper(TypeLoc);
        } else {
          if (LangOpts.Mode == LangMode::ISO)
            Diags.error(TypeLoc, "'type' before a type (a distinct type) is a "
                                 "Free Pascal extension; use --mode=tp");
          TS = parseType();
        }
      } else {
        TS = parseType();
      }
    }
    Hints H = parseHints();
    if (TS) {
      TD = Ctx.create<TypeDecl>(Name.Loc, Name, TS);
      TD->addHints(H.Flags, H.Message);
    } else
      skipUntil({tok::semi, tok::kw_begin});
    if (!expect(tok::semi, "after type declaration")) {
      skipUntil({tok::semi, tok::kw_begin});
      tryConsume(tok::semi);
    }
  }
  return TD;
}

// varPart = "var" identList ":" type ";" { identList ":" type ";" } .
void Parser::parseVarPart(llvm::SmallVectorImpl<Decl *> &Decls) {
  consumeToken(); // 'var'
  if (Tok.isNot(tok::identifier)) {
    Diags.error(Tok.getLocation(), "expected variable name after 'var'");
    return;
  }

  while (Tok.is(tok::identifier) && !startsGeneric() && !Diags.shouldStop()) {
    llvm::SmallVector<Identifier, 4> Names;
    TypeSpec *TS = nullptr;
    RaceQualifier Race = RaceQualifier::None;
    bool Listed = parseIdentList(Names, "variable name");
    for (const Identifier &N : Names)
      declareName(N);
    if (Listed &&
        expect(tok::colon, "after variable name")) {
      Race = parseRaceQualifier();
      TS = parseType();
    }

    // objfpc: a variable that owns its object,  x: TFoo owned;  (`owned` is
    // not a reserved word)
    bool Owned = false;
    if (TS && LangOpts.ObjKeywords && isIdentifierNamed("owned")) {
      consumeToken();
      Owned = true;
    }

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
        V->setOwned(Owned);
        V->setRace(Race);
        V->addHints(H.Flags, H.Message);
        Decls.push_back(V);
      }
    }

    if (!expect(tok::semi, "after variable declaration")) {
      skipUntil({tok::semi, tok::kw_begin});
      tryConsume(tok::semi);
      continue;
    }

    // Turbo/FPC: a C variable  x: T; [cvar;] external ['lib'] [name 'sym'];
    // (neither word is reserved: `external: T` declares a variable)
    size_t Count = TS ? Names.size() : 0;
    while (LangOpts.Mode != LangMode::ISO &&
           (isIdentifierNamed("cvar") || isIdentifierNamed("external")) &&
           !peekToken().isOneOf(tok::colon, tok::comma)) {
      bool IsCVar = isIdentifierNamed("cvar");
      SourceLocation DirLoc = consumeToken();
      Expr *LibraryExpr = nullptr, *NameExpr = nullptr;
      if (!IsCVar) {
        if (Tok.isNot(tok::semi) && !isIdentifierNamed("name"))
          LibraryExpr = parseExpression();
        if (isIdentifierNamed("name")) {
          consumeToken();
          if (Tok.is(tok::semi))
            Diags.error(Tok.getLocation(), "expected the external symbol name");
          else
            NameExpr = parseExpression();
        }
        if (NameExpr && Count > 1)
          Diags.error(DirLoc, "'name' can only be given for one variable",
                      {SourceRange(Names[0].Loc, Names.back().getRange().getEnd())});
      }
      expect(tok::semi, IsCVar ? "after 'cvar'" : "after 'external'");
      for (size_t I = Decls.size() - Count; I < Decls.size(); ++I) {
        auto *V = llvm::cast<VarDecl>(Decls[I]);
        if (IsCVar)
          V->setCVar();
        else
          V->setExternal(LibraryExpr, NameExpr);
      }
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
  bool ResultName = resultSwitch(); // {$modeswitch result} here
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
  if (Name.isValid() && !ClassName.isValid())
    declareName(Name); // (declared(NAME); a method's is its class's)
  if (ParsingGeneric && Name.isValid() && Tok.is(tok::less)) // <T, U>
    parseTypeParamList();
  if (Name.isValid() && MK != MethodKind::None && !ClassName.isValid()) {
    std::string K = (ParsingClassInit ? "class " : "") + std::string(Kind);
    Diags.error(Name.Loc,
                "a " + K + " must be declared in a class; implement it as '" +
                    K + " TClass." + Name.Spelling.str() + "'",
                {Name.getRange()});
  }
  if (ClassName.isValid() && MK == MethodKind::None)
    MK = MethodKind::Method;

  llvm::SmallVector<VarDecl *, 4> Params;
  bool HasParamList = Tok.is(tok::l_paren);
  if (HasParamList && !parseFormalParams(Params))
    skipUntil({tok::colon, tok::semi, tok::kw_begin});

  TypeSpec *ResultTS = nullptr;
  if (tryConsume(tok::colon)) {
    // procedure P: async (objfpc): an async procedure (a task, no value)
    bool AsyncProc = !IsFunction && isAsyncWord("async") &&
                     !peekToken().isOneOf(tok::identifier, tok::kw_string);
    if (!IsFunction && !AsyncProc)
      Diags.error(PrevTokEnd, std::string("a ") + Kind +
                                  " cannot have a result type");
    ResultTS = parseTypeIdentifier("function result type");
    if (AsyncProc)
      IsFunction = true; // (Sema: its body is a procedure's)
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
  SourceLocation InlineLoc, VarArgsLoc, StaticLoc;
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
    if (isIdentifierNamed("rtlproc")) { // Free Pascal's RTL marker: no effect
      consumeToken();
      expect(tok::semi, "after 'rtlproc'");
      continue;
    }
    // static: a class method's implementation repeating its declaration's
    // (Free Pascal; Sema checks it), or no effect on a plain routine
    if (isIdentifierNamed("static") && peekToken().is(tok::semi)) {
      StaticLoc = consumeToken();
      expect(tok::semi, "after 'static'");
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
      R->setStaticDirectiveLoc(StaticLoc);
      R->setResultName(ResultName);
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

  // declared(NAME): its parameters and declarations in a scope of its own
  Lex.pushNameScope();
  for (VarDecl *P : Params)
    declareName(P->getName());
  Block *Body = parseBlock();
  Lex.popNameScope();
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
  if (ParsingGeneric && Tok.is(tok::less)) // generic function M<T>
    parseTypeParamList();
  llvm::SmallVector<VarDecl *, 4> Params;
  bool HasParamList = Tok.is(tok::l_paren);
  if (HasParamList && !parseFormalParams(Params))
    return nullptr;
  TypeSpec *ResultTS = nullptr;
  if (tryConsume(tok::colon)) {
    // procedure P: async (objfpc): an async procedure (a task, no value)
    bool AsyncProc = !IsFunction && isAsyncWord("async") &&
                     !peekToken().isOneOf(tok::identifier, tok::kw_string);
    if (!IsFunction && !AsyncProc)
      Diags.error(PrevTokEnd, std::string("a ") + Kind +
                                  " cannot have a result type");
    if (!(ResultTS = parseTypeIdentifier("function result type")))
      return nullptr;
    if (AsyncProc)
      IsFunction = true; // (Sema: its body is a procedure's)
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
    if (W == "static") { // a record's class method, or (objfpc) a class's
      Static = true;
    } else if (W == "virtual") {
      Virtual = true;
    } else if (W == "override") {
      Override = true;
    } else if (W == "abstract") {
      Abstract = true;
    } else if (W == "overload") {
      Overload = true;
    } else if (W == "reintroduce" ||
               (W == "rtlproc" && !peekToken().isOneOf(tok::colon, tok::comma))) {
      // reintroduce: hides an inherited virtual method without a warning;
      // rtlproc: Free Pascal's RTL marker (a field if followed by ':' or ',')
      // (hides an inherited virtual method without a warning: no effect)
    } else if (W == "dynamic" || W == "message" || W == "final" ||
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
  else if (InRecord && Static && !RecordClassMethod)
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
  if (Static && !InRecord) // a class's (Sema checks it is a class method)
    R->setStatic();
  return R;
}

// classInit = ("class constructor" | "class destructor") name ";"
//             { directive ";" }      (in a class or an advanced record)
RoutineDecl *Parser::parseClassInitHeading() {
  ClassInitKind K = Tok.is(tok::kw_constructor) ? ClassInitKind::Constructor
                                                : ClassInitKind::Destructor;
  RoutineDecl *R = parseMethodHeading();
  if (R)
    markClassInit(R, K);
  return R;
}

void Parser::markClassInit(RoutineDecl *R, ClassInitKind K) {
  R->setMethodKind(MethodKind::Method);
  R->setClassMethod();
  R->setStatic();
  R->setClassInit(K);
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
  if (inlineOn())
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
  bool ResultName = resultSwitch(); // {$modeswitch result} here
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
  Lex.pushNameScope();
  for (VarDecl *P : Params)
    declareName(P->getName());
  Block *Body = parseBlock();
  Lex.popNameScope();
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
  R->setResultName(ResultName);
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
  TypeSpec *ParentSpec = nullptr;              // (specialize TBase<T>)
  llvm::SmallVector<TypeSpec *, 2> InterfaceSpecs;
  bool AnySpec = false;
  auto Spec = [&](llvm::ArrayRef<Decl *> Members) {
    auto *CS = Ctx.create<ClassTypeSpec>(SourceRange(Start, PrevTokEnd), Parent,
                                         Members, false);
    CS->setInterfaces(Ctx.copyArray<Identifier>(Interfaces));
    CS->setParentSpec(ParentSpec);
    if (AnySpec)
      CS->setInterfaceSpecs(Ctx.copyArray<TypeSpec *>(InterfaceSpecs));
    return CS;
  };
  if (Tok.is(tok::l_paren)) {
    SourceLocation LParen = consumeToken();
    if (!parseHeadingType(Parent, ParentSpec, "parent class name"))
      return nullptr;
    while (tryConsume(tok::comma)) {
      Identifier I;
      TypeSpec *IS = nullptr;
      if (!parseHeadingType(I, IS, "interface name"))
        return nullptr;
      Interfaces.push_back(I);
      InterfaceSpecs.push_back(IS);
      AnySpec |= IS != nullptr;
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
    if (startsGeneric()) { // generic function M<T>(...)
      RoutineDecl *R = parseGenericMethodHeading();
      if (!R)
        return Fail();
      R->setVisibility(Vis);
      Members.push_back(R);
      ClassVars = false;
      continue;
    }
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
      RaceQualifier Race = parseRaceQualifier();
      TypeSpec *TS = parseType();
      if (!TS)
        return Fail();
      Hints H = parseHints();
      for (const Identifier &N : Names) {
        if (ClassVars) {
          auto *V = Ctx.create<VarDecl>(N, TS);
          V->setRace(Race);
          V->setClassVar();
          V->setVisibility(Vis);
          V->addHints(H.Flags, H.Message);
          Members.push_back(V);
          continue;
        }
        auto *F = Ctx.create<FieldDecl>(N, TS);
        F->setVisibility(Vis);
        F->addHints(H.Flags, H.Message);
        F->setRace(Race);
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
      if (Tok.isOneOf(tok::kw_constructor, tok::kw_destructor)) {
        RoutineDecl *R = parseClassInitHeading();
        if (!R)
          return Fail();
        R->setVisibility(Vis);
        Members.push_back(R);
        ClassVars = false;
        continue;
      }
      if (Tok.is(tok::kw_property)) { // class property P: T read ...
        PropertyDecl *P = parsePropertyDecl();
        if (!P)
          return Fail();
        Hints H;
        while (parseSeparatedHint(H)) {
        }
        P->addHints(H.Flags, H.Message);
        P->setClassProperty();
        P->setVisibility(Vis);
        Members.push_back(P);
        ClassVars = false;
        continue;
      }
      if (isIdentifierNamed("operator")) {
        Diags.error(Tok.getLocation(),
                    "class operators can only be declared in records");
        return Fail();
      }
      Diags.error(Tok.getLocation(),
                  "expected 'var', 'procedure', 'function', 'constructor', "
                  "'destructor' or 'property' after 'class'");
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
  bool Corba = corbaInterfaces();
  SourceLocation IntfLoc = consumeToken(); // 'interface'
  if (Tok.is(tok::semi)) // IFoo = interface;
    return Ctx.create<InterfaceTypeSpec>(SourceRange(Start, PrevTokEnd),
                                         Identifier(), llvm::ArrayRef<Decl *>(),
                                         /*Forward=*/true, Corba, "");
  Identifier Parent;
  TypeSpec *ParentSpec = nullptr;
  if (Tok.is(tok::l_paren)) {
    SourceLocation LParen = consumeToken();
    if (!parseHeadingType(Parent, ParentSpec, "parent interface name"))
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
    if (Tok.is(tok::kw_property)) { // read and written through its methods
      PropertyDecl *Prop = parsePropertyDecl();
      if (!Prop) {
        skipUntil({tok::kw_end});
        tryConsume(tok::kw_end);
        return nullptr;
      }
      Members.push_back(Prop);
      continue;
    }
    Diags.error(Tok.getLocation(),
                "expected a method heading, a property or 'end' in an interface");
    skipUntil({tok::kw_end});
    tryConsume(tok::kw_end);
    return nullptr;
  }
  if (!expect(tok::kw_end, "at end of interface type")) {
    Diags.note(IntfLoc, "to match this 'interface'");
    return nullptr;
  }
  auto *IS = Ctx.create<InterfaceTypeSpec>(SourceRange(Start, PrevTokEnd), Parent,
                                           Ctx.copyArray<Decl *>(Members), false,
                                           Corba, GUID);
  IS->setParentSpec(ParentSpec);
  return IS;
}

/// In a `class var` section: identList ":" type ";" (with hints), as
/// class variables of visibility Vis appended to Members.
bool Parser::parseClassVarNames(llvm::SmallVectorImpl<Decl *> &Members,
                                Visibility Vis) {
  llvm::SmallVector<Identifier, 4> Names;
  if (!parseIdentList(Names, "class variable name") ||
      !expect(tok::colon, "after class variable name"))
    return false;
  TypeSpec *TS = parseType();
  if (!TS)
    return false;
  Hints H = parseHints();
  for (const Identifier &N : Names) {
    auto *V = Ctx.create<VarDecl>(N, TS);
    V->setClassVar();
    V->setVisibility(Vis);
    V->addHints(H.Flags, H.Message);
    Members.push_back(V);
  }
  if (!tryConsume(tok::semi) && Tok.isNot(tok::kw_end)) {
    expect(tok::semi, "after class variable declaration");
    return false;
  }
  return true;
}

// typeHelper = "helper" [ "(" ident ")" ] "for" typeIdent
//              { visibility | methodHeading | "class" methodHeading
//              | property | constPart } "end" .
// After `type` (objfpc, {$modeswitch typehelpers}), or after `record` for a
// record helper (objfpc, {$modeswitch advancedrecords}).
TypeSpec *Parser::parseTypeHelper(SourceLocation Start, bool Record) {
  SourceLocation HelperLoc = consumeToken(); // 'helper'
  const char *Kind = Record ? "record helper" : "type helper";
  if (Record && (!LangOpts.ObjKeywords || !advancedRecords()))
    Diags.error(HelperLoc, "record helpers need --mode=objfpc and "
                           "{$modeswitch advancedrecords}");
  else if (!Record && (!LangOpts.ObjKeywords || !typeHelpers()))
    Diags.error(HelperLoc, "type helpers need --mode=objfpc and "
                           "{$modeswitch typehelpers}");
  Identifier Parent;
  if (Tok.is(tok::l_paren)) {
    SourceLocation LParen = consumeToken();
    if (!expectIdentifier(Parent, "parent helper name"))
      return nullptr;
    if (!expect(tok::r_paren, "after parent helper name")) {
      Diags.note(LParen, "to match this '('");
      return nullptr;
    }
  }
  if (!expect(tok::kw_for, Record ? "in a record helper" : "in a type helper"))
    return nullptr;
  // (objfpc: also `array of T`, a dynamic array type, as in a generic
  // helper for every dynamic array)
  TypeSpec *Extended = Tok.is(tok::kw_array) && peekToken().is(tok::kw_of)
                           ? parseType()
                           : parseTypeIdentifier("the type a helper extends");
  if (!Extended)
    return nullptr;
  llvm::SmallVector<Decl *, 8> Members;
  Visibility Vis = Visibility::Public;
  bool ClassVars = false; // in a `class var` section
  auto Fail = [&]() -> TypeSpec * {
    skipUntil({tok::kw_end});
    tryConsume(tok::kw_end);
    return nullptr;
  };
  while (Tok.isNot(tok::kw_end) && Tok.isNot(tok::eof) && !Diags.shouldStop()) {
    if (startsGeneric()) { // generic function M<T>(...)
      RoutineDecl *R = parseGenericMethodHeading();
      if (!R)
        return Fail();
      R->setVisibility(Vis);
      Members.push_back(R);
      ClassVars = false;
      continue;
    }
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
      if (W == "strict") {
        consumeToken();
        if (!isIdentifierNamed("private") && !isIdentifierNamed("protected")) {
          Diags.error(Tok.getLocation(),
                      "expected 'private' or 'protected' after 'strict'");
          return Fail();
        }
        Vis = isIdentifierNamed("private") ? Visibility::StrictPrivate
                                           : Visibility::StrictProtected;
        ClassVars = false;
        consumeToken();
        continue;
      }
      if (ClassVars) { // class var: static storage, not a field
        if (!parseClassVarNames(Members, Vis))
          return Fail();
        continue;
      }
      Diags.error(Tok.getLocation(), llvm::Twine("a ") + Kind + " cannot contain fields");
      return Fail();
    }
    ClassVars = false; // (anything else ends a class var section)
    if (Tok.is(tok::kw_var)) {
      Diags.error(Tok.getLocation(), llvm::Twine("a ") + Kind + " cannot contain fields");
      return Fail();
    }
    if (Tok.isOneOf(tok::kw_constructor, tok::kw_destructor)) {
      Diags.error(Tok.getLocation(),
                  llvm::Twine("constructors and destructors in ") + Kind +
                      "s are not supported yet");
      return Fail();
    }
    if (Tok.isOneOf(tok::kw_procedure, tok::kw_function)) {
      RoutineDecl *R = parseMethodHeading(/*InRecord=*/true);
      if (!R)
        return Fail();
      R->setVisibility(Vis);
      Members.push_back(R);
      continue;
    }
    if (Tok.is(tok::kw_class)) {
      consumeToken();
      if (tryConsume(tok::kw_var)) { // class variables follow
        ClassVars = true;
        continue;
      }
      if (Tok.isNot(tok::kw_procedure) && Tok.isNot(tok::kw_function)) {
        Diags.error(Tok.getLocation(),
                    "expected 'procedure', 'function' or 'var' after 'class'");
        return Fail();
      }
      RoutineDecl *R = parseMethodHeading(/*InRecord=*/true,
                                          /*RecordClassMethod=*/true);
      if (!R)
        return Fail();
      R->setClassMethod();
      R->setVisibility(Vis);
      Members.push_back(R);
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
      continue;
    }
    if (Tok.is(tok::kw_const)) {
      llvm::SmallVector<Decl *, 4> Consts;
      parseConstPart(Consts, /*InMembers=*/true);
      for (Decl *D : Consts) {
        if (llvm::isa<VarDecl>(D)) {
          Diags.error(D->getLocation(),
                      llvm::Twine("typed constants in ") + Kind +
                          "s are not supported yet",
                      {D->getName().getRange()});
          continue;
        }
        D->setVisibility(Vis);
        Members.push_back(D);
      }
      continue;
    }
    Diags.error(Tok.getLocation(),
                "expected a method, property or constant declaration, or 'end'");
    return Fail();
  }
  if (!expect(tok::kw_end, Record ? "at end of record helper" : "at end of type helper")) {
    Diags.note(HelperLoc, "to match this 'helper'");
    return nullptr;
  }
  return Ctx.create<HelperTypeSpec>(SourceRange(Start, PrevTokEnd), Parent,
                                    Extended, Ctx.copyArray<Decl *>(Members),
                                    Record);
}

// params = "(" section { ";" section } ")" ;
// section = ["var" | "const"] identList ":" typeIdent .   ("const": TP)
bool Parser::parseFormalParams(llvm::SmallVectorImpl<VarDecl *> &Params,
                               tok::TokenKind Close) {
  SourceLocation LParen = consumeToken(); // '(' or '['
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
      Params.push_back(Ctx.create<VarDecl>(Name, TS, VarDecl::ValueParam));
      continue;
    }
    bool IsVar = tryConsume(tok::kw_var);
    bool IsConst = false, IsConstRef = false;
    if (!IsVar && Tok.is(tok::kw_const) && LangOpts.Mode != LangMode::ISO) {
      consumeToken();
      IsConst = true;
    }
    // Turbo/FPC: `constref` (not a reserved word), a const parameter always
    // passed by reference.
    if (!IsVar && !IsConst && LangOpts.Mode != LangMode::ISO &&
        Tok.is(tok::identifier) && Tok.getIdentifier() == "constref" &&
        peekToken().is(tok::identifier)) {
      consumeToken();
      IsConst = IsConstRef = true;
    }
    // objfpc: `out`, not a reserved word, is a var parameter here.
    bool IsOut = !IsVar && !IsConst && outSwitch() &&
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
      if (!defaultParamsSwitch())
        Diags.error(EqLoc, "default parameter values need --mode=objfpc or "
                           "{$modeswitch defaultparameters}");
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
      auto *V = Ctx.create<VarDecl>(
          N, TS,
          IsVar ? VarDecl::VarParam
                : IsConst ? VarDecl::ConstParam : VarDecl::ValueParam);
      if (DefaultOK)
        V->setDefault(Default);
      if (IsOut)
        V->setOutParam();
      if (IsConstRef)
        V->setConstRefParam();
      Params.push_back(V);
    }
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
  TypeSpec *TS = nullableSuffix(parseTypeImpl());
  --Nesting;
  return TS;
}

// T? (objfpc), after a type's name: a class or interface reference that
// may be nil (Sema checks what it names).
TypeSpec *Parser::nullableSuffix(TypeSpec *TS) {
  if (TS && nullSafe())
    TS->setNullSafe();
  if (TS && Tok.is(tok::question) &&
      (llvm::isa<NamedTypeSpec>(TS) || llvm::isa<SpecializeTypeSpec>(TS))) {
    consumeToken();
    TS->setNullable(PrevTokEnd);
  }
  return TS;
}

// type = simpleType | ["packed"] (arrayType | recordType) .
TypeSpec *Parser::parseTypeImpl() {
  SourceLocation Start = Tok.getLocation();
  if (startsSpecialize()) // specialize TList<integer>
    return parseSpecializeType();
  bool Packed = tryConsume(tok::kw_packed);
  // bitpacked array / record (Free Pascal; not reserved)
  if (!Packed && LangOpts.Mode != LangMode::ISO &&
      isIdentifierNamed("bitpacked") &&
      peekToken().isOneOf(tok::kw_array, tok::kw_record)) {
    consumeToken();
    TypeSpec *TS = Tok.is(tok::kw_array) ? parseArrayType(Start, false)
                                         : parseRecordType(Start, false);
    if (auto *A = llvm::dyn_cast_or_null<ArrayTypeSpec>(TS))
      A->setBitPacked();
    else if (auto *R = llvm::dyn_cast_or_null<RecordTypeSpec>(TS))
      R->setBitPacked();
    else if (llvm::isa_and_nonnull<HelperTypeSpec>(TS))
      Diags.error(Start, "a record helper cannot be packed");
    return TS;
  }
  if (!Packed && (isAsyncWord("async") || startsIteratorType()))
    return parseAsyncType();
  // reference to procedure / function (objfpc; `reference` is not
  // reserved): a closure, which may outlive the routine it is made in.
  bool Reference = false;
  if (!Packed && LangOpts.ObjKeywords && isIdentifierNamed("reference") &&
      peekToken().is(tok::kw_to)) {
    consumeToken();
    consumeToken();
    if (Tok.isNot(tok::kw_procedure) && Tok.isNot(tok::kw_function)) {
      Diags.error(Tok.getLocation(),
                  "expected 'procedure' or 'function' after 'reference to'");
      return nullptr;
    }
    Reference = true;
  }
  // vector[N] of T (p3's own, like async: not reserved)
  if (!Packed && LangOpts.Mode != LangMode::ISO &&
      isIdentifierNamed("vector") && peekToken().is(tok::l_square)) {
    consumeToken();
    consumeToken();
    Expr *N = nullptr;
    if (!(N = parseExpression()) ||
        !expect(tok::r_square, "after the vector's length"))
      return nullptr;
    if (!expect(tok::kw_of, "after the vector's length"))
      return nullptr;
    TypeSpec *Elem = parseType();
    if (!Elem)
      return nullptr;
    return Ctx.create<VectorTypeSpec>(SourceRange(Start, PrevTokEnd), Elem, N);
  }
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
                                      !MaxLen && longStrings());
  }
  case tok::kw_file: {
    consumeToken();
    // Turbo/FPC: `file` alone, an untyped file
    if (Tok.isNot(tok::kw_of) && LangOpts.Mode != LangMode::ISO)
      return Ctx.create<FileTypeSpec>(SourceRange(Start, PrevTokEnd), nullptr);
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
    if (Reference)
      RS->setReference();
    if (LangOpts.ObjKeywords && Tok.is(tok::kw_of)) { // a method pointer
      if (Reference)
        Diags.error(Tok.getLocation(),
                    "a 'reference to' type cannot be 'of object'");
      consumeToken();
      if (!expect(tok::kw_object, "after 'of'"))
        return nullptr;
      if (!Reference)
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
    if (Reference && CC != CallConv::Default) {
      Diags.error(Start, "a 'reference to' type cannot have a calling "
                         "convention");
      CC = CallConv::Default;
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
// iterator type (objfpc; `iterator` is not reserved): where a type
// follows it.
bool Parser::startsIteratorType() {
  return isAsyncWord("iterator") &&
         (peekToken().isOneOf(tok::identifier, tok::kw_string) ||
          peekToken().is(tok::l_paren));
}

// async [type] (objfpc; `async` is not reserved): a task type.
// iterator type: an iterator type.
TypeSpec *Parser::parseAsyncType() {
  if (isIdentifierNamed("iterator")) {
    SourceLocation Start = consumeToken(); // iterator
    TypeSpec *Inner = parseTypeIdentifier("the values' type of an iterator");
    if (!Inner)
      return nullptr;
    return Ctx.create<AsyncTypeSpec>(SourceRange(Start, PrevTokEnd), Inner,
                                     /*Iterator=*/true);
  }
  SourceLocation Start = consumeToken(); // async
  TypeSpec *Inner = nullptr;
  if ((Tok.isOneOf(tok::identifier, tok::kw_string) ||
       (Tok.is(tok::l_paren) && LangOpts.ObjKeywords)) && // (a tuple type)
      !(Tok.is(tok::identifier) && isIdentifierNamed("async"))) {
    Inner = parseTypeIdentifier("the result type of a task");
    if (!Inner)
      return nullptr;
  }
  return Ctx.create<AsyncTypeSpec>(SourceRange(Start, PrevTokEnd), Inner);
}

TypeSpec *Parser::parseTypeIdentifier(llvm::StringRef Context) {
  return nullableSuffix(parseTypeIdentifierImpl(Context));
}

TypeSpec *Parser::parseTypeIdentifierImpl(llvm::StringRef Context) {
  if (isAsyncWord("async") || startsIteratorType())
    return parseAsyncType();
  if (Tok.is(tok::kw_string)) { // Turbo/FPC: `string` names a type here
    auto *TS = Ctx.create<StringTypeSpec>(Tok.getRange(), nullptr,
                                          longStrings());
    consumeToken();
    return TS;
  }
  if (startsSpecialize()) // specialize TList<integer>
    return parseSpecializeType();
  if (Tok.is(tok::identifier))
    return parseNamedType();
  // (integer, string): a tuple type (objfpc)
  if (Tok.is(tok::l_paren) && LangOpts.ObjKeywords &&
      Context != "variant tag type" && Context != "the type a helper extends")
    return parseTupleType(/*Always=*/true);
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
  if (LangOpts.Mode == LangMode::ISO || Tok.isNot(tok::period) ||
      peekToken().isNot(tok::identifier))
    return Ctx.create<NamedTypeSpec>(Name);
  // TOuter.TInner (objfpc), or a subrange whose first bound is a qualified
  // constant (TColor.Green..TColor.Blue, TFoo.C + 1..9).
  Expr *First = Ctx.create<NameRefExpr>(Name);
  llvm::SmallVector<Identifier, 2> Qualifiers;
  while (Tok.is(tok::period) && peekToken().is(tok::identifier)) {
    consumeToken(); // '.'
    Qualifiers.push_back(Name);
    Name = makeIdentifier(Tok);
    First = Ctx.create<FieldExpr>(SourceRange(Start, Tok.getEndLocation()),
                                  First, Name);
    consumeToken();
  }
  if (continuesConstExpr(Tok.getKind())) {
    Expr *Lo = parseSimpleExpression(First);
    if (!Lo || !expect(tok::dotdot, "in subrange type"))
      return nullptr;
    Expr *Hi = parseSimpleExpression();
    if (!Hi)
      return nullptr;
    return Ctx.create<SubrangeTypeSpec>(SourceRange(Start, PrevTokEnd), Lo, Hi);
  }
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

  if (Tok.is(tok::l_paren) && LangOpts.ObjKeywords)
    return parseTupleType(/*Always=*/false);

  if (Tok.is(tok::l_paren)) {
    bool Scoped = scopedEnums(); // {$scopedenums on}: TColor.Red only
    consumeToken();
    llvm::SmallVector<Identifier, 8> Names;
    bool Listed = parseIdentList(Names, "enumeration value name");
    if (!Scoped)
      for (const Identifier &N : Names)
        declareName(N);
    if (!Listed)
      return nullptr;
    if (!expect(tok::r_paren, "after enumeration values"))
      return nullptr;
    return Ctx.create<EnumTypeSpec>(SourceRange(Start, PrevTokEnd),
                                    Ctx.copyArray<Identifier>(Names), Scoped);
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

// tupleType = "(" [ ident ":" ] type { "," [ ident ":" ] type } ")" .  (objfpc)
// A list of names only is an enumeration, unless Always (where a type
// must be named: a result or parameter type); with two names or more it
// carries the tuple type it is if Sema finds that all of them name types.
TypeSpec *Parser::parseTupleType(bool Always) {
  SourceLocation Start = Tok.getLocation();
  bool Scoped = scopedEnums();
  consumeToken(); // '('
  llvm::SmallVector<TupleElementSpec, 4> Elems;
  llvm::SmallVector<Identifier, 8> Names;
  bool Bare = true;
  do {
    TupleElementSpec El;
    if (Tok.is(tok::identifier) && peekToken().is(tok::colon)) {
      El.Name = makeIdentifier(Tok);
      consumeToken();
      consumeToken();
      Bare = false;
      El.Type = parseType();
    } else if (Tok.is(tok::identifier) &&
               peekToken().isOneOf(tok::comma, tok::r_paren)) {
      Identifier N = makeIdentifier(Tok);
      consumeToken();
      Names.push_back(N);
      El.Type = Ctx.create<NamedTypeSpec>(N);
    } else if (Bare && !Always && !Tok.isOneOf(tok::identifier, tok::caret,
                                               tok::l_paren, tok::kw_array,
                                               tok::kw_string, tok::kw_set,
                                               tok::kw_record, tok::kw_packed,
                                               tok::kw_procedure,
                                               tok::kw_function, tok::kw_file)) {
      Diags.error(Tok.getLocation(), "expected enumeration value name");
      return nullptr;
    } else {
      Bare = false;
      El.Type = parseType();
    }
    if (!El.Type)
      return nullptr;
    Elems.push_back(El);
  } while (tryConsume(tok::comma));
  Bare = Bare && !Always;
  if (Bare && !Scoped)
    for (const Identifier &N : Names)
      declareName(N);
  if (!expect(tok::r_paren, Bare ? "after enumeration values"
                                 : "after the elements of a tuple type"))
    return nullptr;
  SourceRange R(Start, PrevTokEnd);
  if (!Bare && Elems.size() < 2) {
    Diags.error(Start, "a tuple type has two elements or more", {R});
    return nullptr;
  }
  auto *TT = Elems.size() < 2 ? nullptr
                              : Ctx.create<TupleTypeSpec>(
                                    R, Ctx.copyArray<TupleElementSpec>(Elems));
  if (!Bare)
    return TT;
  auto *ES =
      Ctx.create<EnumTypeSpec>(R, Ctx.copyArray<Identifier>(Names), Scoped);
  ES->setTuple(TT);
  return ES;
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
  unsigned Pack = packRecords(); // {$packrecords N} here
  TypeSpec *TS = parseRecordTypeBody(Start, Packed);
  if (auto *RS = llvm::dyn_cast_or_null<RecordTypeSpec>(TS))
    RS->setPackRecords(Pack);
  return TS;
}

TypeSpec *Parser::parseRecordTypeBody(SourceLocation Start, bool Packed) {
  SourceLocation RecordLoc = consumeToken(); // 'record'
  // record helper for T (objfpc): `helper` followed by `for` or `(` cannot
  // begin a field.
  if (isIdentifierNamed("helper") &&
      peekToken().isOneOf(tok::kw_for, tok::l_paren)) {
    if (Packed)
      Diags.error(Start, "a record helper cannot be packed");
    return parseTypeHelper(RecordLoc, /*Record=*/true);
  }
  if (LangOpts.ObjKeywords && advancedRecords())
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
  bool ClassVars = false; // in a `class var` section
  auto Fail = [&]() -> TypeSpec * {
    skipUntil({tok::kw_end});
    tryConsume(tok::kw_end);
    return nullptr;
  };
  auto AddMember = [&](Decl *D) {
    D->setVisibility(Vis);
    Members.push_back(D);
    AfterMethods = true;
    ClassVars = false;
  };
  while (Tok.isNot(tok::kw_end) && Tok.isNot(tok::eof) && !Diags.shouldStop()) {
    if (startsGeneric()) { // generic function M<T>(...)
      RoutineDecl *R = parseGenericMethodHeading(/*InRecord=*/true);
      if (!R)
        return Fail();
      AddMember(R);
      continue;
    }
    if (Tok.is(tok::identifier)) {
      llvm::StringRef W = Tok.getIdentifier();
      if (W == "private" || W == "public") {
        Vis = W == "private" ? Visibility::Private : Visibility::Public;
        AfterMethods = false;
        ClassVars = false;
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
        ClassVars = false;
        continue;
      }
      if (ClassVars) { // class var: static storage, not a field
        if (!parseClassVarNames(Members, Vis))
          return Fail();
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
    ClassVars = false; // (anything else ends a class var section)
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
      if (Tok.isOneOf(tok::kw_constructor, tok::kw_destructor)) {
        RoutineDecl *R = parseClassInitHeading();
        if (!R)
          return Fail();
        AddMember(R);
        continue;
      }
      if (Tok.is(tok::kw_property)) { // class property P: T read ...
        PropertyDecl *P = parsePropertyDecl();
        if (!P)
          return Fail();
        Hints H;
        while (parseSeparatedHint(H)) {
        }
        P->addHints(H.Flags, H.Message);
        P->setClassProperty();
        AddMember(P);
        continue;
      }
      if (tryConsume(tok::kw_var)) { // class variables follow
        ClassVars = true;
        AfterMethods = false;
        continue;
      }
      Diags.error(Tok.getLocation(),
                  "expected 'procedure', 'function', 'constructor', "
                  "'destructor', 'property', 'operator' or 'var' after 'class'");
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

// varStatement = "var" identList ( ":" type [ "owned" ] [ ":=" expression ]
//                                | ":=" expression )
//              | "const" ident [ ":" type ] ( ":=" | "=" ) expression .
Stmt *Parser::parseVarDeclStmt() {
  bool IsConst = Tok.is(tok::kw_const);
  SourceLocation VarLoc = consumeToken(); // 'var' or 'const'
  auto Fail = [&]() -> Stmt * {
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(VarLoc);
  };
  llvm::SmallVector<Identifier, 2> Names;
  if (Tok.is(tok::l_paren) && !IsConst) { // var (a, b) := e
    consumeToken();
    if (!parseIdentList(Names, "variable name") ||
        !expect(tok::r_paren, "after the variables of a tuple"))
      return Fail();
    llvm::SmallVector<VarDecl *, 2> Vars;
    for (const Identifier &N : Names) {
      declareName(N);
      auto *V = Ctx.create<VarDecl>(N, nullptr);
      V->setInline();
      Vars.push_back(V);
    }
    return parseDestructure(VarLoc, {}, Ctx.copyArray<VarDecl *>(Vars));
  }
  if (!parseIdentList(Names, IsConst ? "constant name" : "variable name"))
    return Fail();
  for (const Identifier &N : Names)
    declareName(N);
  TypeSpec *TS = nullptr;
  bool Owned = false;
  RaceQualifier Race = RaceQualifier::None;
  if (tryConsume(tok::colon)) {
    if (!IsConst)
      Race = parseRaceQualifier();
    TS = parseType();
    if (!TS)
      return Fail();
    if (isIdentifierNamed("owned") && !IsConst) {
      consumeToken();
      Owned = true;
    }
  }
  Expr *Init = nullptr;
  SourceLocation AssignLoc;
  if (Tok.is(tok::colon_equal) || (IsConst && Tok.is(tok::equal))) {
    AssignLoc = consumeToken();
    if (Names.size() > 1)
      Diags.error(AssignLoc,
                  IsConst ? "only one constant can be declared at a time"
                          : "only one variable can be initialized",
                  {SourceRange(Names[0].Loc, Names.back().getRange().getEnd())});
    Init = parseExpression();
    if (!Init)
      return Fail();
  } else if (IsConst) {
    Diags.error(Tok.getLocation(),
                "expected ':=' and a value after constant name");
    return Fail();
  } else if (!TS) {
    Diags.error(Tok.getLocation(),
                "expected ':' and a type, or ':=' and a value, after "
                "variable name");
    return Fail();
  }
  llvm::SmallVector<VarDecl *, 2> Vars;
  for (const Identifier &N : Names) {
    auto *V = Ctx.create<VarDecl>(N, TS);
    V->setInline();
    V->setOwned(Owned);
    V->setRace(Race);
    Vars.push_back(V);
  }
  if (Init && Vars.size() > 1)
    Init = nullptr; // (reported)
  auto *VS = Ctx.create<VarDeclStmt>(SourceRange(VarLoc, PrevTokEnd),
                                     Ctx.copyArray<VarDecl *>(Vars), Init,
                                     AssignLoc, Lex.longStrings());
  if (IsConst)
    VS->setConst();
  return VS;
}

// destructuring = ( "(" designator { "," designator } ")"
//                 | "var" "(" identList ")" ) ":=" expression .
// The targets are parsed; the token is the ":=".
Stmt *Parser::parseDestructure(SourceLocation Start,
                               llvm::ArrayRef<Expr *> Targets,
                               llvm::ArrayRef<VarDecl *> Vars) {
  if (Tok.isNot(tok::colon_equal)) {
    Diags.error(Tok.getLocation(),
                "expected ':=' and a tuple after these variables");
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(Start);
  }
  SourceLocation AssignLoc = consumeToken();
  Expr *Value = parseExpression();
  if (!Value) {
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(Start);
  }
  return Ctx.create<DestructureStmt>(SourceRange(Start, PrevTokEnd), Targets,
                                     Vars, Value, AssignLoc,
                                     Lex.longStrings());
}

//===- async (objfpc) --------------------------------------------------------===//
// await, spawn, scope, detached and async are not reserved: each is that
// word where it starts its construct and no declaration of it is visible.

bool Parser::isAsyncWord(llvm::StringRef W) {
  return LangOpts.ObjKeywords && isIdentifierNamed(W) && !Lex.isDeclared(W);
}

// raceQualifier = "shared" | "isolated"  (objfpc, before a type; the words
// are not reserved: a qualifier where a type follows and no declaration of
// the word is visible)
RaceQualifier Parser::parseRaceQualifier() {
  RaceQualifier Q = isAsyncWord("shared")     ? RaceQualifier::Shared
                    : isAsyncWord("isolated") ? RaceQualifier::Isolated
                                              : RaceQualifier::None;
  if (Q != RaceQualifier::None &&
      peekToken().isOneOf(tok::identifier, tok::kw_array, tok::kw_string,
                          tok::caret, tok::kw_packed, tok::l_paren))
    consumeToken();
  else
    Q = RaceQualifier::None;
  return Q;
}

bool Parser::startsAwaitExpr() {
  return isAsyncWord("await") &&
         peekToken().isOneOf(tok::identifier, tok::l_paren, tok::kw_inherited);
}

bool Parser::startsSpawnExpr() {
  return isAsyncWord("spawn") &&
         peekToken().isOneOf(tok::identifier, tok::kw_begin, tok::l_paren);
}

// await e: e a factor (await F(x) + 1 is (await F(x)) + 1).
Expr *Parser::parseAwaitExpr() {
  SourceLocation Start = consumeToken(); // await
  Expr *Op = parseFactor();
  if (!Op)
    return nullptr;
  return Ctx.create<AwaitExpr>(SourceRange(Start, PrevTokEnd), Op);
}

// spawn F(args) | spawn begin ... end: the block becomes a hidden async
// procedure ($spawnN, nested here: it uses this routine's variables, which
// the scope it is spawned in keeps alive), and the expression its call.
Expr *Parser::parseSpawnExpr() {
  SourceLocation Start = consumeToken(); // spawn
  if (Tok.isNot(tok::kw_begin)) {
    Expr *Call = parseFactor();
    if (!Call)
      return nullptr;
    return Ctx.create<SpawnExpr>(SourceRange(Start, PrevTokEnd), Call);
  }
  std::string HN = "$spawn" + std::to_string(++HiddenLambdas);
  llvm::StringRef N = Ctx.getStrings().intern(HN);
  Identifier Name{N, N, Start};
  size_t HiddenMark = HiddenTypeDecls.size();
  CompoundStmt *Body = parseCompoundStmt();
  llvm::SmallVector<Decl *, 2> Decls(HiddenTypeDecls.begin() + HiddenMark,
                                     HiddenTypeDecls.end());
  HiddenTypeDecls.resize(HiddenMark);
  auto *R = Ctx.create<RoutineDecl>(
      Start, Name, /*IsFunction=*/true, llvm::ArrayRef<VarDecl *>(), false,
      Ctx.create<AsyncTypeSpec>(SourceRange(Start), nullptr),
      Ctx.create<Block>(Ctx.copyArray<Decl *>(Decls), Body), false);
  R->getBody()->setNullSafe(nullSafe());
  R->setSpawnBlock();
  if (raceChecks())
    R->setRaceChecked();
  HiddenTypeDecls.push_back(R);
  auto *Call = Ctx.create<CallExpr>(SourceRange(Start, PrevTokEnd), Name,
                                    llvm::ArrayRef<Expr *>());
  Call->setDecl(R); // (declared by Sema when it completes it)
  return Ctx.create<SpawnExpr>(SourceRange(Start, PrevTokEnd), Call);
}

// scope [token] statements end. A token is an expression; without one the
// first statement starts with a keyword (or the block is empty).
Stmt *Parser::parseScopeStmt() {
  SourceLocation Loc = consumeToken(); // scope
  Expr *Token = nullptr;
  bool StatementWord =
      Tok.is(tok::identifier) &&
      (isAsyncWord("await") || isAsyncWord("spawn") || isAsyncWord("scope") ||
       isAsyncWord("detached") || isIdentifierNamed("checked") ||
       isIdentifierNamed("unchecked"));
  if ((Tok.is(tok::identifier) && !StatementWord) || Tok.is(tok::kw_nil)) {
    Token = parseExpression();
    if (!Token) {
      skipUntil({tok::kw_end});
      tryConsume(tok::kw_end);
      return Ctx.create<EmptyStmt>(Loc);
    }
  }
  llvm::SmallVector<Stmt *, 8> Body;
  parseStatementSequence(Body, tok::kw_end, Loc, "scope");
  SourceLocation EndLoc = Tok.getEndLocation();
  tryConsume(tok::kw_end);
  return Ctx.create<ScopeStmt>(SourceRange(Loc, EndLoc), Token,
                               Ctx.copyArray<Stmt *>(Body));
}

// detached [(handler)] call
Stmt *Parser::parseDetachedStmt() {
  SourceLocation Loc = consumeToken(); // detached
  Expr *Handler = nullptr;
  if (tryConsume(tok::l_paren)) {
    Handler = parseExpression();
    if (!Handler || !expect(tok::r_paren, "after the handler of 'detached'")) {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(Loc);
    }
  }
  Expr *Call = parseFactor();
  if (!Call) {
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(Loc);
  }
  return Ctx.create<DetachedStmt>(SourceRange(Loc, PrevTokEnd), Handler, Call);
}

// yield e | yield break | yield from e (objfpc; `yield` is not reserved:
// a statement where an expression or `break` follows it).
bool Parser::startsYieldStmt() {
  return isAsyncWord("yield") &&
         !peekToken().isOneOf(tok::colon_equal, tok::semi, tok::period,
                              tok::l_square, tok::caret, tok::kw_end,
                              tok::kw_else, tok::kw_until, tok::eof) &&
         !peekToken().isOneOf(tok::plus_equal, tok::minus_equal,
                              tok::star_equal, tok::slash_equal, tok::colon);
}

Stmt *Parser::parseYieldStmt() {
  SourceLocation Loc = consumeToken(); // yield
  if (Tok.is(tok::identifier) && isIdentifierNamed("break") &&
      peekToken().isOneOf(tok::semi, tok::kw_end, tok::kw_else, tok::kw_until,
                          tok::eof)) {
    consumeToken();
    return Ctx.create<YieldStmt>(SourceRange(Loc, PrevTokEnd), YieldStmt::Break,
                                 nullptr);
  }
  YieldStmt::YieldKind K = YieldStmt::Value;
  if (isAsyncWord("from") &&
      !peekToken().isOneOf(tok::semi, tok::kw_end, tok::kw_else, tok::kw_until,
                           tok::eof, tok::period, tok::l_square, tok::caret) &&
      !peekToken().isOneOf(tok::r_paren, tok::plus, tok::minus, tok::star,
                           tok::slash, tok::equal, tok::less, tok::greater) &&
      !peekToken().isOneOf(tok::kw_and, tok::kw_or, tok::kw_div, tok::kw_mod,
                           tok::not_equal, tok::less_equal, tok::greater_equal,
                           tok::kw_in)) {
    consumeToken(); // from
    K = YieldStmt::From;
  }
  Expr *E = parseExpression();
  if (!E) {
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(Loc);
  }
  return Ctx.create<YieldStmt>(SourceRange(Loc, PrevTokEnd), K, E);
}

// A statement of the async words, or null if the identifier starts none.
Stmt *Parser::parseAsyncStatement() {
  if (!LangOpts.ObjKeywords)
    return nullptr;
  SourceLocation Start = Tok.getLocation();
  if (startsYieldStmt())
    return parseYieldStmt();
  bool PoolRun = isAsyncWord("pool") && peekToken().is(tok::period);
  if (startsAwaitExpr() || startsSpawnExpr() || PoolRun) { // (Pool.Run(f): parseFactor)
    Expr *E = PoolRun                       ? parseFactor()
              : isIdentifierNamed("await") ? parseAwaitExpr()
                                           : parseSpawnExpr();
    if (!E) {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(Start);
    }
    return Ctx.create<ExprStmt>(SourceRange(Start, PrevTokEnd), E);
  }
  if (isAsyncWord("scope") &&
      peekToken().isOneOf(tok::identifier, tok::kw_nil, tok::kw_begin,
                          tok::kw_var, tok::kw_const, tok::kw_for,
                          tok::kw_while, tok::kw_repeat, tok::kw_if,
                          tok::kw_case, tok::kw_with, tok::kw_try,
                          tok::kw_raise, tok::kw_end))
    return parseScopeStmt();
  if (isAsyncWord("detached") && peekToken().isOneOf(tok::identifier, tok::l_paren))
    return parseDetachedStmt();
  return nullptr;
}

// for await [var] x in e do s: reads channel e until it is closed:
//   begin
//     var $chN := e;
//     while true do begin
//       var ($vN, $okN) := await $chN.TryReceive;
//       if not $okN then break;
//       [var] x := $vN;
//       s
//     end
//   end
Stmt *Parser::parseForAwait(SourceLocation ForLoc) {
  consumeToken(); // await
  auto Fail = [&] {
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(ForLoc);
  };
  bool Declares = tryConsume(tok::kw_var);
  Identifier X;
  if (!expectIdentifier(X, "control variable after 'for await'"))
    return Fail();
  if (Declares)
    declareName(X);
  if (!expect(tok::kw_in, "after the variable of 'for await'"))
    return Fail();
  Expr *Source = parseExpression();
  if (!Source)
    return Fail();
  if (!expect(tok::kw_do, "after the channel of 'for await'"))
    return Fail();
  Stmt *Body = parseStatement();
  SourceRange R(ForLoc, PrevTokEnd);
  unsigned K = ++HiddenLambdas;
  auto Hidden = [&](const char *Base) {
    llvm::StringRef N = Ctx.getStrings().intern(Base + std::to_string(K));
    return Identifier{N, N, ForLoc};
  };
  Identifier Ch = Hidden("$ch"), V = Hidden("$v"), Ok = Hidden("$ok");
  auto Inline = [&](const Identifier &N) {
    auto *D = Ctx.create<VarDecl>(N, nullptr);
    D->setInline();
    return D;
  };
  auto *ChDecl = Ctx.create<VarDeclStmt>(R, Ctx.copyArray<VarDecl *>({Inline(Ch)}),
                                         Source, ForLoc, Lex.longStrings());
  Expr *Receive = Ctx.create<AwaitExpr>(
      R, Ctx.create<FieldExpr>(R, Ctx.create<NameRefExpr>(Ch),
                               Identifier{Ctx.getStrings().intern("tryreceive"),
                                          Ctx.getStrings().intern("TryReceive"),
                                          ForLoc}));
  Stmt *Take = Ctx.create<DestructureStmt>(
      R, llvm::ArrayRef<Expr *>(), Ctx.copyArray<VarDecl *>({Inline(V), Inline(Ok)}),
      Receive, ForLoc, Lex.longStrings());
  Identifier BreakId{Ctx.getStrings().intern("break"),
                     Ctx.getStrings().intern("break"), ForLoc};
  Stmt *Stop = Ctx.create<IfStmt>(
      R, Ctx.create<UnaryExpr>(R, UnaryOp::Not, Ctx.create<NameRefExpr>(Ok)),
      Ctx.create<ProcCallStmt>(R, BreakId, llvm::ArrayRef<Expr *>(), false), nullptr);
  Stmt *SetX = Declares
                   ? static_cast<Stmt *>(Ctx.create<VarDeclStmt>(
                         R, Ctx.copyArray<VarDecl *>({Inline(X)}),
                         Ctx.create<NameRefExpr>(V), ForLoc, Lex.longStrings()))
                   : Ctx.create<AssignStmt>(R, Ctx.create<NameRefExpr>(X), ForLoc,
                                            Ctx.create<NameRefExpr>(V));
  auto *Loop = Ctx.create<CompoundStmt>(R, Ctx.copyArray<Stmt *>({Take, Stop, SetX, Body}));
  Identifier TrueId{Ctx.getStrings().intern("true"), Ctx.getStrings().intern("true"),
                    ForLoc};
  Stmt *While = Ctx.create<WhileStmt>(R, Ctx.create<NameRefExpr>(TrueId), Loop);
  return Ctx.create<CompoundStmt>(R, Ctx.copyArray<Stmt *>({ChDecl, While}));
}

// checked statements end / unchecked statements end (objfpc; the words
// are not reserved): a block whose integer arithmetic is checked for
// overflow (an EIntOverflow exception) or not (it wraps, and so do integer
// values converted to narrower integer types), whatever {$Q} says around
// it; {$Q} inside switches again. The block is a compound statement; the
// switches are recorded with the {$Q} directives.
bool Parser::startsCheckedBlock() {
  if (!LangOpts.ObjKeywords ||
      !(isIdentifierNamed("checked") || isIdentifierNamed("unchecked")))
    return false;
  // (checked := ..., checked(...), checked; are a variable's or a call's)
  return peekToken().isOneOf(tok::identifier, tok::kw_begin, tok::kw_if,
                             tok::kw_while, tok::kw_repeat, tok::kw_for,
                             tok::kw_case, tok::kw_with, tok::kw_try,
                             tok::kw_raise, tok::kw_var, tok::kw_const,
                             tok::kw_goto, tok::kw_inherited, tok::kw_end);
}

/// The switches of a checked (or unchecked) block or expression from L on;
/// Before / WrapBefore receive the states they replace.
void Parser::setCheckedSwitches(SourceLocation L, bool Checked,
                                CheckDirectives::State &Before,
                                CheckDirectives::State &WrapBefore) {
  Before = Lex.checkStateAt(CheckDirectives::Overflow, L);
  WrapBefore = Lex.checkStateAt(CheckDirectives::Wrap, L);
  Lex.addCheckState(L, CheckDirectives::Overflow,
                    Checked ? CheckDirectives::On : CheckDirectives::Off);
  Lex.addCheckState(L, CheckDirectives::Wrap,
                    Checked ? CheckDirectives::Off : CheckDirectives::On);
}

// checked(expression) / unchecked(expression) (objfpc): the expression's
// integer arithmetic checked for overflow or wrapping, as in a block; a
// value (in parentheses). A routine of that name (declared by the program)
// makes it a call.
bool Parser::startsCheckedExpr() {
  if (!LangOpts.ObjKeywords || !peekToken().is(tok::l_paren))
    return false;
  if (isIdentifierNamed("checked"))
    return !Lex.isDeclared("checked");
  if (isIdentifierNamed("unchecked"))
    return !Lex.isDeclared("unchecked");
  return false;
}

Expr *Parser::parseCheckedExpr() {
  bool Checked = isIdentifierNamed("checked");
  SourceLocation Loc = Tok.getLocation();
  CheckDirectives::State Before, WrapBefore;
  setCheckedSwitches(Loc, Checked, Before, WrapBefore);
  consumeToken();
  consumeToken(); // '('
  Expr *Inner = parseExpression();
  Lex.addCheckState(Tok.getLocation(), CheckDirectives::Overflow, Before);
  Lex.addCheckState(Tok.getLocation(), CheckDirectives::Wrap, WrapBefore);
  if (!Inner ||
      !expect(tok::r_paren, Checked ? "after the expression of 'checked'"
                                    : "after the expression of 'unchecked'"))
    return nullptr;
  return Ctx.create<ParenExpr>(SourceRange(Loc, PrevTokEnd), Inner);
}

Stmt *Parser::parseCheckedBlock() {
  bool Checked = isIdentifierNamed("checked");
  SourceLocation Loc = Tok.getLocation();
  CheckDirectives::State Before, WrapBefore;
  setCheckedSwitches(Loc, Checked, Before, WrapBefore);
  consumeToken();
  llvm::SmallVector<Stmt *, 8> Body;
  parseStatementSequence(Body, tok::kw_end, Loc,
                         Checked ? "checked" : "unchecked");
  Lex.addCheckState(Tok.getLocation(), CheckDirectives::Overflow, Before);
  Lex.addCheckState(Tok.getLocation(), CheckDirectives::Wrap, WrapBefore);
  SourceLocation EndLoc = Tok.getEndLocation();
  tryConsume(tok::kw_end);
  return Ctx.create<CompoundStmt>(SourceRange(Loc, EndLoc),
                                  Ctx.copyArray<Stmt *>(Body));
}

Stmt *Parser::parseStatementImpl() {
  switch (Tok.getKind()) {
  case tok::identifier:
    if (Stmt *S = parseAsyncStatement())
      return S;
    if (startsCheckedBlock())
      return parseCheckedBlock();
    return recordingDesignator([&] { return parseAssignmentOrCall(); });
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
  case tok::kw_var:
    if (!LangOpts.ObjKeywords) {
      Diags.error(Tok.getLocation(),
                  "variables declared among the statements need "
                  "--mode=objfpc");
      break;
    }
    return parseVarDeclStmt();
  case tok::kw_const:
    if (!LangOpts.ObjKeywords) {
      Diags.error(Tok.getLocation(),
                  "constants declared among the statements need "
                  "--mode=objfpc");
      break;
    }
    return parseVarDeclStmt();
  case tok::l_paren: { // Turbo/FPC: (p + 1)^ := x; (obj as TFoo).Show;
    if (LangOpts.Mode == LangMode::ISO) {
      Diags.error(Tok.getLocation(), "expected statement");
      break;
    }
    return recordingDesignator([&]() -> Stmt * {
      SourceLocation Start = Tok.getLocation();
      Expr *Target = parseFactor();
      if (!Target) {
        skipUntil({tok::semi, tok::kw_end});
        return Ctx.create<EmptyStmt>(Start);
      }
      if (auto *T = llvm::dyn_cast<TupleExpr>(Target)) // (a, b) := e
        return parseDestructure(Start, T->getElements(), {});
      return finishDesignatorStmt(Start, Target);
    });
  }
  case tok::kw_inherited:
    return recordingDesignator([&]() -> Stmt * {
      SourceLocation Start = Tok.getLocation();
      Expr *Target = parseInherited();
      if (!Target) {
        skipUntil({tok::semi, tok::kw_end});
        return Ctx.create<EmptyStmt>(Start);
      }
      return finishDesignatorStmt(Start, Target);
    });

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
  if (startsSpecialize()) { // specialize P<T>(args);
    SourceLocation Start = Tok.getLocation();
    Identifier Name;
    llvm::SmallVector<TypeSpec *, 2> TypeArgs;
    llvm::SmallVector<Expr *, 4> Args;
    bool HasParens = false;
    if (parseSpecialization(Name, TypeArgs)) {
      HasParens = Tok.is(tok::l_paren);
      if (HasParens)
        parseActualParams(Args, /*AllowFormat=*/false);
    } else {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(Start);
    }
    auto *S = Ctx.create<ProcCallStmt>(SourceRange(Start, PrevTokEnd), Name,
                                       Ctx.copyArray<Expr *>(Args), HasParens);
    S->setTypeArgs(Ctx.copyArray<TypeSpec *>(TypeArgs));
    return S;
  }
  Identifier Name = makeIdentifier(Tok);
  SourceLocation Start = consumeToken();

  Expr *Target = Ctx.create<NameRefExpr>(Name);
  bool HasSelectors = Tok.isOneOf(tok::l_square, tok::period, tok::caret,
                                  tok::question_period);
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

  if (isAssignOp())
    return parseAssignment(Start, Target);

  llvm::SmallVector<Expr *, 4> Args;
  bool HasParens = Tok.is(tok::l_paren);
  if (HasParens) {
    parseActualParams(Args, /*AllowFormat=*/true);
  } else if (atTemplate()) { // Log`...`; a tagged template string
    HasParens = true;
    if (!parseTemplateArgs(Args)) {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(Start);
    }
  }

  // Turbo/FPC: a typecast assigned to, longint(x) := 1, or with selectors,
  // TFoo(x).Show; f(1)^.n := 2; f(x)(y);
  if (HasParens && LangOpts.Mode != LangMode::ISO &&
      (Tok.isOneOf(tok::l_square, tok::period, tok::caret, tok::colon_equal,
                   tok::l_paren, tok::question_period) ||
       (isAssignOp() && Tok.isNot(tok::equal)))) {
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

template <typename ParseFn> Stmt *Parser::recordingDesignator(ParseFn Parse) {
  llvm::SmallVector<Token, 16> Toks;
  auto *Saved = StmtTokens;
  StmtTokens = &Toks;
  Stmt *S = Parse();
  StmtTokens = Saved;
  return S;
}

bool Parser::isAssignOp() const {
  return Tok.isOneOf(tok::colon_equal, tok::equal, tok::plus_equal,
                     tok::minus_equal, tok::star_equal, tok::slash_equal);
}

Stmt *Parser::parseAssignment(SourceLocation Start, Expr *Target) {
  // The designator's tokens (recorded up to the operator).
  llvm::SmallVector<Token, 16> TargetToks;
  if (StmtTokens)
    TargetToks.assign(StmtTokens->begin(), StmtTokens->end());
  StmtTokens = nullptr; // the rest is not needed
  tok::TokenKind K = Tok.getKind();
  bool Compound = K != tok::colon_equal && K != tok::equal;
  SourceLocation AssignLoc = consumeToken();
  Expr *Value = parseExpression();
  if (!Value) {
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(Start);
  }
  if (Compound) {
    // x += e is x := x + (e), x read again (as in Free Pascal, whose copy
    // of x evaluates its indexes and calls again).
    Expr *Copy = reparseDesignator(TargetToks);
    if (!Copy)
      return Ctx.create<EmptyStmt>(Start);
    BinaryOp Op = K == tok::plus_equal    ? BinaryOp::Add
                  : K == tok::minus_equal ? BinaryOp::Sub
                  : K == tok::star_equal  ? BinaryOp::Mul
                                          : BinaryOp::RealDiv;
    Value = Ctx.create<BinaryExpr>(
        SourceRange(Copy->getBeginLoc(), Value->getRange().getEnd()), Op,
        AssignLoc, Copy, Value);
  }
  auto *S = Ctx.create<AssignStmt>(SourceRange(Start, PrevTokEnd), Target,
                                   AssignLoc, Value);
  S->setCompound(Compound);
  return S;
}

Expr *Parser::reparseDesignator(llvm::ArrayRef<Token> Toks) {
  GenericTokens G;
  G.Tokens = Toks;
  G.LongStrings = longStrings();
  G.AdvancedRecords = advancedRecords();
  G.CorbaInterfaces = corbaInterfaces();
  G.InlineOn = inlineOn();
  G.TypeHelpers = typeHelpers();
  G.RaceChecks = raceChecks();
  G.NullSafe = nullSafe();
  G.ScopedEnums = scopedEnums();
  G.PackRecords = packRecords();
  G.ResultSwitch = resultSwitch();
  G.OutSwitch = outSwitch();
  G.DefaultParamsSwitch = defaultParamsSwitch();
  Token SavedTok = Tok, SavedPeek = PeekTok;
  bool SavedHasPeek = HasPeek;
  SourceLocation SavedPrev = PrevTokEnd;
  const GenericTokens *SavedReplay = Replay;
  size_t SavedPos = ReplayPos;
  auto *SavedRecording = Recording;
  Recording = nullptr; // recorded already
  Replay = &G;
  ReplayPos = 0;
  HasPeek = false;
  nextToken(Tok);
  Expr *E = parseFactor();
  Tok = SavedTok;
  PeekTok = SavedPeek;
  HasPeek = SavedHasPeek;
  PrevTokEnd = SavedPrev;
  Replay = SavedReplay;
  ReplayPos = SavedPos;
  Recording = SavedRecording;
  return E;
}

// The rest of a statement that starts with a designator with selectors:
// an assignment, or (Turbo/FPC) a call through a procedural value or a
// method call: r.p(x); a[i]; obj.Show; inherited Create(x);
Stmt *Parser::finishDesignatorStmt(SourceLocation Start, Expr *Target) {
  if (Tok.is(tok::equal))
    Diags.error(Tok.getLocation(), "expected ':=' in assignment, found '='",
                {Tok.getRange()});

  if (isAssignOp())
    return parseAssignment(Start, Target);

  if (auto *Nav = llvm::dyn_cast<SafeNavExpr>(Target);
      Nav && Tok.isOneOf(tok::semi, tok::kw_end, tok::kw_else, tok::kw_until,
                         tok::eof)) {
    // x?.P(args); a call where x is not nil (each `?.` of the chain)
    while (true) {
      Nav->setStatement();
      if (auto *Inner = llvm::dyn_cast<SafeNavExpr>(Nav->getAccess())) {
        Nav = Inner;
        continue;
      }
      if (!llvm::isa<IndirectCallExpr>(Nav->getAccess())) // x?.P
        Nav->setAccess(Ctx.create<IndirectCallExpr>(
            Nav->getAccess()->getRange(), Nav->getAccess(), llvm::ArrayRef<Expr *>()));
      break;
    }
    return Ctx.create<ExprStmt>(SourceRange(Start, PrevTokEnd), Target);
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
    // Name := value: the argument for the parameter Name (objfpc)
    if (Tok.is(tok::identifier) && peekToken().is(tok::colon_equal) &&
        LangOpts.Mode != LangMode::ISO) {
      Identifier Name = makeIdentifier(Tok);
      consumeToken();
      consumeToken();
      if (!LangOpts.ObjKeywords)
        Diags.error(Name.Loc, "named arguments need --mode=objfpc",
                    {Name.getRange()});
      Expr *V = parseExpression();
      if (!V) {
        skipUntil({tok::comma, tok::r_paren, tok::semi, tok::kw_end});
        continue;
      }
      Args.push_back(Ctx.create<NamedArgExpr>(SourceRange(Name.Loc, PrevTokEnd),
                                              Name, V));
      continue;
    }
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
  if (isAsyncWord("await") && peekToken().isOneOf(tok::kw_var, tok::identifier))
    return parseForAwait(ForLoc);
  auto Fail = [&] {
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(ForLoc);
  };

  // objfpc: for var x [: T] ..., a variable of the loop
  bool Declares = LangOpts.ObjKeywords && Tok.is(tok::kw_var);
  if (Declares)
    consumeToken();
  if (Tok.isNot(tok::identifier)) {
    Diags.error(Tok.getLocation(), "expected control variable after 'for'");
    return Fail();
  }
  auto *Var = Ctx.create<NameRefExpr>(makeIdentifier(Tok));
  consumeToken();
  VarDecl *Declared = nullptr;
  if (Declares) {
    declareName(Var->getName());
    TypeSpec *TS = nullptr;
    if (tryConsume(tok::colon)) {
      TS = parseType();
      if (!TS)
        return Fail();
    }
    Declared = Ctx.create<VarDecl>(Var->getName(), TS);
    Declared->setInline();
  }

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
    auto *In = Ctx.create<ForInStmt>(SourceRange(ForLoc, PrevTokEnd), Var,
                                     InLoc, Collection, Body);
    In->setInlineVar(Declared);
    return In;
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
  auto *For = Ctx.create<ForStmt>(SourceRange(ForLoc, PrevTokEnd), Var, Init,
                                  Final, Downto, Body);
  For->setInlineVar(Declared);
  return For;
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
                                               Tok.getIntegerValue(),
                                             Tok.isBasedInteger());
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
    E = Ctx.create<IntegerLiteralExpr>(Tok.getRange(), Tok.getIntegerValue(),
                                             Tok.isBasedInteger());
    break;
  case tok::real_literal:
    E = Ctx.create<RealLiteralExpr>(Tok.getRange(), Tok.getRealValue());
    break;
  case tok::string_literal:
    E = Ctx.create<StringLiteralExpr>(Tok.getRange(), Tok.getStringValue(),
                                      Tok.getWideCharCode());
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
  // TOuter.C (objfpc): a constant declared in a class or record; TColor.Red
  while (Name && LangOpts.Mode != LangMode::ISO && Tok.is(tok::period) &&
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

// raiseStmt = "raise" [ expression [ "at" expression [ "," expression ] ] ] .
//                                                                (objfpc)
// (`at`, the exception's address and frame, is not a reserved word.)
Stmt *Parser::parseRaiseStmt() {
  SourceLocation RaiseLoc = consumeToken();
  Expr *Object = nullptr, *Address = nullptr, *Frame = nullptr;
  if (!Tok.isOneOf(tok::semi, tok::kw_end, tok::kw_else, tok::kw_until,
                   tok::kw_except, tok::kw_finally, tok::eof)) {
    Object = parseExpression();
    if (Object && Tok.is(tok::identifier) && Tok.getIdentifier() == "at") {
      consumeToken();
      Address = parseExpression();
      if (Address && Tok.is(tok::comma)) {
        consumeToken();
        Frame = parseExpression();
        if (!Frame)
          Address = nullptr;
      }
      if (!Address)
        Object = nullptr;
    }
    if (!Object) {
      skipUntil({tok::semi, tok::kw_end});
      return Ctx.create<EmptyStmt>(RaiseLoc);
    }
  }
  return Ctx.create<RaiseStmt>(SourceRange(RaiseLoc, PrevTokEnd), Object,
                               Address, Frame);
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
  // a ?? b (objfpc): below the comparisons, grouping to the right
  if (E && Tok.is(tok::question_question)) {
    SourceLocation OpLoc = consumeToken();
    Expr *R = parseExpression();
    E = R ? Ctx.create<CoalesceExpr>(
                SourceRange(E->getBeginLoc(), R->getRange().getEnd()), E, OpLoc, R)
          : nullptr;
  }
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
Expr *Parser::parseSimpleExpression(Expr *First) {
  Expr *LHS;
  if (First) {
    LHS = parseTerm(First);
  } else if (Tok.isOneOf(tok::plus, tok::minus)) {
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
Expr *Parser::parseTerm(Expr *First) {
  Expr *LHS = First ? First : parseFactor();
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
    // x?.m... (objfpc): the selectors that follow apply to x's value
    // (Hole) where it is not nil
    if (Tok.is(tok::question_period)) {
      SourceLocation Q = consumeToken();
      Identifier Field;
      if (!expectIdentifier(Field, "member name after '?.'"))
        return nullptr;
      llvm::StringRef HN = Ctx.getStrings().intern("$nav");
      auto *Hole = Ctx.create<NameRefExpr>(Identifier{HN, HN, Q});
      Expr *Access = parseSelectors(Ctx.create<FieldExpr>(
          SourceRange(Q, PrevTokEnd), Hole, Field));
      if (!Access)
        return nullptr;
      return Ctx.create<SafeNavExpr>(SourceRange(Base->getBeginLoc(), PrevTokEnd),
                                     Base, Access, Hole);
    }
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
      if (startsSpecialize()) { // obj.specialize M<T>: a generic method
        llvm::SmallVector<TypeSpec *, 2> TypeArgs;
        if (!parseSpecialization(Field, TypeArgs))
          return nullptr;
        Base = Ctx.create<FieldExpr>(SourceRange(Base->getBeginLoc(), PrevTokEnd),
                                     Base, Field,
                                     Ctx.copyArray<TypeSpec *>(TypeArgs));
        continue;
      }
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
    // A tagged template string: obj.Sql`...` is obj.Sql([texts], [values]).
    if (atTemplate() && !llvm::isa<NameRefExpr>(Base)) {
      llvm::SmallVector<Expr *, 2> Args;
      if (!parseTemplateArgs(Args))
        return nullptr;
      Base = Ctx.create<IndirectCallExpr>(
          SourceRange(Base->getBeginLoc(), PrevTokEnd), Base,
          Ctx.copyArray<Expr *>(Args));
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

//===----------------------------------------------------------------------===//
// Template strings:  `text ${ expression [ ":" width [ ":" digits ] ] } text`
//===----------------------------------------------------------------------===//

/// The parts of the template string at the token: its text (2 * n + 1
/// StringLiteralExprs, without the layout's indentation) and its n values.
bool Parser::parseTemplateParts(llvm::SmallVectorImpl<Expr *> &Texts,
                                llvm::SmallVectorImpl<Expr *> &Values) {
  llvm::SmallVector<std::string, 4> Chunks;
  llvm::SmallVector<SourceRange, 4> Ranges;
  bool OK = true;
  auto Text = [&] {
    Chunks.push_back(Tok.getStringValue().str());
    Ranges.push_back(Tok.getRange());
    consumeToken();
  };
  bool More = Tok.is(tok::template_head);
  Text();
  while (More) {
    Expr *E = parseExpression();
    if (E && Tok.is(tok::colon)) {
      consumeToken();
      Expr *Width = parseExpression();
      Expr *Precision = nullptr;
      bool Bad = !Width;
      if (!Bad && tryConsume(tok::colon))
        Bad = !(Precision = parseExpression());
      if (Bad)
        E = nullptr;
      else
        E = Ctx.create<FormattedExpr>(
            SourceRange(E->getBeginLoc(), PrevTokEnd), E, Width, Precision);
    }
    if (E && !Tok.isOneOf(tok::template_middle, tok::template_tail)) {
      Diags.error(Tok.getLocation(),
                  "expected '}' after the value in a template string");
      E = nullptr;
    }
    if (!E) {
      // Recover at this template's next text, past the templates inside.
      OK = false;
      unsigned Depth = 0;
      while (Tok.isNot(tok::eof)) {
        if (Tok.is(tok::template_head))
          ++Depth;
        else if (Depth == 0 &&
                 Tok.isOneOf(tok::template_middle, tok::template_tail))
          break;
        else if (Tok.is(tok::template_tail))
          --Depth;
        consumeToken();
      }
      if (Tok.is(tok::eof))
        return false;
    } else {
      Values.push_back(E);
    }
    More = Tok.is(tok::template_middle);
    Text();
  }
  if (!OK)
    return false;
  Lexer::dedentTemplate(Chunks);
  for (size_t I = 0; I < Chunks.size(); ++I)
    Texts.push_back(Ctx.create<StringLiteralExpr>(
        Ranges[I], Ctx.getStrings().intern(Chunks[I])));
  return true;
}

/// A template string with interpolations: a TemplateExpr.
Expr *Parser::parseTemplate() {
  SourceLocation Start = Tok.getLocation();
  llvm::SmallVector<Expr *, 4> Texts, Values, Parts;
  if (!parseTemplateParts(Texts, Values))
    return nullptr;
  for (size_t I = 0; I < Texts.size(); ++I) {
    Parts.push_back(Texts[I]);
    if (I < Values.size())
      Parts.push_back(Values[I]);
  }
  return Ctx.create<TemplateExpr>(SourceRange(Start, PrevTokEnd),
                                  Ctx.copyArray<Expr *>(Parts));
}

/// A tagged template string, Tag`a ${x} b`: the arguments of the call
/// Tag(['a ', ' b'], [x]), the text parts and the values.
bool Parser::parseTemplateArgs(llvm::SmallVectorImpl<Expr *> &Args) {
  SourceLocation Start = Tok.getLocation();
  llvm::SmallVector<Expr *, 4> Texts, Values;
  if (!parseTemplateParts(Texts, Values))
    return false;
  SourceRange R(Start, PrevTokEnd);
  llvm::SmallVector<SetElement, 4> Elems;
  for (Expr *T : Texts)
    Elems.push_back({T, nullptr});
  Args.push_back(
      Ctx.create<SetCtorExpr>(R, Ctx.copyArray<SetElement>(Elems)));
  Elems.clear();
  for (Expr *V : Values) {
    if (auto *F = llvm::dyn_cast<FormattedExpr>(V)) {
      Diags.error(F->getWidth()->getBeginLoc(),
                  "a value of a tagged template string cannot have a field "
                  "width",
                  {F->getWidth()->getRange()});
      V = F->getValue();
    }
    Elems.push_back({V, nullptr});
  }
  Args.push_back(
      Ctx.create<SetCtorExpr>(R, Ctx.copyArray<SetElement>(Elems)));
  return true;
}

// factor = unsignedConst | designator | ident "(" args ")"
//        | "(" expression ")" | "not" factor .
Expr *Parser::parseFactor() {
  // string(x): a typecast to the type `string` now means.
  if (Tok.is(tok::kw_string) && LangOpts.Mode != LangMode::ISO &&
      peekToken().is(tok::l_paren)) {
    Identifier Name{Ctx.getStrings().intern(longStrings() ? "ansistring"
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
                                             Tok.getIntegerValue(),
                                             Tok.isBasedInteger());
    consumeToken();
    return E;
  }
  case tok::real_literal: {
    auto *E = Ctx.create<RealLiteralExpr>(Tok.getRange(), Tok.getRealValue());
    consumeToken();
    return E;
  }
  case tok::string_literal: {
    auto *E = Ctx.create<StringLiteralExpr>(
        Tok.getRange(), Tok.getStringValue(), Tok.getWideCharCode());
    consumeToken();
    if (typeHelpers() && Tok.is(tok::period)) // 'abc'.ToUpper
      return parseSelectors(E);
    return E;
  }
  case tok::template_head:
    return parseTemplate();
  case tok::identifier: {
    if (startsCheckedExpr())
      return parseCheckedExpr();
    if (startsAwaitExpr())
      return parseAwaitExpr();
    if (startsSpawnExpr())
      return parseSpawnExpr();
    // Pool.Run(F(args)) (objfpc; Pool not declared): F's task on the pool
    if (isAsyncWord("pool") && peekToken().is(tok::period)) {
      SourceLocation Start = Tok.getLocation();
      Identifier PoolId = makeIdentifier(Tok);
      consumeToken();
      consumeToken(); // '.'
      Identifier Member;
      if (!expectIdentifier(Member, "member name after '.'"))
        return nullptr;
      if (Member.Name == "run" && Tok.is(tok::l_paren)) {
        consumeToken();
        Expr *Call = parseExpression();
        if (!Call || !expect(tok::r_paren, "after the call Pool.Run runs"))
          return nullptr;
        return Ctx.create<SpawnExpr>(SourceRange(Start, PrevTokEnd), Call,
                                     /*OnPool=*/true);
      }
      return parseSelectors(Ctx.create<FieldExpr>(
          SourceRange(Start, PrevTokEnd), Ctx.create<NameRefExpr>(PoolId), Member));
    }
    if (startsSpecialize()) { // specialize F<T>(args)
      SourceLocation Start = Tok.getLocation();
      Identifier Name;
      llvm::SmallVector<TypeSpec *, 2> TypeArgs;
      if (!parseSpecialization(Name, TypeArgs))
        return nullptr;
      if (Tok.is(tok::period)) { // specialize TList<integer>.Create
        std::string HN = "$spec" + std::to_string(++HiddenTypes);
        llvm::StringRef N = Ctx.getStrings().intern(HN);
        Identifier Id{N, N, Start};
        auto *TS = Ctx.create<SpecializeTypeSpec>(
            SourceRange(Start, PrevTokEnd), Name,
            Ctx.copyArray<TypeSpec *>(TypeArgs));
        HiddenTypeDecls.push_back(Ctx.create<TypeDecl>(Start, Id, TS));
        return parseSelectors(Ctx.create<NameRefExpr>(Id));
      }
      llvm::SmallVector<Expr *, 4> Args;
      if (Tok.is(tok::l_paren) &&
          !parseActualParams(Args, /*AllowFormat=*/false) && Args.empty())
        return nullptr;
      auto *Call = Ctx.create<CallExpr>(SourceRange(Start, PrevTokEnd), Name,
                                        Ctx.copyArray<Expr *>(Args));
      Call->setTypeArgs(Ctx.copyArray<TypeSpec *>(TypeArgs));
      return parseSelectors(Call);
    }
    // objfpc: move p, the object of the owned variable p (`move` is not a
    // reserved word: only a name can follow it here)
    if (LangOpts.ObjKeywords && isIdentifierNamed("move") &&
        peekToken().is(tok::identifier)) {
      SourceLocation Start = consumeToken();
      Identifier Var = makeIdentifier(Tok);
      consumeToken();
      return Ctx.create<MoveExpr>(SourceRange(Start, PrevTokEnd),
                                  Ctx.create<NameRefExpr>(Var));
    }
    Identifier Name = makeIdentifier(Tok);
    consumeToken();
    if (atTemplate()) { // a tagged template string: Tag`...`
      llvm::SmallVector<Expr *, 2> Args;
      if (!parseTemplateArgs(Args))
        return nullptr;
      return parseSelectors(
          Ctx.create<CallExpr>(SourceRange(Name.Loc, PrevTokEnd), Name,
                               Ctx.copyArray<Expr *>(Args)));
    }
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
    if (Tok.is(tok::comma) && LangOpts.ObjKeywords) { // (a, b): a tuple
      llvm::SmallVector<Expr *, 4> Elems;
      Elems.push_back(Inner);
      while (tryConsume(tok::comma)) {
        Expr *E = parseExpression();
        if (!E)
          return nullptr;
        Elems.push_back(E);
      }
      if (!expect(tok::r_paren, "after the elements of a tuple")) {
        Diags.note(LParen, "to match this '('");
        return nullptr;
      }
      Expr *Tuple = Ctx.create<TupleExpr>(SourceRange(LParen, PrevTokEnd),
                                          Ctx.copyArray<Expr *>(Elems),
                                          Lex.longStrings());
      if (Tok.is(tok::period))
        return parseSelectors(Tuple);
      return Tuple;
    }
    if (Tok.is(tok::comma) && LangOpts.Mode == LangMode::TP &&
        !LangOpts.ObjKeywords) {
      Diags.error(Tok.getLocation(), "tuples need --mode=objfpc");
      Diags.note(LParen, "to match this '('");
      return nullptr;
    }
    if (!expect(tok::r_paren, "after expression")) {
      Diags.note(LParen, "to match this '('");
      return nullptr;
    }
    Expr *Paren = Ctx.create<ParenExpr>(SourceRange(LParen, PrevTokEnd), Inner);
    // Turbo/FPC: (p + 1)^, (obj as TFoo).Show
    if (LangOpts.Mode != LangMode::ISO &&
        Tok.isOneOf(tok::period, tok::caret, tok::l_square))
      return parseSelectors(Paren);
    return Paren;
  }
  case tok::kw_if: { // if c then a else b, a value (objfpc)
    if (!LangOpts.ObjKeywords) {
      Diags.error(Tok.getLocation(), "expected expression");
      return nullptr;
    }
    SourceLocation IfLoc = consumeToken();
    Expr *Cond = parseExpression();
    if (!Cond || !expect(tok::kw_then, "after the condition of a conditional "
                                       "expression"))
      return nullptr;
    Expr *Then = parseExpression();
    if (!Then)
      return nullptr;
    if (Tok.isNot(tok::kw_else)) {
      Diags.error(Tok.getLocation(),
                  "expected 'else': a conditional expression has a value "
                  "either way");
      Diags.note(IfLoc, "the conditional expression starts here");
      return nullptr;
    }
    consumeToken();
    Expr *Else = parseExpression();
    if (!Else)
      return nullptr;
    return Ctx.create<CondExpr>(SourceRange(IfLoc, PrevTokEnd), Cond, Then,
                                Else, Lex.longStrings());
  }
  case tok::kw_function:
  case tok::kw_procedure: // function(x) => e, a lambda (objfpc)
    if (!LangOpts.ObjKeywords) {
      Diags.error(Tok.getLocation(), "expected expression");
      return nullptr;
    }
    return parseLambda();
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
