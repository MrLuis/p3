//===- AST.h - Pascal abstract syntax tree ------------------------*- C++ -*-===//
//
// Node families, each with LLVM-style RTTI (isa<>/cast<>/dyn_cast<>):
//
//   Expr      - values (literals, names, operators, calls)
//   Stmt      - actions (compound, procedure call, assignment, ...)
//   Decl      - named entities (program, variables, constants, types,
//               predeclared routines)
//   TypeSpec  - a type as written in the source (`integer`; later
//               `array [1..10] of char`, ...). Sema maps it to a Type.
//
// The parser builds the tree; Sema fills in the semantic links (Expr types,
// resolved declarations). Every node records its source range.
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

class Decl;
class Type;

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
// Operators
//===----------------------------------------------------------------------===//

enum class UnaryOp { Plus, Minus, Not };

enum class BinaryOp {
  // adding operators
  Add, Sub, Or,
  // multiplying operators
  Mul, RealDiv, IntDiv, Mod, And,
  // relational operators
  Eq, Ne, Lt, Le, Gt, Ge, In,
};

const char *getOpSpelling(UnaryOp Op);
const char *getOpSpelling(BinaryOp Op);
bool isRelational(BinaryOp Op);

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
  Type *Ty = nullptr; ///< set by Sema

protected:
  Expr(ExprKind K, SourceRange R) : Kind(K), Range(R) {}

public:
  ExprKind getKind() const { return Kind; }
  SourceRange getRange() const { return Range; }
  SourceLocation getBeginLoc() const { return Range.getBegin(); }

  Type *getType() const { return Ty; }
  void setType(Type *T) { Ty = T; }
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

/// A use of a declared name: variable, constant, ...
class NameRefExpr final : public Expr {
  Identifier Name;
  Decl *D = nullptr; ///< set by Sema

public:
  explicit NameRefExpr(Identifier Name)
      : Expr(EK_NameRef, Name.getRange()), Name(Name) {}
  const Identifier &getName() const { return Name; }
  Decl *getDecl() const { return D; }
  void setDecl(Decl *NewD) { D = NewD; }
  static bool classof(const Expr *E) { return E->getKind() == EK_NameRef; }
};

/// ( e ) -- kept so diagnostics can tell parenthesized operands apart.
class ParenExpr final : public Expr {
  Expr *Inner;

public:
  ParenExpr(SourceRange R, Expr *Inner) : Expr(EK_Paren, R), Inner(Inner) {}
  Expr *getInner() const { return Inner; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Paren; }
};

class UnaryExpr final : public Expr {
  UnaryOp Op;
  Expr *Operand;

public:
  UnaryExpr(SourceRange R, UnaryOp Op, Expr *Operand)
      : Expr(EK_Unary, R), Op(Op), Operand(Operand) {}
  UnaryOp getOp() const { return Op; }
  SourceLocation getOpLoc() const { return getBeginLoc(); }
  Expr *getOperand() const { return Operand; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Unary; }
};

class BinaryExpr final : public Expr {
  BinaryOp Op;
  SourceLocation OpLoc;
  Expr *LHS, *RHS;

public:
  BinaryExpr(SourceRange R, BinaryOp Op, SourceLocation OpLoc, Expr *LHS,
             Expr *RHS)
      : Expr(EK_Binary, R), Op(Op), OpLoc(OpLoc), LHS(LHS), RHS(RHS) {}
  BinaryOp getOp() const { return Op; }
  SourceLocation getOpLoc() const { return OpLoc; }
  Expr *getLHS() const { return LHS; }
  Expr *getRHS() const { return RHS; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Binary; }
};

/// Function call in an expression: abs(x). (User functions: M6.)
class CallExpr final : public Expr {
  Identifier Callee;
  llvm::ArrayRef<Expr *> Args;
  Decl *D = nullptr; ///< set by Sema

public:
  CallExpr(SourceRange R, Identifier Callee, llvm::ArrayRef<Expr *> Args)
      : Expr(EK_Call, R), Callee(Callee), Args(Args) {}
  const Identifier &getCallee() const { return Callee; }
  llvm::ArrayRef<Expr *> getArgs() const { return Args; }
  Decl *getDecl() const { return D; }
  void setDecl(Decl *NewD) { D = NewD; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Call; }
};

