//===- AST.h - Pascal abstract syntax tree ------------------------*- C++ -*-===//
//
// Three node families, each with LLVM-style RTTI (isa<>/cast<>/dyn_cast<>):
//
//   Expr  - values               (literals; later names, operators, calls)
//   Stmt  - actions              (compound, procedure call; later the rest)
//   Decl  - named entities       (program; later const/type/var/routines)
//
// TypeSpec (written types such as `array [1..10] of integer`) joins in M4/M7.
// Every node records its source range for diagnostics and debug info.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_AST_AST_H
#define PCC_AST_AST_H

#include "pcc/Basic/SourceLocation.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"

#include <cstdint>

namespace pcc {

/// A name as it appears in the source.
struct Identifier {
  llvm::StringRef Name;     ///< canonical lowercase, interned
  llvm::StringRef Spelling; ///< as written (points into the source buffer)
  SourceLocation Loc;

  bool isValid() const { return !Name.empty(); }
  SourceRange getRange() const {
    return {Loc, Loc.getLocWithOffset(static_cast<int>(Spelling.size()))};
  }
};

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

class Expr {
public:
  enum ExprKind {
#define EXPR(CLASS) EK_##CLASS,
#include "pcc/AST/ASTNodes.def"
  };

private:
  const ExprKind Kind;
  SourceRange Range;

protected:
  Expr(ExprKind K, SourceRange R) : Kind(K), Range(R) {}

public:
  ExprKind getKind() const { return Kind; }
  SourceRange getRange() const { return Range; }
  SourceLocation getBeginLoc() const { return Range.getBegin(); }
};

/// 42
class IntegerLiteralExpr final : public Expr {
  uint64_t Value;

public:
  IntegerLiteralExpr(SourceRange R, uint64_t V)
      : Expr(EK_IntegerLiteral, R), Value(V) {}
  uint64_t getValue() const { return Value; }
  static bool classof(const Expr *E) { return E->getKind() == EK_IntegerLiteral; }
};

/// 'text'. A literal of exactly one character denotes a char value.
class StringLiteralExpr final : public Expr {
  llvm::StringRef Value; ///< '' already unescaped

public:
  StringLiteralExpr(SourceRange R, llvm::StringRef V)
      : Expr(EK_StringLiteral, R), Value(V) {}
  llvm::StringRef getValue() const { return Value; }
  bool isCharLiteral() const { return Value.size() == 1; }
  static bool classof(const Expr *E) { return E->getKind() == EK_StringLiteral; }
};

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

class Stmt {
public:
  enum StmtKind {
#define STMT(CLASS) SK_##CLASS,
#include "pcc/AST/ASTNodes.def"
  };

private:
  const StmtKind Kind;
  SourceRange Range;

protected:
  Stmt(StmtKind K, SourceRange R) : Kind(K), Range(R) {}

public:
  StmtKind getKind() const { return Kind; }
  SourceRange getRange() const { return Range; }
  SourceLocation getBeginLoc() const { return Range.getBegin(); }
};

/// The empty statement, e.g. between `;;` or before `end`.
class EmptyStmt final : public Stmt {
public:
  explicit EmptyStmt(SourceLocation L) : Stmt(SK_Empty, SourceRange(L)) {}
  static bool classof(const Stmt *S) { return S->getKind() == SK_Empty; }
};

/// begin S1; S2; ... end
class CompoundStmt final : public Stmt {
  llvm::ArrayRef<Stmt *> Body;

public:
  CompoundStmt(SourceRange R, llvm::ArrayRef<Stmt *> Body)
      : Stmt(SK_Compound, R), Body(Body) {}
  llvm::ArrayRef<Stmt *> getBody() const { return Body; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Compound; }
};

/// writeln('hi', 42)   or   writeln
class ProcCallStmt final : public Stmt {
  Identifier Callee;
  llvm::ArrayRef<Expr *> Args;
  bool HasParens;

public:
  ProcCallStmt(SourceRange R, Identifier Callee, llvm::ArrayRef<Expr *> Args,
               bool HasParens)
      : Stmt(SK_ProcCall, R), Callee(Callee), Args(Args), HasParens(HasParens) {}
  const Identifier &getCallee() const { return Callee; }
  llvm::ArrayRef<Expr *> getArgs() const { return Args; }
  bool hasParens() const { return HasParens; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_ProcCall; }
};

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

/// ISO 7185 block: declaration parts followed by the statement part.
/// M4+ adds label/const/type/var/procedure/function declarations here.
class Block {
  CompoundStmt *Body;

public:
  explicit Block(CompoundStmt *Body) : Body(Body) {}
  CompoundStmt *getBody() const { return Body; }
};

class Decl {
public:
  enum DeclKind { DK_Program };

private:
  const DeclKind Kind;
  Identifier Name;
  SourceLocation Loc; ///< location of the introducing keyword

protected:
  Decl(DeclKind K, SourceLocation Loc, Identifier Name)
      : Kind(K), Name(Name), Loc(Loc) {}

public:
  DeclKind getKind() const { return Kind; }
  const Identifier &getName() const { return Name; }
  SourceLocation getLocation() const { return Loc; }
};

/// program Name(input, output); <block> .
class ProgramDecl final : public Decl {
  llvm::ArrayRef<Identifier> Params;
  Block *Body;

public:
  ProgramDecl(SourceLocation Loc, Identifier Name,
              llvm::ArrayRef<Identifier> Params, Block *Body)
      : Decl(DK_Program, Loc, Name), Params(Params), Body(Body) {}
  llvm::ArrayRef<Identifier> getParams() const { return Params; }
  Block *getBlock() const { return Body; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Program; }
};

} // namespace pcc

#endif // PCC_AST_AST_H
