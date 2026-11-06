#include "pcc/Parse/Parser.h"
#include "pcc/AST/ASTContext.h"
#include "pcc/Basic/Diagnostics.h"
#include "pcc/Lexer/Lexer.h"

#include "llvm/ADT/SmallVector.h"

using namespace pcc;

Parser::Parser(Lexer &Lex, DiagnosticsEngine &Diags, ASTContext &Ctx)
    : Lex(Lex), Diags(Diags), Ctx(Ctx) {
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

//===----------------------------------------------------------------------===//
// program = "program" ident [ "(" identList ")" ] ";" block "." .
//===----------------------------------------------------------------------===//

ProgramDecl *Parser::parseProgram() {
  SourceLocation ProgramLoc = Tok.getLocation();
  Identifier Name;
  llvm::SmallVector<Identifier, 4> Params;

  if (!tryConsume(tok::kw_program)) {
    Diags.error(Tok.getLocation(), "expected 'program'");
    // Recover by looking for the program body.
    skipUntil({tok::kw_begin});
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

  return Ctx.create<ProgramDecl>(ProgramLoc, Name, Ctx.copyArray<Identifier>(Params), B);
}

//===----------------------------------------------------------------------===//
// block = [declarations] compoundStmt .
//===----------------------------------------------------------------------===//

Block *Parser::parseBlock() {
  // Declaration parts arrive in M4 (var), M6 (procedure/function),
  // M7 (const/type) and M10 (label). Report and skip them for now.
  while (Tok.isOneOf(tok::kw_label, tok::kw_const, tok::kw_type, tok::kw_var,
                     tok::kw_procedure, tok::kw_function)) {
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
  return Ctx.create<Block>(Body);
}

//===----------------------------------------------------------------------===//
// compound = "begin" statement { ";" statement } "end" .
//===----------------------------------------------------------------------===//

CompoundStmt *Parser::parseCompoundStmt() {
  SourceLocation BeginLoc = consumeToken(); // 'begin'
  llvm::SmallVector<Stmt *, 8> Body;

  while (!Diags.shouldStop()) {
    Body.push_back(parseStatement());

    if (tryConsume(tok::semi))
      continue;
    if (Tok.is(tok::kw_end))
      break;
    if (Tok.is(tok::eof)) {
      Diags.error(Tok.getLocation(), "expected 'end'");
      Diags.note(BeginLoc, "to match this 'begin'");
      break;
    }
    if (startsStatement()) {
      // Most likely a forgotten ';' -- report it and carry on parsing.
      Diags.error(PrevTokEnd, "expected ';' between statements");
      continue;
    }
    Diags.error(Tok.getLocation(), "expected ';' or 'end' after statement");
    skipUntil({tok::semi, tok::kw_end});
    if (tryConsume(tok::semi))
      continue;
    if (Tok.isNot(tok::kw_end)) {
      Diags.note(BeginLoc, "to match this 'begin'");
      break;
    }
  }

  SourceLocation EndLoc = Tok.getEndLocation();
  tryConsume(tok::kw_end);
  return Ctx.create<CompoundStmt>(SourceRange(BeginLoc, EndLoc),
                                  Ctx.copyArray<Stmt *>(Body));
}

//===----------------------------------------------------------------------===//
// statement = [ procCall | compound ] .
//===----------------------------------------------------------------------===//

Stmt *Parser::parseStatement() {
  switch (Tok.getKind()) {
  case tok::identifier:
    return parseProcCall();
  case tok::kw_begin:
    return parseCompoundStmt();

  // The empty statement: nothing before a separator or terminator.
  case tok::semi:
  case tok::kw_end:
  case tok::kw_else:
  case tok::kw_until:
  case tok::eof:
    return Ctx.create<EmptyStmt>(Tok.getLocation());

  case tok::kw_if:
  case tok::kw_while:
  case tok::kw_repeat:
  case tok::kw_for:
  case tok::kw_case:
  case tok::kw_with:
  case tok::kw_goto:
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

//===----------------------------------------------------------------------===//
// procCall = ident [ "(" expression { "," expression } ")" ] .
//===----------------------------------------------------------------------===//

Stmt *Parser::parseProcCall() {
  Identifier Callee = makeIdentifier(Tok);
  SourceLocation Start = consumeToken();

  if (Tok.isOneOf(tok::colon_equal, tok::l_square, tok::period, tok::caret)) {
    unsupported("assignment statements are");
    skipUntil({tok::semi, tok::kw_end});
    return Ctx.create<EmptyStmt>(Start);
  }

  llvm::SmallVector<Expr *, 4> Args;
  bool HasParens = false;
  if (Tok.is(tok::l_paren)) {
    HasParens = true;
    SourceLocation LParen = consumeToken();
    if (Tok.is(tok::r_paren)) {
      Diags.error(Tok.getLocation(), "expected expression",
                  {SourceRange(LParen, Tok.getEndLocation())});
      Diags.note(LParen, "an empty argument list '()' is not allowed; "
                         "omit the parentheses");
    } else {
      do {
        if (Expr *E = parseExpression()) {
          Args.push_back(E);
          if (Tok.is(tok::colon)) {
            unsupported("field width specifiers are");
            skipUntil({tok::comma, tok::r_paren, tok::semi, tok::kw_end});
          }
        } else {
          skipUntil({tok::comma, tok::r_paren, tok::semi, tok::kw_end});
        }
      } while (tryConsume(tok::comma));
    }
    if (!expect(tok::r_paren, "after arguments"))
      Diags.note(LParen, "to match this '('");
  }

  return Ctx.create<ProcCallStmt>(SourceRange(Start, PrevTokEnd), Callee,
                                  Ctx.copyArray<Expr *>(Args), HasParens);
}

//===----------------------------------------------------------------------===//
// expression = integerLiteral | stringLiteral .
//===----------------------------------------------------------------------===//

Expr *Parser::parseExpression() {
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
  // Valid starts of expressions that later milestones implement.
  case tok::identifier:
  case tok::real_literal:
  case tok::l_paren:
  case tok::l_square:
  case tok::plus:
  case tok::minus:
  case tok::kw_not:
  case tok::kw_nil:
    unsupported("this kind of expression is");
    return nullptr;
  default:
    Diags.error(Tok.getLocation(), "expected expression");
    return nullptr;
  }
}
