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
  Lex.lex(Tok);
  return L;
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

void Parser::unsupported(llvm::StringRef What) {
  Diags.error(Tok.getLocation(), What + " not supported yet", {Tok.getRange()});
}

bool Parser::startsStatement() const {
  return Tok.isOneOf(tok::identifier, tok::kw_begin, tok::kw_if,
                     tok::kw_while, tok::kw_repeat, tok::kw_for, tok::kw_case,
                     tok::kw_with, tok::kw_goto, tok::integer_literal);
}

bool Parser::startsDeclarationPart() const {
  return Tok.isOneOf(tok::kw_label, tok::kw_const, tok::kw_type, tok::kw_var,
                     tok::kw_procedure, tok::kw_function);
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
                                 Ctx.copyArray<Identifier>(Params), B);
}

//===----------------------------------------------------------------------===//
// block = [ varPart ] compoundStmt .
//===----------------------------------------------------------------------===//

Block *Parser::parseBlock() {
  llvm::SmallVector<VarDecl *, 8> Vars;
  bool SeenVarPart = false;

  while (startsDeclarationPart() && !Diags.shouldStop()) {
    if (Tok.is(tok::kw_var)) {
      if (SeenVarPart && LangOpts.Mode == LangMode::ISO)
        Diags.error(Tok.getLocation(),
                    "ISO Pascal allows only one 'var' section per block",
                    {Tok.getRange()});
      SeenVarPart = true;
      parseVarPart(Vars);
      continue;
    }
    // label (M10), const/type (M7), procedure/function (M6).
    unsupported("'" + Tok.getRawText().lower() + "' declarations are");
    consumeToken();
    skipUntil({tok::kw_begin, tok::kw_label, tok::kw_const, tok::kw_type,
               tok::kw_var, tok::kw_procedure, tok::kw_function});
  }

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
  return Ctx.create<Block>(Ctx.copyArray<VarDecl *>(Vars), Body);
}

// varPart = "var" identList ":" type ";" { identList ":" type ";" } .
void Parser::parseVarPart(llvm::SmallVectorImpl<VarDecl *> &Vars) {
  consumeToken(); // 'var'
  if (Tok.isNot(tok::identifier)) {
    Diags.error(Tok.getLocation(), "expected variable name after 'var'");
    return;
  }

  while (Tok.is(tok::identifier) && !Diags.shouldStop()) {
    llvm::SmallVector<Identifier, 4> Names;
    do {
      if (Tok.isNot(tok::identifier)) {
        Diags.error(Tok.getLocation(), "expected variable name");
        break;
      }
      Names.push_back(makeIdentifier(Tok));
      consumeToken();
    } while (tryConsume(tok::comma));

    TypeSpec *TS = nullptr;
    if (expect(tok::colon, "after variable name"))
      TS = parseType();

    if (!TS) {
      skipUntil({tok::semi, tok::kw_begin});
    } else {
      for (const Identifier &N : Names)
        Vars.push_back(Ctx.create<VarDecl>(N, TS));
    }

    if (!expect(tok::semi, "after variable declaration")) {
      skipUntil({tok::semi, tok::kw_begin});
      tryConsume(tok::semi);
    }
  }
}

// type = typeIdent .   (M7+: enumerations, subranges, structured types)
TypeSpec *Parser::parseType() {
  if (Tok.is(tok::identifier)) {
    auto *TS = Ctx.create<NamedTypeSpec>(makeIdentifier(Tok));
    consumeToken();
    return TS;
  }
  switch (Tok.getKind()) {
  case tok::kw_array:
  case tok::kw_record:
  case tok::kw_set:
  case tok::kw_file:
  case tok::kw_packed:
    unsupported("'" + Tok.getRawText().lower() + "' types are");
    return nullptr;
  case tok::caret:
    unsupported("pointer types are");
    return nullptr;
  case tok::l_paren:
    unsupported("enumerated types are");
    return nullptr;
  case tok::integer_literal:
  case tok::string_literal:
  case tok::minus:
  case tok::plus:
    unsupported("subrange types are");
    return nullptr;
  default:
    Diags.error(Tok.getLocation(), "expected type");
    return nullptr;
  }
}

//===----------------------------------------------------------------------===//
// compound = "begin" statement { ";" statement } "end" .
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

//===----------------------------------------------------------------------===//
// statement = [ assignment | procCall | compound ] .
//===----------------------------------------------------------------------===//

Stmt *Parser::parseStatement() {
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

  // The empty statement: nothing before a separator or terminator.
  case tok::semi:
  case tok::kw_end:
  case tok::kw_else:
  case tok::kw_until:
  case tok::eof:
    return Ctx.create<EmptyStmt>(Tok.getLocation());

  case tok::kw_with: // M8
  case tok::kw_goto: // M10
    unsupported("'" + Tok.getRawText().lower() + "' statements are");
    break;
  case tok::integer_literal:
    unsupported("statement labels are");
    break;
  default:
    Diags.error(Tok.getLocation(), "expected statement");
    break;
  }

  SourceLocation L = Tok.getLocation();
  skipUntil({tok::semi, tok::kw_end});
  return Ctx.create<EmptyStmt>(L);
}