/// A write parameter with field width: `e:w` or `e:w:p` (ISO 6.9.3).
class FormattedExpr final : public Expr {
  Expr *Value;
  Expr *Width;
  Expr *Precision; ///< may be null

public:
  FormattedExpr(SourceRange R, Expr *Value, Expr *Width, Expr *Precision)
      : Expr(EK_Formatted, R), Value(Value), Width(Width), Precision(Precision) {}
  Expr *getValue() const { return Value; }
  Expr *getWidth() const { return Width; }
  Expr *getPrecision() const { return Precision; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Formatted; }
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

/// writeln('hi', x:5)   or   readln
class ProcCallStmt final : public Stmt {
  Identifier Callee;
  llvm::ArrayRef<Expr *> Args;
  bool HasParens;
  Decl *D = nullptr; ///< set by Sema

public:
  ProcCallStmt(SourceRange R, Identifier Callee, llvm::ArrayRef<Expr *> Args,
               bool HasParens)
      : Stmt(SK_ProcCall, R), Callee(Callee), Args(Args), HasParens(HasParens) {}
  const Identifier &getCallee() const { return Callee; }
  llvm::ArrayRef<Expr *> getArgs() const { return Args; }
  bool hasParens() const { return HasParens; }
  Decl *getDecl() const { return D; }
  void setDecl(Decl *NewD) { D = NewD; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_ProcCall; }
};

/// target := value
class AssignStmt final : public Stmt {
  Expr *Target;
  Expr *Value;
  SourceLocation AssignLoc;

public:
  AssignStmt(SourceRange R, Expr *Target, SourceLocation AssignLoc, Expr *Value)
      : Stmt(SK_Assign, R), Target(Target), Value(Value), AssignLoc(AssignLoc) {}
  Expr *getTarget() const { return Target; }
  Expr *getValue() const { return Value; }
  SourceLocation getAssignLoc() const { return AssignLoc; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Assign; }
};

//===----------------------------------------------------------------------===//
// Written types
//===----------------------------------------------------------------------===//

class TypeSpec {
public:
  enum TypeSpecKind { TS_Named };

private:
  const TypeSpecKind Kind;
  SourceRange Range;

protected:
  TypeSpec(TypeSpecKind K, SourceRange R) : Kind(K), Range(R) {}

public:
  TypeSpecKind getKind() const { return Kind; }
  SourceRange getRange() const { return Range; }
  SourceLocation getBeginLoc() const { return Range.getBegin(); }
};

/// A type identifier: integer, char, MyType.
class NamedTypeSpec final : public TypeSpec {
  Identifier Name;

public:
  explicit NamedTypeSpec(Identifier Name)
      : TypeSpec(TS_Named, Name.getRange()), Name(Name) {}
  const Identifier &getName() const { return Name; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Named; }
};

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

class Decl {
public:
  enum DeclKind { DK_Program, DK_Var, DK_Const, DK_Type, DK_Builtin };

private:
  const DeclKind Kind;
  Identifier Name;
  SourceLocation Loc; ///< invalid for predeclared entities

protected:
  Decl(DeclKind K, SourceLocation Loc, Identifier Name)
      : Kind(K), Name(Name), Loc(Loc) {}

public:
  DeclKind getKind() const { return Kind; }
  const Identifier &getName() const { return Name; }
  SourceLocation getLocation() const { return Loc; }
  bool isPredeclared() const { return !Loc.isValid(); }
  /// What this entity is, for diagnostics: "variable", "type", ...
  const char *getKindName() const;
};

/// var x: integer;
class VarDecl final : public Decl {
  TypeSpec *TS;
  Type *Ty = nullptr; ///< set by Sema

public:
  VarDecl(Identifier Name, TypeSpec *TS) : Decl(DK_Var, Name.Loc, Name), TS(TS) {}
  TypeSpec *getTypeSpec() const { return TS; }
  Type *getType() const { return Ty; }
  void setType(Type *T) { Ty = T; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Var; }
};

/// A named constant. M4: only the predeclared true/false/maxint.
/// M7 adds `const` declarations.
class ConstDecl final : public Decl {
  Type *Ty;
  int64_t Value;

public:
  ConstDecl(SourceLocation Loc, Identifier Name, Type *Ty, int64_t Value)
      : Decl(DK_Const, Loc, Name), Ty(Ty), Value(Value) {}
  Type *getType() const { return Ty; }
  int64_t getValue() const { return Value; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Const; }
};

/// A named type. M4: only the predeclared integer/boolean/char.
class TypeDecl final : public Decl {
  Type *Ty;

public:
  TypeDecl(SourceLocation Loc, Identifier Name, Type *Ty)
      : Decl(DK_Type, Loc, Name), Ty(Ty) {}
  Type *getType() const { return Ty; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Type; }
};

enum class BuiltinKind {
#define BUILTIN_PROC(ID, NAME) ID,
#define BUILTIN_FUNC(ID, NAME) ID,
#include "pcc/AST/Builtins.def"
};

/// A required (predeclared) procedure or function such as writeln or abs.
class BuiltinDecl final : public Decl {
  BuiltinKind BK;
  bool IsProcedure;

public:
  BuiltinDecl(Identifier Name, BuiltinKind BK, bool IsProcedure)
      : Decl(DK_Builtin, SourceLocation(), Name), BK(BK),
        IsProcedure(IsProcedure) {}
  BuiltinKind getBuiltinKind() const { return BK; }
  bool isProcedure() const { return IsProcedure; }
  bool isFunction() const { return !IsProcedure; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Builtin; }
};

/// ISO 7185 block: declaration parts followed by the statement part.
/// M4 has the variable part; label/const/type/routines arrive later.
class Block {
  llvm::ArrayRef<VarDecl *> Vars;
  CompoundStmt *Body;

public:
  Block(llvm::ArrayRef<VarDecl *> Vars, CompoundStmt *Body)
      : Vars(Vars), Body(Body) {}
  llvm::ArrayRef<VarDecl *> getVars() const { return Vars; }
  CompoundStmt *getBody() const { return Body; }
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