// assignment = ident ":=" expression .
// procCall   = ident [ "(" actualParam { "," actualParam } ")" ] .
Stmt *Parser::parseAssignmentOrCall() {
  Identifier Name = makeIdentifier(Tok);
  SourceLocation Start = consumeToken();

  if (Tok.isOneOf(tok::l_square, tok::period, tok::caret)) {
    unsupported(Tok.is(tok::l_square) ? "array indexing is"
                : Tok.is(tok::period) ? "record field access is"
                                      : "pointer dereferencing is");
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(Start);
  }

  // Common slip: '=' where ':=' was meant. Report it, then parse the
  // statement as an assignment anyway.
  if (Tok.is(tok::equal))
    Diags.error(Tok.getLocation(), "expected ':=' in assignment, found '='",
                {Tok.getRange()});

  if (Tok.isOneOf(tok::colon_equal, tok::equal)) {
    SourceLocation AssignLoc = consumeToken();
    auto *Target = Ctx.create<NameRefExpr>(Name);
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

  return Ctx.create<ProcCallStmt>(SourceRange(Start, PrevTokEnd), Name,
                                  Ctx.copyArray<Expr *>(Args), HasParens);
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
// Structured statements (M5)
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
  return Tok.is(tok::kw_else) ||
         (Tok.is(tok::identifier) && Tok.getIdentifier() == "otherwise");
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
      if (CaseLabel *L = parseCaseLabel())
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
CaseLabel *Parser::parseCaseLabel() {
  Expr *Lo = parseConstant();
  if (!Lo)
    return nullptr;
  Expr *Hi = nullptr;
  if (Tok.is(tok::dotdot)) {
    if (LangOpts.Mode == LangMode::ISO)
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

// constant = [ "+" | "-" ] ( unsignedInteger | ident ) | string .
Expr *Parser::parseConstant() {
  if (Tok.isOneOf(tok::plus, tok::minus)) {
    UnaryOp Op = Tok.is(tok::plus) ? UnaryOp::Plus : UnaryOp::Minus;
    SourceLocation OpLoc = consumeToken();
    Expr *Operand = nullptr;
    if (Tok.is(tok::integer_literal)) {
      Operand = Ctx.create<IntegerLiteralExpr>(Tok.getRange(),
                                               Tok.getIntegerValue());
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
  default:                 return std::nullopt;
  }
}

std::optional<BinaryOp> Parser::peekAddOp() const {
  switch (Tok.getKind()) {
  case tok::plus:  return BinaryOp::Add;
  case tok::minus: return BinaryOp::Sub;
  case tok::kw_or: return BinaryOp::Or;
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
  default:          return std::nullopt;
  }
}

static Expr *makeBinary(ASTContext &Ctx, BinaryOp Op, SourceLocation OpLoc,
                        Expr *L, Expr *R) {
  SourceRange Range(L->getBeginLoc(), R->getRange().getEnd());
  return Ctx.create<BinaryExpr>(Range, Op, OpLoc, L, R);
}

// expression = simpleExpr [ relOp simpleExpr ] .
Expr *Parser::parseExpression() {
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

// factor = unsignedConst | ident | ident "(" exprList ")"
//        | "(" expression ")" | "not" factor .
Expr *Parser::parseFactor() {
  switch (Tok.getKind()) {
  case tok::integer_literal: {
    auto *E = Ctx.create<IntegerLiteralExpr>(Tok.getRange(),
                                             Tok.getIntegerValue());
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
      return Ctx.create<CallExpr>(SourceRange(Name.Loc, PrevTokEnd), Name,
                                  Ctx.copyArray<Expr *>(Args));
    }
    if (Tok.isOneOf(tok::l_square, tok::period, tok::caret)) {
      unsupported(Tok.is(tok::l_square) ? "array indexing is"
                  : Tok.is(tok::period) ? "record field access is"
                                        : "pointer dereferencing is");
      return nullptr;
    }
    return Ctx.create<NameRefExpr>(Name);
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
    return Ctx.create<ParenExpr>(SourceRange(LParen, PrevTokEnd), Inner);
  }
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
  case tok::real_literal:
    unsupported("real numbers are");
    consumeToken();
    return nullptr;
  case tok::kw_nil:
    unsupported("'nil' is");
    consumeToken();
    return nullptr;
  case tok::l_square:
    unsupported("set constructors are");
    return nullptr;
  default:
    Diags.error(Tok.getLocation(), "expected expression");
    return nullptr;
  }
}
