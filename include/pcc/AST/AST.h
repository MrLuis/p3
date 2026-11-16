//===- AST.h - Pascal abstract syntax tree ------------------------*- C++ -*-===//
//
// Node families, each with LLVM-style RTTI (isa<>/cast<>/dyn_cast<>):
//
//   Expr      - values (literals, names, operators, calls, a[i], r.f)
//   Stmt      - actions
//   Decl      - named entities: program, variables/parameters, constants,
//               types, record fields, procedures/functions, builtins
//   TypeSpec  - a type as written (`integer`, `1..10`, `array [..] of T`,
//               `record ... end`); Sema maps it to a Type
//
// The parser builds the tree; Sema fills in the semantic links (types,
// resolved declarations, constant values). All nodes live in the ASTContext
// arena and must stay trivially destructible.
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

class ClassType;
class InterfaceType;
class Decl;
class FieldDecl;
class LabelDecl;
class RoutineDecl;
class Type;
class TaskType;
class TypeSpec;
class PropertyDecl;
class VarDecl;
struct WithContext;

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

/// Value of a constant (Sema-computed).
struct ConstValue {
  enum Kind { None, Ordinal, Real, String };
  Kind K = None;
  int64_t Int = 0;      ///< Ordinal: integer, char code, boolean, enum index
  double RealVal = 0;   ///< Real
  llvm::StringRef Str;  ///< String (interned)

  static ConstValue ordinal(int64_t V) { ConstValue C; C.K = Ordinal; C.Int = V; return C; }
  static ConstValue real(double V) { ConstValue C; C.K = Real; C.RealVal = V; return C; }
  static ConstValue string(llvm::StringRef S) { ConstValue C; C.K = String; C.Str = S; return C; }
  bool isValid() const { return K != None; }
};

//===----------------------------------------------------------------------===//
// Operators
//===----------------------------------------------------------------------===//

enum class UnaryOp { Plus, Minus, Not };

/// A calling convention directive (Turbo/FPC), as written. pcc calls every
/// routine with the target's C convention (a cdecl routine's procedural
/// values point to a C entry without the static link); as in Free Pascal on 64-bit
/// targets, 'stdcall' is the default convention and 'cdecl' a different one,
/// so headings, implementations, overrides, interface implementations and
/// procedural values must agree on whether they are 'cdecl'.
enum class CallConv : unsigned char { Default, CDecl, StdCall };
inline const char *callConvName(CallConv CC) {
  return CC == CallConv::CDecl ? "cdecl" : CC == CallConv::StdCall ? "stdcall" : "";
}

enum class BinaryOp {
  // adding operators (Xor: Turbo/FPC)
  Add, Sub, Or, Xor,
  // multiplying operators (Shl, Shr: Turbo/FPC; As: objfpc)
  Mul, RealDiv, IntDiv, Mod, And, Shl, Shr, As,
  // relational operators (Is: objfpc)
  Eq, Ne, Lt, Le, Gt, Ge, In, Is,
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
  bool ConstRefAddress = false; ///< set by Sema

protected:
  Expr(ExprKind K, SourceRange R) : Kind(K), Range(R) {}

public:
  ExprKind getKind() const { return Kind; }
  SourceRange getRange() const { return Range; }
  SourceLocation getBeginLoc() const { return Range.getBegin(); }

  Type *getType() const { return Ty; }
  void setType(Type *T) { Ty = T; }

  /// An argument for a `constref` parameter that is a variable of the
  /// parameter's type: its address is passed (else a temporary's).
  bool isConstRefAddress() const { return ConstRefAddress; }
  void setConstRefAddress() { ConstRefAddress = true; }

  /// A user-defined conversion (objfpc advanced records): the `:=` or
  /// `Explicit` operator Op converts this expression's value to To, which
  /// becomes its type; getUnconvertedType() is the type it had.
  void setConversion(RoutineDecl *Op, Type *To) {
    Conversion = Op;
    Unconverted = Ty;
    Ty = To;
  }
  RoutineDecl *getConversion() const { return Suspended ? nullptr : Conversion; }
  Type *getUnconvertedType() const { return Unconverted; }
  /// While code generation computes the value to convert: no conversion,
  /// the unconverted type.
  void suspendConversion() {
    std::swap(Ty, Unconverted);
    Suspended = true;
  }
  void resumeConversion() {
    std::swap(Ty, Unconverted);
    Suspended = false;
  }

private:
  RoutineDecl *Conversion = nullptr;
  Type *Unconverted = nullptr;
  bool Suspended = false;
};

/// 42
class IntegerLiteralExpr final : public Expr {
  uint64_t Value;
  bool Based; ///< written $FF, %1010 or &17

public:
  IntegerLiteralExpr(SourceRange R, uint64_t V, bool Based = false)
      : Expr(EK_IntegerLiteral, R), Value(V), Based(Based) {}
  uint64_t getValue() const { return Value; }
  /// Written $FF, %1010 or &17 (Turbo/FPC): beyond int64, the bits of an
  /// int64 (as in Free Pascal), not a qword.
  bool isBased() const { return Based; }
  static bool classof(const Expr *E) { return E->getKind() == EK_IntegerLiteral; }
};

/// 3.14
class RealLiteralExpr final : public Expr {
  double Value;

public:
  RealLiteralExpr(SourceRange R, double V) : Expr(EK_RealLiteral, R), Value(V) {}
  double getValue() const { return Value; }
  static bool classof(const Expr *E) { return E->getKind() == EK_RealLiteral; }
};

/// 'text'. A literal of exactly one character denotes a char value; a lone
/// character code above 255 (#$D800, Turbo/FPC) a widechar value (its
/// Value is then "?", the char it converts to).
class StringLiteralExpr final : public Expr {
  llvm::StringRef Value; ///< '' already unescaped
  unsigned WideCode;     ///< the widechar's code, or 0

public:
  StringLiteralExpr(SourceRange R, llvm::StringRef V, unsigned WideCode = 0)
      : Expr(EK_StringLiteral, R), Value(V), WideCode(WideCode) {}
  llvm::StringRef getValue() const { return Value; }
  bool isCharLiteral() const { return Value.size() == 1 && !WideCode; }
  bool isWideCharLiteral() const { return WideCode != 0; }
  unsigned getWideCode() const { return WideCode; }
  static bool classof(const Expr *E) { return E->getKind() == EK_StringLiteral; }
};

/// A use of a declared name: variable, constant, field (inside `with`),
/// or a parameterless function (a call).
class NameRefExpr final : public Expr {
  Identifier Name;
  Decl *D = nullptr;              ///< set by Sema
  WithContext *With = nullptr;    ///< set by Sema for fields found via `with`

public:
  explicit NameRefExpr(Identifier Name)
      : Expr(EK_NameRef, Name.getRange()), Name(Name) {}
  const Identifier &getName() const { return Name; }
  Decl *getDecl() const { return D; }
  void setDecl(Decl *NewD) { D = NewD; }
  WithContext *getWith() const { return With; }
  void setWith(WithContext *W) { With = W; }
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

/// `move p` (objfpc): the object of the owned variable p, which gives it
/// up (it is nil afterwards, and cannot be used until assigned again).
class MoveExpr final : public Expr {
  NameRefExpr *Operand;

public:
  MoveExpr(SourceRange R, NameRefExpr *Operand)
      : Expr(EK_Move, R), Operand(Operand) {}
  NameRefExpr *getOperand() const { return Operand; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Move; }
};

class UnaryExpr final : public Expr {
  UnaryOp Op;
  Expr *Operand;
  RoutineDecl *Operator = nullptr; ///< a record's class operator (Sema)

public:
  UnaryExpr(SourceRange R, UnaryOp Op, Expr *Operand)
      : Expr(EK_Unary, R), Op(Op), Operand(Operand) {}
  UnaryOp getOp() const { return Op; }
  SourceLocation getOpLoc() const { return getBeginLoc(); }
  Expr *getOperand() const { return Operand; }
  RoutineDecl *getOperator() const { return Operator; }
  void setOperator(RoutineDecl *R) { Operator = R; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Unary; }
};

class BinaryExpr final : public Expr {
  BinaryOp Op;
  SourceLocation OpLoc;
  Expr *LHS, *RHS;
  /// Set by Sema for integer operators and comparisons: the integer type
  /// both operands are converted to before the operation.
  Type *OperandTy = nullptr;

public:
  BinaryExpr(SourceRange R, BinaryOp Op, SourceLocation OpLoc, Expr *LHS,
             Expr *RHS)
      : Expr(EK_Binary, R), Op(Op), OpLoc(OpLoc), LHS(LHS), RHS(RHS) {}
  BinaryOp getOp() const { return Op; }
  SourceLocation getOpLoc() const { return OpLoc; }
  Expr *getLHS() const { return LHS; }
  Expr *getRHS() const { return RHS; }
  Type *getOperandType() const { return OperandTy; }
  void setOperandType(Type *T) { OperandTy = T; }
  /// A record's class operator implementing it (Sema); Negated: `<>`
  /// through the `=` operator (its result negated).
  RoutineDecl *getOperator() const { return Operator; }
  bool isNegated() const { return Negated; }
  void setOperator(RoutineDecl *R, bool Negate) {
    Operator = R;
    Negated = Negate;
  }
  static bool classof(const Expr *E) { return E->getKind() == EK_Binary; }

private:
  RoutineDecl *Operator = nullptr;
  bool Negated = false;
};

/// Function call: abs(x), f(1, 2).
class CallExpr final : public Expr {
  Identifier Callee;
  llvm::ArrayRef<Expr *> Args;
  Decl *D = nullptr; ///< set by Sema: BuiltinDecl or RoutineDecl
  /// Set by Sema for a method found through a class scope (objfpc): the
  /// object it is called on (Self, or the object of a `with`).
  WithContext *With = nullptr;
  Expr *CalleeValue = nullptr; ///< set by Sema: a procedural field or property
  /// specialize F<T1, T2>(args) (objfpc): the type arguments
  llvm::ArrayRef<TypeSpec *> TypeArgs;
  bool VarCast = false; ///< set by Sema: T(x) of an untyped parameter x
  int MethodCast = 0;
  /// The call of a lambda's copy factory, $copyN(k, s): that lambda (see
  /// RoutineDecl::isCopyFactory).
  RoutineDecl *CopyLambda = nullptr;

public:
  RoutineDecl *getCopyLambda() const { return CopyLambda; }
  void setCopyLambda(RoutineDecl *L) { CopyLambda = L; }
  Expr *getCalleeValue() const { return CalleeValue; }
  void setCalleeValue(Expr *V) { CalleeValue = V; }
  llvm::ArrayRef<TypeSpec *> getTypeArgs() const { return TypeArgs; }
  void setTypeArgs(llvm::ArrayRef<TypeSpec *> T) { TypeArgs = T; }
  CallExpr(SourceRange R, Identifier Callee, llvm::ArrayRef<Expr *> Args)
      : Expr(EK_Call, R), Callee(Callee), Args(Args) {}
  /// A typecast of an untyped parameter (Turbo/FPC): a variable of the
  /// type at the parameter's address.
  bool isVarCast() const { return VarCast; }
  /// TMethod(m), m a method pointer: its address when m is a variable.
  bool isMethodCast() const { return MethodCast != 0; }
  bool isMethodCastOfVariable() const { return MethodCast == 2; }
  void setMethodCast(bool OfVariable) { MethodCast = OfVariable ? 2 : 1; }
  void setVarCast() { VarCast = true; }
  const Identifier &getCallee() const { return Callee; }
  llvm::ArrayRef<Expr *> getArgs() const { return Args; }
  void setArgs(llvm::ArrayRef<Expr *> A) { Args = A; }
  Decl *getDecl() const { return D; }
  void setDecl(Decl *NewD) { D = NewD; }
  WithContext *getWith() const { return With; }
  void setWith(WithContext *W) { With = W; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Call; }
};

/// A template string with interpolations, `a ${x} b ${y:8:2} c` (tp and
/// objfpc): an ansistring made of its text and of its values, each as
/// `write` writes it. Parts: text and values in turn, from the first text
/// to the last (StringLiteralExprs, which may be empty); a value with a
/// field width is a FormattedExpr.
class TemplateExpr final : public Expr {
  llvm::ArrayRef<Expr *> Parts;

public:
  TemplateExpr(SourceRange R, llvm::ArrayRef<Expr *> Parts)
      : Expr(EK_Template, R), Parts(Parts) {}
  llvm::ArrayRef<Expr *> getParts() const { return Parts; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Template; }
};

/// Name := Value among the arguments of a call (objfpc): the argument for
/// the parameter Name. Sema puts the arguments of such a call in the order
/// of the parameters, without these nodes.
class NamedArgExpr final : public Expr {
  Identifier Name;
  Expr *Value;

public:
  NamedArgExpr(SourceRange R, Identifier Name, Expr *Value)
      : Expr(EK_NamedArg, R), Name(Name), Value(Value) {}
  const Identifier &getName() const { return Name; }
  Expr *getValue() const { return Value; }
  static bool classof(const Expr *E) { return E->getKind() == EK_NamedArg; }
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

/// a[i]. `a[i, j]` is parsed as `a[i][j]`.
class IndexExpr final : public Expr {
  Expr *Base;
  Expr *Index;
  // objfpc: an array property, `obj.P[i, j]`, `P[i]` (through Self or with)
  // or `obj[i]` (the default property), is the outermost IndexExpr of the
  // chain `((root[i])[j])`; Sema sets the property, the chain's root
  // (obj.P, P or obj) and the indices.
  PropertyDecl *Prop = nullptr;
  Expr *PropRoot = nullptr;
  llvm::ArrayRef<Expr *> PropArgs;

public:
  IndexExpr(SourceRange R, Expr *Base, Expr *Index)
      : Expr(EK_Index, R), Base(Base), Index(Index) {}
  Expr *getBase() const { return Base; }
  Expr *getIndex() const { return Index; }
  PropertyDecl *getProperty() const { return Prop; }
  Expr *getPropertyRoot() const { return PropRoot; }
  llvm::ArrayRef<Expr *> getPropertyArgs() const { return PropArgs; }
  void setProperty(PropertyDecl *P, Expr *Root, llvm::ArrayRef<Expr *> Args) {
    Prop = P;
    PropRoot = Root;
    PropArgs = Args;
  }
  static bool classof(const Expr *E) { return E->getKind() == EK_Index; }
};

/// r.f -- also obj.member (objfpc): a field, method or property of a
/// class, and TClass.Create (a constructor called through its class).
class FieldExpr final : public Expr {
  Expr *Base;
  Identifier Field;
  Decl *Member = nullptr; ///< set by Sema: FieldDecl, RoutineDecl, PropertyDecl
  /// A constant member (TFoo.C, obj.C): Sema's reference to it, which code
  /// generation uses in place of this expression.
  NameRefExpr *ConstRef = nullptr;
  /// A type helper's member used on a value that is not a variable: code
  /// generation passes Self a temporary holding it (set by Sema).
  bool HelperValue = false;

public:
  FieldExpr(SourceRange R, Expr *Base, Identifier Field)
      : Expr(EK_Field, R), Base(Base), Field(Field) {}
  Expr *getBase() const { return Base; }
  const Identifier &getField() const { return Field; }
  FieldDecl *getFieldDecl() const;
  Decl *getMember() const { return Member; }
  void setMember(Decl *D) { Member = D; }
  NameRefExpr *getConstRef() const { return ConstRef; }
  bool isHelperValue() const { return HelperValue; }
  void setHelperValue(bool V) { HelperValue = V; }
  void setConstRef(NameRefExpr *N) { ConstRef = N; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Field; }
};

/// nil
class NilExpr final : public Expr {
public:
  explicit NilExpr(SourceRange R) : Expr(EK_Nil, R) {}
  static bool classof(const Expr *E) { return E->getKind() == EK_Nil; }
};

/// p^ (also the buffer variable f^ of a file, M10)
class DerefExpr final : public Expr {
  Expr *Base;

public:
  DerefExpr(SourceRange R, Expr *Base) : Expr(EK_Deref, R), Base(Base) {}
  Expr *getBase() const { return Base; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Deref; }
};

/// One member of a set constructor: `e` or `lo..hi`.
struct SetElement {
  Expr *Lo;
  Expr *Hi; ///< null unless a range
};

/// [1, 3..5, c]
class SetCtorExpr final : public Expr {
  llvm::ArrayRef<SetElement> Elements;

public:
  SetCtorExpr(SourceRange R, llvm::ArrayRef<SetElement> Elements)
      : Expr(EK_SetCtor, R), Elements(Elements) {}
  llvm::ArrayRef<SetElement> getElements() const { return Elements; }
  static bool classof(const Expr *E) { return E->getKind() == EK_SetCtor; }
};

/// (a, b + 1) (objfpc): a tuple value, or the targets of (a, b) := e.
class TupleExpr final : public Expr {
  llvm::ArrayRef<Expr *> Elements;
  bool LongStrings;        ///< {$H+} here: a string constant is an ansistring
  VarDecl *Temp = nullptr; ///< set by Sema: the hidden variable it is built in

public:
  TupleExpr(SourceRange R, llvm::ArrayRef<Expr *> Elements, bool LongStrings)
      : Expr(EK_Tuple, R), Elements(Elements), LongStrings(LongStrings) {}
  llvm::ArrayRef<Expr *> getElements() const { return Elements; }
  bool hasLongStrings() const { return LongStrings; }
  VarDecl *getTemp() const { return Temp; }
  void setTemp(VarDecl *V) { Temp = V; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Tuple; }
};

class IfStmt;

/// await e (objfpc): waits for task e (in an async routine: suspended);
/// its value is the task's result (raising its exception if it failed).
class AwaitExpr final : public Expr {
  Expr *Operand;

public:
  AwaitExpr(SourceRange R, Expr *Operand) : Expr(EK_Await, R), Operand(Operand) {}
  Expr *getOperand() const { return Operand; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Await; }
};

/// spawn F(args) / spawn begin ... end (objfpc): a child task of the
/// innermost scope, run on the pool; Call is a call of an async routine (a
/// block: of a hidden async procedure the parser made of it). Also
/// Pool.Run(F(args)) (OnPool: on the pool, not a child of a scope).
class SpawnExpr final : public Expr {
  Expr *Call;
  bool OnPool;

public:
  SpawnExpr(SourceRange R, Expr *Call, bool OnPool = false)
      : Expr(EK_Spawn, R), Call(Call), OnPool(OnPool) {}
  Expr *getCall() const { return Call; }
  bool isPoolRun() const { return OnPool; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Spawn; }
};

/// if c then a else b, as an expression (objfpc): a where c holds, else b;
/// only the one chosen is evaluated.
class CondExpr final : public Expr {
  Expr *Cond, *Then, *Else;
  bool LongStrings; ///< {$H+} here: string constants give an ansistring
  /// Set by Sema: `if Cond then tmp := Then else tmp := Else` for a hidden
  /// variable tmp, which code generation runs, and tmp, the value.
  IfStmt *Lowered = nullptr;
  NameRefExpr *Result = nullptr;

public:
  CondExpr(SourceRange R, Expr *Cond, Expr *Then, Expr *Else, bool LongStrings)
      : Expr(EK_Cond, R), Cond(Cond), Then(Then), Else(Else),
        LongStrings(LongStrings) {}
  Expr *getCond() const { return Cond; }
  Expr *getThen() const { return Then; }
  Expr *getElse() const { return Else; }
  bool hasLongStrings() const { return LongStrings; }
  IfStmt *getLowered() const { return Lowered; }
  NameRefExpr *getResult() const { return Result; }
  void setLowered(IfStmt *S, NameRefExpr *R) {
    Lowered = S;
    Result = R;
  }
  static bool classof(const Expr *E) { return E->getKind() == EK_Cond; }
};

/// @x  (Turbo/FPC): an untyped pointer to a variable.
class AddrOfExpr final : public Expr {
  Expr *Operand;
  RoutineDecl *Method = nullptr; ///< set by Sema: @obj.M, a method pointer
  RoutineDecl *Code = nullptr;   ///< set by Sema: @TFoo.M, a code address
  /// A lambda, function(x) => e (objfpc): the hidden routine the parser
  /// made of it, which the operand names; Sema completes it here, where
  /// the type wanted is known.
  RoutineDecl *Lambda = nullptr;

public:
  AddrOfExpr(SourceRange R, Expr *Operand) : Expr(EK_AddrOf, R), Operand(Operand) {}
  Expr *getOperand() const { return Operand; }
  RoutineDecl *getMethod() const { return Method; }
  void setMethod(RoutineDecl *R) { Method = R; }
  RoutineDecl *getCode() const { return Code; }
  void setCode(RoutineDecl *R) { Code = R; }
  RoutineDecl *getLambda() const { return Lambda; }
  void setLambda(RoutineDecl *R) { Lambda = R; }
  static bool classof(const Expr *E) { return E->getKind() == EK_AddrOf; }
};

/// One element of an aggregate: `value` or `field: value`.
struct AggregateElement {
  Identifier Field;   ///< invalid for positional (array) elements
  Expr *Value;        ///< an expression or a nested AggregateExpr
  ConstValue Folded;  ///< set by Sema for expression values
  AggregateElement(Identifier Field, Expr *Value) : Field(Field), Value(Value) {}
};

/// The value of a structured typed constant (Turbo/FPC):
///   array:  (v1, v2, ...)        record:  (f1: v1; f2: v2)
class AggregateExpr final : public Expr {
  llvm::ArrayRef<AggregateElement *> Elements;

public:
  AggregateExpr(SourceRange R, llvm::ArrayRef<AggregateElement *> Elements)
      : Expr(EK_Aggregate, R), Elements(Elements) {}
  llvm::ArrayRef<AggregateElement *> getElements() const { return Elements; }
  bool isRecordForm() const {
    return !Elements.empty() && Elements[0]->Field.isValid();
  }
  static bool classof(const Expr *E) { return E->getKind() == EK_Aggregate; }
};

/// inherited [Name] (objfpc): the method Name (by default the enclosing
/// method's) of the enclosing method's parent class, called on Self without
/// virtual dispatch. With arguments it is the callee of an
/// IndirectCallExpr; a bare `inherited` passes the enclosing method's
/// parameters (ImplicitArgs, made by Sema).
class InheritedExpr final : public Expr {
  Identifier Name;                      ///< invalid for a bare `inherited`
  RoutineDecl *Method = nullptr;        ///< set by Sema (null: nothing to call)
  RoutineDecl *Enclosing = nullptr;     ///< set by Sema: the calling method
  llvm::ArrayRef<Expr *> ImplicitArgs;  ///< set by Sema for a bare `inherited`

public:
  InheritedExpr(SourceRange R, Identifier Name)
      : Expr(EK_Inherited, R), Name(Name) {}
  const Identifier &getName() const { return Name; }
  bool isBare() const { return !Name.isValid(); }
  RoutineDecl *getMethod() const { return Method; }
  void setMethod(RoutineDecl *M) { Method = M; }
  RoutineDecl *getEnclosing() const { return Enclosing; }
  void setEnclosing(RoutineDecl *M) { Enclosing = M; }
  llvm::ArrayRef<Expr *> getImplicitArgs() const { return ImplicitArgs; }
  void setImplicitArgs(llvm::ArrayRef<Expr *> A) { ImplicitArgs = A; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Inherited; }
};

/// A call through a procedural value that is not a plain name (Turbo/FPC):
/// passes[i].run(v). Calls through procedural variables and parameters
/// named directly are CallExpr / ProcCallStmt.
class IndirectCallExpr final : public Expr {
  Expr *Callee;
  llvm::ArrayRef<Expr *> Args;

public:
  IndirectCallExpr(SourceRange R, Expr *Callee, llvm::ArrayRef<Expr *> Args)
      : Expr(EK_IndirectCall, R), Callee(Callee), Args(Args) {}
  Expr *getCallee() const { return Callee; }
  llvm::ArrayRef<Expr *> getArgs() const { return Args; }
  void setArgs(llvm::ArrayRef<Expr *> A) { Args = A; }
  static bool classof(const Expr *E) { return E->getKind() == EK_IndirectCall; }
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

/// writeln('hi', x:5), p(1, y), readln
class ProcCallStmt final : public Stmt {
  Identifier Callee;
  llvm::ArrayRef<Expr *> Args;
  bool HasParens;
  Decl *D = nullptr; ///< set by Sema: BuiltinDecl or RoutineDecl
  WithContext *With = nullptr; ///< set by Sema for methods (see CallExpr)
  Expr *CalleeValue = nullptr; ///< set by Sema: a procedural field or property
  llvm::ArrayRef<TypeSpec *> TypeArgs; ///< specialize P<T>(args) (objfpc)

public:
  Expr *getCalleeValue() const { return CalleeValue; }
  void setCalleeValue(Expr *E) { CalleeValue = E; }
  llvm::ArrayRef<TypeSpec *> getTypeArgs() const { return TypeArgs; }
  void setTypeArgs(llvm::ArrayRef<TypeSpec *> T) { TypeArgs = T; }
  ProcCallStmt(SourceRange R, Identifier Callee, llvm::ArrayRef<Expr *> Args,
               bool HasParens)
      : Stmt(SK_ProcCall, R), Callee(Callee), Args(Args), HasParens(HasParens) {}
  const Identifier &getCallee() const { return Callee; }
  llvm::ArrayRef<Expr *> getArgs() const { return Args; }
  void setArgs(llvm::ArrayRef<Expr *> A) { Args = A; }
  bool hasParens() const { return HasParens; }
  Decl *getDecl() const { return D; }
  void setDecl(Decl *NewD) { D = NewD; }
  WithContext *getWith() const { return With; }
  void setWith(WithContext *W) { With = W; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_ProcCall; }
};

/// target := value  (target: variable, a[i], r.f, or a function's result)
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
  /// Written x += e (-=, *=, /=; Free Pascal's C operators): Value is the
  /// BinaryExpr x + (e), x parsed again.
  bool isCompound() const { return Compound; }
  void setCompound(bool C) { Compound = C; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Assign; }

private:
  bool Compound = false;
};

/// if Cond then Then [else Else]
class IfStmt final : public Stmt {
  Expr *Cond;
  Stmt *Then;
  Stmt *Else; ///< null without an else part

public:
  IfStmt(SourceRange R, Expr *Cond, Stmt *Then, Stmt *Else)
      : Stmt(SK_If, R), Cond(Cond), Then(Then), Else(Else) {}
  Expr *getCond() const { return Cond; }
  Stmt *getThen() const { return Then; }
  Stmt *getElse() const { return Else; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_If; }
};

/// while Cond do Body
class WhileStmt final : public Stmt {
  Expr *Cond;
  Stmt *Body;

public:
  WhileStmt(SourceRange R, Expr *Cond, Stmt *Body)
      : Stmt(SK_While, R), Cond(Cond), Body(Body) {}
  Expr *getCond() const { return Cond; }
  Stmt *getBody() const { return Body; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_While; }
};

/// repeat S1; S2; ... until Cond
class RepeatStmt final : public Stmt {
  llvm::ArrayRef<Stmt *> Body;
  Expr *Cond;

public:
  RepeatStmt(SourceRange R, llvm::ArrayRef<Stmt *> Body, Expr *Cond)
      : Stmt(SK_Repeat, R), Body(Body), Cond(Cond) {}
  llvm::ArrayRef<Stmt *> getBody() const { return Body; }
  Expr *getCond() const { return Cond; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Repeat; }
};

/// for Var := Init to|downto Final do Body
class ForStmt final : public Stmt {
  NameRefExpr *Var;
  VarDecl *InlineVar = nullptr;
  Expr *Init;
  Expr *Final;
  bool Downto;
  Stmt *Body;

public:
  ForStmt(SourceRange R, NameRefExpr *Var, Expr *Init, Expr *Final,
          bool Downto, Stmt *Body)
      : Stmt(SK_For, R), Var(Var), Init(Init), Final(Final), Downto(Downto),
        Body(Body) {}
  NameRefExpr *getVar() const { return Var; }
  /// `for var x ...` (objfpc): the variable the statement declares, visible
  /// in the loop only; without a type it has the type of what it takes.
  VarDecl *getInlineVar() const { return InlineVar; }
  void setInlineVar(VarDecl *V) { InlineVar = V; }
  Expr *getInit() const { return Init; }
  Expr *getFinal() const { return Final; }
  bool isDownto() const { return Downto; }
  Stmt *getBody() const { return Body; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_For; }
};

/// for Var in Collection do Body (Turbo/FPC): Var takes each element of an
/// array, string or set, each value of an ordinal type, or each value an
/// enumerator gives (Collection.GetEnumerator, then MoveNext / Current).
/// Sema classifies the collection and builds the loop out of ordinary
/// nodes over hidden variables, which code generation binds: Step is
/// `Var := <element>` (the element variable, or Enum.Current); for an
/// enumerator, GetEnum is Coll.GetEnumerator and MoveNext Enum.MoveNext.
class ForInStmt final : public Stmt {
public:
  enum InKind : unsigned char {
    Invalid, Array, OpenArray, DynArray, AnsiString, ShortString,
    StringConst, Set, Ordinals, Enumerator
  };

private:
  NameRefExpr *Var;
  VarDecl *InlineVar = nullptr;
  Expr *Collection;
  Stmt *Body;
  SourceLocation InLoc;
  InKind Kind = Invalid;
  Type *ElemTy = nullptr;     ///< not for an enumerator; null: an empty set
  VarDecl *ElemVar = nullptr; ///< the current element
  VarDecl *CollVar = nullptr; ///< an enumerator's collection
  VarDecl *EnumVar = nullptr; ///< the enumerator
  AssignStmt *Step = nullptr;
  Expr *GetEnum = nullptr;
  Expr *MoveNext = nullptr;

public:
  ForInStmt(SourceRange R, NameRefExpr *Var, SourceLocation InLoc,
            Expr *Collection, Stmt *Body)
      : Stmt(SK_ForIn, R), Var(Var), Collection(Collection), Body(Body),
        InLoc(InLoc) {}
  NameRefExpr *getVar() const { return Var; }
  /// `for var x ...` (objfpc): the variable the statement declares, visible
  /// in the loop only; without a type it has the type of what it takes.
  VarDecl *getInlineVar() const { return InlineVar; }
  void setInlineVar(VarDecl *V) { InlineVar = V; }
  Expr *getCollection() const { return Collection; }
  Stmt *getBody() const { return Body; }
  SourceLocation getInLoc() const { return InLoc; }
  InKind getInKind() const { return Kind; }
  Type *getElementType() const { return ElemTy; }
  VarDecl *getElementVar() const { return ElemVar; }
  VarDecl *getCollectionVar() const { return CollVar; }
  VarDecl *getEnumeratorVar() const { return EnumVar; }
  AssignStmt *getStep() const { return Step; }
  Expr *getGetEnumerator() const { return GetEnum; }
  Expr *getMoveNext() const { return MoveNext; }
  void setLowering(InKind K, Type *Elem, VarDecl *ElemV, AssignStmt *S) {
    Kind = K;
    ElemTy = Elem;
    ElemVar = ElemV;
    Step = S;
  }
  void setEnumerator(VarDecl *Coll, VarDecl *Enum, Expr *Get, Expr *Next,
                     AssignStmt *S) {
    Kind = Enumerator;
    CollVar = Coll;
    EnumVar = Enum;
    GetEnum = Get;
    MoveNext = Next;
    Step = S;
  }
  static bool classof(const Stmt *S) { return S->getKind() == SK_ForIn; }
};

/// One case label (also used for record variant labels): a constant, or
/// (--mode=tp in case statements) a range `Lo..Hi`. Sema evaluates the
/// constants into LoValue/HiValue.
struct CaseLabel {
  Expr *Lo;
  Expr *Hi; ///< null unless a range
  int64_t LoValue = 0;
  int64_t HiValue = 0;

  CaseLabel(Expr *Lo, Expr *Hi) : Lo(Lo), Hi(Hi) {}
  bool isRange() const { return Hi != nullptr; }
  SourceRange getRange() const {
    return {Lo->getBeginLoc(), (Hi ? Hi : Lo)->getRange().getEnd()};
  }
};

/// `1, 3..5: Body` inside a case statement.
class CaseArm {
  llvm::ArrayRef<CaseLabel *> Labels;
  Stmt *Body;

public:
  CaseArm(llvm::ArrayRef<CaseLabel *> Labels, Stmt *Body)
      : Labels(Labels), Body(Body) {}
  llvm::ArrayRef<CaseLabel *> getLabels() const { return Labels; }
  Stmt *getBody() const { return Body; }
};

/// case Selector of Arms [else ElseBody] end
/// (`else`/`otherwise` is a Turbo/FPC extension.)
class CaseStmt final : public Stmt {
  Expr *Selector;
  llvm::ArrayRef<CaseArm *> Arms;
  llvm::ArrayRef<Stmt *> ElseBody;
  SourceLocation ElseLoc; ///< valid iff there is an else part

public:
  CaseStmt(SourceRange R, Expr *Selector, llvm::ArrayRef<CaseArm *> Arms,
           SourceLocation ElseLoc, llvm::ArrayRef<Stmt *> ElseBody)
      : Stmt(SK_Case, R), Selector(Selector), Arms(Arms), ElseBody(ElseBody),
        ElseLoc(ElseLoc) {}
  Expr *getSelector() const { return Selector; }
  llvm::ArrayRef<CaseArm *> getArms() const { return Arms; }
  bool hasElse() const { return ElseLoc.isValid(); }
  SourceLocation getElseLoc() const { return ElseLoc; }
  llvm::ArrayRef<Stmt *> getElseBody() const { return ElseBody; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Case; }
};

/// One record variable of a with statement. Field names found through it
/// point here (NameRefExpr::getWith), and codegen evaluates the record's
/// address once per with statement. In objfpc the "record" can be an object
/// (a value of a class type, evaluated once), and every method body has an
/// implicit context whose Record is `Self` (IsSelf), through which the
/// members of its class are found.
struct WithContext {
  Expr *Record;
  bool IsSelf = false;
  /// The routine whose `with` statement (or whose Self) this is (Sema).
  RoutineDecl *Routine = nullptr;
  ClassType *Helper = nullptr; ///< Self in a method of this type helper
  explicit WithContext(Expr *Record, bool IsSelf = false)
      : Record(Record), IsSelf(IsSelf) {}
};

/// with r1, r2 do Body   ==   with r1 do with r2 do Body
class WithStmt final : public Stmt {
  llvm::ArrayRef<WithContext *> Records;
  Stmt *Body;

public:
  WithStmt(SourceRange R, llvm::ArrayRef<WithContext *> Records, Stmt *Body)
      : Stmt(SK_With, R), Records(Records), Body(Body) {}
  llvm::ArrayRef<WithContext *> getRecords() const { return Records; }
  Stmt *getBody() const { return Body; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_With; }
};

/// A procedure call through a procedural value that is not a plain name:
/// `handlers[k](x);`, `r.p;` (Turbo/FPC). Call holds the callee and
/// arguments.
class IndirectCallStmt final : public Stmt {
  IndirectCallExpr *Call;

public:
  IndirectCallStmt(SourceRange R, IndirectCallExpr *Call)
      : Stmt(SK_IndirectCall, R), Call(Call) {}
  IndirectCallExpr *getCall() const { return Call; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_IndirectCall; }
};

/// 10: Body
class LabeledStmt final : public Stmt {
  Identifier Label;
  Stmt *Body;
  LabelDecl *D = nullptr; ///< set by Sema

public:
  LabeledStmt(SourceRange R, Identifier Label, Stmt *Body)
      : Stmt(SK_Labeled, R), Label(Label), Body(Body) {}
  const Identifier &getLabel() const { return Label; }
  Stmt *getBody() const { return Body; }
  LabelDecl *getDecl() const { return D; }
  void setDecl(LabelDecl *L) { D = L; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Labeled; }
};

/// goto 10
class GotoStmt final : public Stmt {
  Identifier Label;
  LabelDecl *D = nullptr; ///< set by Sema

public:
  GotoStmt(SourceRange R, Identifier Label) : Stmt(SK_Goto, R), Label(Label) {}
  const Identifier &getLabel() const { return Label; }
  LabelDecl *getDecl() const { return D; }
  void setDecl(LabelDecl *L) { D = L; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Goto; }
};

/// `on [Name:] Class do Body` in the except part of a try statement
/// (objfpc). Name, if given, is a variable of the class type visible in
/// Body only (Sema creates Var).
class ExceptHandler {
  Identifier VarName; ///< invalid for `on TFoo do`
  Identifier ClassName;
  Stmt *Body;
  SourceLocation OnLoc;
  VarDecl *Var = nullptr;     ///< set by Sema
  ClassType *Class = nullptr; ///< set by Sema

public:
  ExceptHandler(SourceLocation OnLoc, Identifier VarName, Identifier ClassName,
                Stmt *Body)
      : VarName(VarName), ClassName(ClassName), Body(Body), OnLoc(OnLoc) {}
  SourceLocation getOnLoc() const { return OnLoc; }
  const Identifier &getVarName() const { return VarName; }
  const Identifier &getClassName() const { return ClassName; }
  Stmt *getBody() const { return Body; }
  VarDecl *getVar() const { return Var; }
  void setVar(VarDecl *V) { Var = V; }
  ClassType *getClass() const { return Class; }
  void setClass(ClassType *C) { Class = C; }
};

/// try Body except [Handlers [else ExceptBody] | ExceptBody] end
/// try Body finally FinallyBody end                          (objfpc)
/// Without handlers, ExceptBody handles every exception; with handlers,
/// it is the else part (hasElse) for exceptions no handler matches.
class TryStmt final : public Stmt {
  llvm::ArrayRef<Stmt *> Body;
  bool IsFinally;
  SourceLocation PartLoc; ///< `except` or `finally`
  llvm::ArrayRef<ExceptHandler *> Handlers;
  SourceLocation ElseLoc; ///< valid iff handlers are followed by else
  llvm::ArrayRef<Stmt *> HandlerBody; ///< except: ExceptBody; finally: FinallyBody

public:
  TryStmt(SourceRange R, llvm::ArrayRef<Stmt *> Body, bool IsFinally,
          SourceLocation PartLoc, llvm::ArrayRef<ExceptHandler *> Handlers,
          SourceLocation ElseLoc, llvm::ArrayRef<Stmt *> HandlerBody)
      : Stmt(SK_Try, R), Body(Body), IsFinally(IsFinally), PartLoc(PartLoc),
        Handlers(Handlers), ElseLoc(ElseLoc), HandlerBody(HandlerBody) {}
  llvm::ArrayRef<Stmt *> getBody() const { return Body; }
  bool isFinally() const { return IsFinally; }
  SourceLocation getPartLoc() const { return PartLoc; }
  llvm::ArrayRef<ExceptHandler *> getHandlers() const { return Handlers; }
  bool hasElse() const { return ElseLoc.isValid(); }
  SourceLocation getElseLoc() const { return ElseLoc; }
  /// The except part (no handlers), the else part, or the finally part.
  llvm::ArrayRef<Stmt *> getExceptBody() const { return HandlerBody; }
  llvm::ArrayRef<Stmt *> getFinallyBody() const { return HandlerBody; }
  /// Does the except part catch every exception (no handlers, or else)?
  bool catchesAll() const { return Handlers.empty() || hasElse(); }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Try; }
};

/// raise Object [at Address [, Frame]], or a bare `raise` (re-raise, in an
/// exception handler)
class RaiseStmt final : public Stmt {
  Expr *Object;  ///< null for a re-raise
  Expr *Address; ///< `at` Address (a pointer), or null
  Expr *Frame;   ///< `at` Address, Frame, or null

public:
  RaiseStmt(SourceRange R, Expr *Object, Expr *Address = nullptr,
            Expr *Frame = nullptr)
      : Stmt(SK_Raise, R), Object(Object), Address(Address), Frame(Frame) {}
  Expr *getObject() const { return Object; }
  Expr *getAddress() const { return Address; }
  Expr *getFrame() const { return Frame; }
  bool isReraise() const { return Object == nullptr; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Raise; }
};

class VarDecl;

/// A variable declared among the statements (objfpc, as in Delphi):
///   var x: T;   var x, y: T;   var x: T := e;   var x := e;
/// visible to the end of the statement sequence it is in. Without a type,
/// the variable has the type of e. It is set (to e, or to zero) each time
/// the statement runs.
class VarDeclStmt final : public Stmt {
  llvm::ArrayRef<VarDecl *> Vars;
  Expr *Init;              ///< null: none
  SourceLocation AssignLoc;
  bool LongStrings;        ///< {$H+} here: a string constant gives an ansistring
  AssignStmt *InitAssign = nullptr; ///< set by Sema: x := e
  bool Const = false;      ///< const x := e: x cannot be changed afterwards

public:
  VarDeclStmt(SourceRange R, llvm::ArrayRef<VarDecl *> Vars, Expr *Init,
              SourceLocation AssignLoc, bool LongStrings)
      : Stmt(SK_VarDecl, R), Vars(Vars), Init(Init), AssignLoc(AssignLoc),
        LongStrings(LongStrings) {}
  llvm::ArrayRef<VarDecl *> getVars() const { return Vars; }
  Expr *getInit() const { return Init; }
  SourceLocation getAssignLoc() const { return AssignLoc; }
  bool hasLongStrings() const { return LongStrings; }
  AssignStmt *getInitAssign() const { return InitAssign; }
  void setInitAssign(AssignStmt *A) { InitAssign = A; }
  bool isConst() const { return Const; }
  void setConst() { Const = true; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_VarDecl; }
};

/// An expression used for its effect (objfpc): `await e;`, `spawn ...;`.
class ExprStmt final : public Stmt {
  Expr *E;

public:
  ExprStmt(SourceRange R, Expr *E) : Stmt(SK_Expr, R), E(E) {}
  Expr *getExpr() const { return E; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Expr; }
};

/// scope [token] statements end (objfpc): the spawned tasks are its
/// children; it ends when they all have, cancelling the others if one
/// fails (then raising its exception), or when the token is cancelled.
class ScopeStmt final : public Stmt {
  Expr *Token; ///< null: none
  llvm::ArrayRef<Stmt *> Body;

public:
  ScopeStmt(SourceRange R, Expr *Token, llvm::ArrayRef<Stmt *> Body)
      : Stmt(SK_Scope, R), Token(Token), Body(Body) {}
  Expr *getToken() const { return Token; }
  llvm::ArrayRef<Stmt *> getBody() const { return Body; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Scope; }
};

/// detached [(handler)] call (objfpc): an async call left to run on its
/// own; its exception, if any, goes to handler (procedure(E: Exception)).
class DetachedStmt final : public Stmt {
  Expr *Handler; ///< null: none
  Expr *Call;

public:
  DetachedStmt(SourceRange R, Expr *Handler, Expr *Call)
      : Stmt(SK_Detached, R), Handler(Handler), Call(Call) {}
  Expr *getHandler() const { return Handler; }
  Expr *getCall() const { return Call; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Detached; }
};

/// (a, b) := e;  var (a, b) := e;  (objfpc): the elements of a tuple
/// assigned to variables, or to new ones.
class DestructureStmt final : public Stmt {
  llvm::ArrayRef<Expr *> Targets; ///< (a, b) := e; set by Sema for var (a, b)
  llvm::ArrayRef<VarDecl *> Vars; ///< var (a, b) := e; else empty
  Expr *Value;
  SourceLocation AssignLoc;
  bool LongStrings; ///< {$H+} here
  /// Set by Sema: the value kept in a hidden variable (null: Value is a
  /// tuple written out, built in its own), then each target assigned.
  AssignStmt *Init = nullptr;
  llvm::ArrayRef<AssignStmt *> Assigns;

public:
  DestructureStmt(SourceRange R, llvm::ArrayRef<Expr *> Targets,
                  llvm::ArrayRef<VarDecl *> Vars, Expr *Value,
                  SourceLocation AssignLoc, bool LongStrings)
      : Stmt(SK_Destructure, R), Targets(Targets), Vars(Vars), Value(Value),
        AssignLoc(AssignLoc), LongStrings(LongStrings) {}
  llvm::ArrayRef<Expr *> getTargets() const { return Targets; }
  void setTargets(llvm::ArrayRef<Expr *> T) { Targets = T; }
  llvm::ArrayRef<VarDecl *> getVars() const { return Vars; }
  Expr *getValue() const { return Value; }
  SourceLocation getAssignLoc() const { return AssignLoc; }
  bool hasLongStrings() const { return LongStrings; }
  AssignStmt *getInit() const { return Init; }
  void setInit(AssignStmt *A) { Init = A; }
  llvm::ArrayRef<AssignStmt *> getAssigns() const { return Assigns; }
  void setAssigns(llvm::ArrayRef<AssignStmt *> A) { Assigns = A; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_Destructure; }
};

//===----------------------------------------------------------------------===//
// Written types
//===----------------------------------------------------------------------===//

class TypeSpec {
public:
  enum TypeSpecKind {
    TS_Named, TS_Enum, TS_Subrange, TS_Array, TS_Record, TS_Pointer, TS_Set,
    TS_File, TS_Routine, TS_String, TS_Class, TS_ClassRef, TS_Interface,
    TS_Helper,
    TS_Untyped,
    TS_ArrayOfConst,
    TS_OpenArray,
    TS_DynArray,
    TS_Tuple,
    TS_Specialize,
    TS_Async
  };

private:
  const TypeSpecKind Kind;
  SourceRange Range;
  Type *Resolved = nullptr; ///< set by Sema (shared by `a, b: T`)

protected:
  TypeSpec(TypeSpecKind K, SourceRange R) : Kind(K), Range(R) {}

public:
  TypeSpecKind getKind() const { return Kind; }
  SourceRange getRange() const { return Range; }
  SourceLocation getBeginLoc() const { return Range.getBegin(); }
  Type *getResolved() const { return Resolved; }
  void setResolved(Type *T) { Resolved = T; }
};

/// A type identifier: integer, char, MyType.
class NamedTypeSpec final : public TypeSpec {
  Identifier Name;
  /// TOuter.TInner (objfpc): the enclosing classes or records, outermost
  /// first; Name is the last name.
  llvm::ArrayRef<Identifier> Qualifiers;

public:
  explicit NamedTypeSpec(Identifier Name)
      : TypeSpec(TS_Named, Name.getRange()), Name(Name) {}
  NamedTypeSpec(SourceRange R, llvm::ArrayRef<Identifier> Qualifiers,
                Identifier Name)
      : TypeSpec(TS_Named, R), Name(Name), Qualifiers(Qualifiers) {}
  const Identifier &getName() const { return Name; }
  llvm::ArrayRef<Identifier> getQualifiers() const { return Qualifiers; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Named; }
};

/// One element of a tuple type: [Name:] Type.
struct TupleElementSpec {
  Identifier Name; ///< invalid: none (it is Item1, Item2, ...)
  TypeSpec *Type;
};

/// (integer, string) / (Min: integer, Max: integer)  (objfpc)
class TupleTypeSpec final : public TypeSpec {
  llvm::ArrayRef<TupleElementSpec> Elements;

public:
  TupleTypeSpec(SourceRange R, llvm::ArrayRef<TupleElementSpec> Elements)
      : TypeSpec(TS_Tuple, R), Elements(Elements) {}
  llvm::ArrayRef<TupleElementSpec> getElements() const { return Elements; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Tuple; }
};

/// (red, green, blue)
class EnumTypeSpec final : public TypeSpec {
  llvm::ArrayRef<Identifier> Names;
  bool Scoped; ///< {$scopedenums on}: the values only as TColor.Red
  /// objfpc, two names or more: the tuple type this is if every name is
  /// the name of a type (Sema decides); not printed.
  TupleTypeSpec *Tuple = nullptr;

public:
  EnumTypeSpec(SourceRange R, llvm::ArrayRef<Identifier> Names,
               bool Scoped = false)
      : TypeSpec(TS_Enum, R), Names(Names), Scoped(Scoped) {}
  llvm::ArrayRef<Identifier> getNames() const { return Names; }
  bool isScoped() const { return Scoped; }
  TupleTypeSpec *getTuple() const { return Tuple; }
  void setTuple(TupleTypeSpec *T) { Tuple = T; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Enum; }
};

/// Lo..Hi
class SubrangeTypeSpec final : public TypeSpec {
  Expr *Lo, *Hi;

public:
  SubrangeTypeSpec(SourceRange R, Expr *Lo, Expr *Hi)
      : TypeSpec(TS_Subrange, R), Lo(Lo), Hi(Hi) {}
  Expr *getLo() const { return Lo; }
  Expr *getHi() const { return Hi; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Subrange; }
};

/// [packed] array [I1, I2] of Elem
class ArrayTypeSpec final : public TypeSpec {
  llvm::ArrayRef<TypeSpec *> Indexes;
  TypeSpec *Element;
  bool Packed;
  bool BitPacked = false; ///< bitpacked (Free Pascal)

public:
  ArrayTypeSpec(SourceRange R, llvm::ArrayRef<TypeSpec *> Indexes,
                TypeSpec *Element, bool Packed)
      : TypeSpec(TS_Array, R), Indexes(Indexes), Element(Element),
        Packed(Packed) {}
  llvm::ArrayRef<TypeSpec *> getIndexes() const { return Indexes; }
  TypeSpec *getElement() const { return Element; }
  bool isPacked() const { return Packed; }
  bool isBitPacked() const { return BitPacked; }
  void setBitPacked() { BitPacked = true; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Array; }
};

struct VariantPart;

/// Fields of a record (or of one record variant).
struct FieldList {
  llvm::ArrayRef<FieldDecl *> Fixed;
  VariantPart *Variant = nullptr;
};

/// One variant: `1, 2: (x, y: integer)`.
struct Variant {
  llvm::ArrayRef<CaseLabel *> Labels;
  FieldList *Fields;
};

/// case [tag:] TagType of Variants
struct VariantPart {
  FieldDecl *Tag = nullptr;             ///< the tag field, if named
  TypeSpec *TagTypeSpec = nullptr;      ///< always a type identifier
  Type *TagType = nullptr;              ///< set by Sema
  llvm::ArrayRef<Variant *> Variants;
  SourceLocation CaseLoc;
};

/// [packed] record FieldList end
class RecordTypeSpec final : public TypeSpec {
  FieldList *Fields;
  bool Packed;
  bool BitPacked = false; ///< bitpacked (Free Pascal)
  unsigned PackRecords = 0;
  /// Advanced records (objfpc, {$modeswitch advancedrecords}): methods,
  /// class operators and properties, in source order.
  llvm::ArrayRef<Decl *> Members;

public:
  RecordTypeSpec(SourceRange R, FieldList *Fields, bool Packed)
      : TypeSpec(TS_Record, R), Fields(Fields), Packed(Packed) {}
  FieldList *getFields() const { return Fields; }
  bool isPacked() const { return Packed; }
  bool isBitPacked() const { return BitPacked; }
  void setBitPacked() { BitPacked = true; }
  /// {$packrecords N} where it was declared (0: the natural layout).
  unsigned getPackRecords() const { return PackRecords; }
  void setPackRecords(unsigned N) { PackRecords = N; }
  llvm::ArrayRef<Decl *> getMembers() const { return Members; }
  void setMembers(llvm::ArrayRef<Decl *> M) { Members = M; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Record; }
};

/// ^TypeName
class PointerTypeSpec final : public TypeSpec {
  Identifier Target;

public:
  PointerTypeSpec(SourceRange R, Identifier Target)
      : TypeSpec(TS_Pointer, R), Target(Target) {}
  const Identifier &getTarget() const { return Target; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Pointer; }
};

class VarDecl;

/// The heading of a procedural / functional parameter:
/// `procedure p(x: integer)` or `function f(x: real): real`.
class RoutineTypeSpec final : public TypeSpec {
  bool OfObject = false; ///< `of object`
  bool Reference = false; ///< `reference to` (objfpc): a closure
  bool IsFunction;
  llvm::ArrayRef<VarDecl *> Params;
  TypeSpec *Result; ///< functions only
  CallConv CC = CallConv::Default;

public:
  bool isOfObject() const { return OfObject; }
  void setOfObject() { OfObject = true; }
  bool isReference() const { return Reference; }
  void setReference() { Reference = true; }
  RoutineTypeSpec(SourceRange R, bool IsFunction,
                  llvm::ArrayRef<VarDecl *> Params, TypeSpec *Result)
      : TypeSpec(TS_Routine, R), IsFunction(IsFunction), Params(Params),
        Result(Result) {}
  CallConv getCallConv() const { return CC; }
  void setCallConv(CallConv C) { CC = C; }
  bool isFunction() const { return IsFunction; }
  llvm::ArrayRef<VarDecl *> getParams() const { return Params; }
  TypeSpec *getResult() const { return Result; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Routine; }
};

/// string [ '[' MaxLen ']' ]  (Turbo/FPC)
class StringTypeSpec final : public TypeSpec {
  Expr *MaxLen; ///< null for plain `string`
  bool Long;    ///< plain `string` under {$H+}: an ansistring

public:
  StringTypeSpec(SourceRange R, Expr *MaxLen, bool Long = false)
      : TypeSpec(TS_String, R), MaxLen(MaxLen), Long(Long) {}
  Expr *getMaxLength() const { return MaxLen; }
  bool isLong() const { return Long; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_String; }
};

/// `array of T` outside a parameter list: a dynamic array (Turbo/FPC).
class DynArrayTypeSpec final : public TypeSpec {
  TypeSpec *Element;

public:
  DynArrayTypeSpec(SourceRange R, TypeSpec *Element)
      : TypeSpec(TS_DynArray, R), Element(Element) {}
  TypeSpec *getElement() const { return Element; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_DynArray; }
};

/// specialize TList<integer> (objfpc): the type made of the generic type
/// TList for these type arguments.
class SpecializeTypeSpec final : public TypeSpec {
  Identifier Name;
  llvm::ArrayRef<TypeSpec *> Args;

public:
  SpecializeTypeSpec(SourceRange R, Identifier Name,
                     llvm::ArrayRef<TypeSpec *> Args)
      : TypeSpec(TS_Specialize, R), Name(Name), Args(Args) {}
  const Identifier &getName() const { return Name; }
  llvm::ArrayRef<TypeSpec *> getArgs() const { return Args; }
  static bool classof(const TypeSpec *T) {
    return T->getKind() == TS_Specialize;
  }
};

/// async T / async (objfpc): a task type; Inner null for async.
class AsyncTypeSpec final : public TypeSpec {
  TypeSpec *Inner;

public:
  AsyncTypeSpec(SourceRange R, TypeSpec *Inner)
      : TypeSpec(TS_Async, R), Inner(Inner) {}
  TypeSpec *getInner() const { return Inner; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Async; }
};

/// A parameter's `array of T`.
class OpenArrayTypeSpec final : public TypeSpec {
  TypeSpec *Element;

public:
  OpenArrayTypeSpec(SourceRange R, TypeSpec *Element)
      : TypeSpec(TS_OpenArray, R), Element(Element) {}
  TypeSpec *getElement() const { return Element; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_OpenArray; }
};

/// A parameter's `array of const`.
class ArrayOfConstTypeSpec final : public TypeSpec {
public:
  explicit ArrayOfConstTypeSpec(SourceRange R) : TypeSpec(TS_ArrayOfConst, R) {}
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_ArrayOfConst; }
};

/// The (absent) type of an untyped parameter:  var x;  const x  (Turbo/FPC)
class UntypedTypeSpec final : public TypeSpec {
public:
  explicit UntypedTypeSpec(SourceRange R) : TypeSpec(TS_Untyped, R) {}
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Untyped; }
};

/// [packed] file of Element
class FileTypeSpec final : public TypeSpec {
  TypeSpec *Element;

public:
  FileTypeSpec(SourceRange R, TypeSpec *Element)
      : TypeSpec(TS_File, R), Element(Element) {}
  TypeSpec *getElement() const { return Element; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_File; }
};

/// [packed] set of Element
class SetTypeSpec final : public TypeSpec {
  TypeSpec *Element;

public:
  SetTypeSpec(SourceRange R, TypeSpec *Element)
      : TypeSpec(TS_Set, R), Element(Element) {}
  TypeSpec *getElement() const { return Element; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Set; }
};

/// class [(Parent)] members end   (objfpc); `class;` declares a class
/// forward. Members are FieldDecls, RoutineDecls (method headings) and
/// PropertyDecls in source order, each with its visibility.
class ClassTypeSpec final : public TypeSpec {
  Identifier Parent; ///< invalid: TObject (an interface: TObject too)
  llvm::ArrayRef<Identifier> Interfaces; ///< the rest of class(P, I1, I2)
  llvm::ArrayRef<Decl *> Members;
  bool Forward;

public:
  ClassTypeSpec(SourceRange R, Identifier Parent, llvm::ArrayRef<Decl *> Members,
                bool Forward)
      : TypeSpec(TS_Class, R), Parent(Parent), Members(Members),
        Forward(Forward) {}
  const Identifier &getParent() const { return Parent; }
  llvm::ArrayRef<Identifier> getInterfaces() const { return Interfaces; }
  void setInterfaces(llvm::ArrayRef<Identifier> I) { Interfaces = I; }
  llvm::ArrayRef<Decl *> getMembers() const { return Members; }
  bool isForward() const { return Forward; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Class; }
};

/// type helper [(Parent)] for Extended members end  (objfpc,
/// {$modeswitch typehelpers}): methods (Self: the extended value, by
/// reference), static class methods, properties and constants for a type.
/// record helper [(Parent)] for Extended ... end  (objfpc, {$modeswitch
/// advancedrecords}): the same, for a record type.
class HelperTypeSpec final : public TypeSpec {
  Identifier Parent; ///< invalid: none
  TypeSpec *Extended;
  llvm::ArrayRef<Decl *> Members;
  bool Record; ///< a record helper

public:
  HelperTypeSpec(SourceRange R, Identifier Parent, TypeSpec *Extended,
                 llvm::ArrayRef<Decl *> Members, bool Record)
      : TypeSpec(TS_Helper, R), Parent(Parent), Extended(Extended),
        Members(Members), Record(Record) {}
  bool isRecordHelper() const { return Record; }
  const Identifier &getParent() const { return Parent; }
  TypeSpec *getExtended() const { return Extended; }
  llvm::ArrayRef<Decl *> getMembers() const { return Members; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Helper; }
};

/// interface [(Parent)] ['GUID'] method headings end  (objfpc, CORBA
/// style: no reference counting). `IFoo = interface;` is a forward
/// declaration.
class InterfaceTypeSpec final : public TypeSpec {
  Identifier Parent; ///< invalid: none
  llvm::ArrayRef<Decl *> Members;
  bool Forward;
  bool Corba;           ///< declared under {$interfaces corba}
  llvm::StringRef GUID; ///< ['...'] (no effect)

public:
  InterfaceTypeSpec(SourceRange R, Identifier Parent,
                    llvm::ArrayRef<Decl *> Members, bool Forward, bool Corba,
                    llvm::StringRef GUID)
      : TypeSpec(TS_Interface, R), Parent(Parent), Members(Members),
        Forward(Forward), Corba(Corba), GUID(GUID) {}
  const Identifier &getParent() const { return Parent; }
  llvm::ArrayRef<Decl *> getMembers() const { return Members; }
  bool isForward() const { return Forward; }
  bool isCorba() const { return Corba; }
  llvm::StringRef getGUID() const { return GUID; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Interface; }
};

/// class of TFoo  (objfpc): a class reference type.
class ClassRefTypeSpec final : public TypeSpec {
  Identifier ClassName;

public:
  ClassRefTypeSpec(SourceRange R, Identifier ClassName)
      : TypeSpec(TS_ClassRef, R), ClassName(ClassName) {}
  const Identifier &getClassName() const { return ClassName; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_ClassRef; }
};

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

/// Visibility of a class member (objfpc). Records' fields are public.
/// Strict private members are visible only in their class's methods, strict
/// protected ones also in its descendants' (not in the rest of the module).
enum class Visibility : unsigned char {
  Public, Published, Protected, Private, StrictProtected, StrictPrivate
};

class Decl {
public:
  enum DeclKind {
    DK_Program, DK_Var, DK_Const, DK_Type, DK_Field, DK_Routine, DK_Builtin,
    DK_Label, DK_Unit, DK_Property
  };

private:
  const DeclKind Kind;
  Identifier Name;
  SourceLocation Loc; ///< invalid for predeclared entities
  Visibility Vis = Visibility::Public;

protected:
  Decl(DeclKind K, SourceLocation Loc, Identifier Name)
      : Kind(K), Name(Name), Loc(Loc) {}

public:
  DeclKind getKind() const { return Kind; }
  const Identifier &getName() const { return Name; }
  SourceLocation getLocation() const { return Loc; }
  bool isPredeclared() const { return !Loc.isValid(); }
  Visibility getVisibility() const { return Vis; }
  void setVisibility(Visibility V) { Vis = V; }
  /// A specialization of a generic routine is renamed (F<integer>).
  void setName(Identifier N) { Name = N; }
  /// What this entity is, for diagnostics: "variable", "type", ...
  const char *getKindName() const;

  /// Free Pascal's hint directives (deprecated ['message'], experimental,
  /// platform, library, unimplemented): each use of the entity is warned
  /// about.
  enum HintFlag : unsigned char {
    HintDeprecated = 1,
    HintExperimental = 2,
    HintPlatform = 4,
    HintLibrary = 8,
    HintUnimplemented = 16
  };
  unsigned char getHints() const { return Hints; }
  llvm::StringRef getHintMessage() const { return HintMessage; }
  void addHints(unsigned char Flags, llvm::StringRef Message) {
    Hints |= Flags;
    if (!Message.empty())
      HintMessage = Message;
  }

private:
  unsigned char Hints = 0;
  llvm::StringRef HintMessage; ///< deprecated's message, if any
};

/// A variable, a formal parameter, or a function's result slot.
class VarDecl final : public Decl {
public:
  /// ConstParam (Turbo/FPC `const x: T`): read-only; strings and structured
  /// values are passed by reference.
  enum VarKind { Variable, ValueParam, VarParam, ConstParam, Result };

private:
  TypeSpec *TS;
  Type *Ty = nullptr;           ///< set by Sema
  VarKind VK;
  RoutineDecl *Owner = nullptr; ///< null for program-level variables
  /// Turbo/FPC typed constant `const x: T = v;`: an initialized variable
  /// with static storage (even inside a routine).
  Expr *Init = nullptr;
  bool InitializedVar = false;
  ConstValue InitValue;         ///< set by Sema
  /// objfpc `class var`: one static variable for the class and its
  /// descendants (VarClass: set by Sema).
  bool IsClassVar = false;
  ClassType *VarClass = nullptr;
  bool Captured = false;        ///< set by Sema
  bool Owned = false;           ///< objfpc `x: TFoo owned`
  bool Inline = false;          ///< objfpc: declared by a `var` statement
  bool ReadOnly = false;      ///< see isReadOnly
  Expr *Absolute = nullptr;     ///< Turbo/FPC `x: T absolute y`: y
  Expr *Default = nullptr;      ///< objfpc: the parameter's default value
  bool Out = false;             ///< objfpc: an `out` parameter
  bool ConstRef = false;        ///< Turbo/FPC: a `constref` parameter
  VarDecl *AbsoluteRoot = nullptr; ///< set by Sema: the variable y is in
  /// Turbo/FPC `x: T; [cvar;] external ['lib'] [name 'sym'];`: a C variable
  /// (the library and symbol: string constants, evaluated by Sema; the
  /// symbol defaults to the name as written).
  bool External = false, CVar = false;
  llvm::StringRef ExternalName, LibraryName;
  Expr *LibraryExpr = nullptr, *NameExpr = nullptr;

public:
  VarDecl(Identifier Name, TypeSpec *TS, VarKind VK = Variable)
      : Decl(DK_Var, Name.Loc, Name), TS(TS), VK(VK) {}
  TypeSpec *getTypeSpec() const { return TS; }
  Type *getType() const { return Ty; }
  void setType(Type *T) { Ty = T; }
  VarKind getVarKind() const { return VK; }
  /// (A lambda's parameter without a type: the kind of the one wanted.)
  void setVarKind(VarKind K) { VK = K; }
  bool isParam() const {
    return VK == ValueParam || VK == VarParam || VK == ConstParam;
  }
  bool isVarParam() const { return VK == VarParam; }
  bool isConstParam() const { return VK == ConstParam; }
  /// An `out` parameter (objfpc) is a var parameter (isVarParam) whose
  /// managed values the caller releases before the call.
  bool isOutParam() const { return Out; }
  void setOutParam() { Out = true; }
  /// A `constref` parameter (Turbo/FPC) is a const parameter (isConstParam)
  /// always passed by reference: the caller's variable, or a temporary.
  bool isConstRefParam() const { return ConstRef; }
  void setConstRefParam() { ConstRef = true; }
  RoutineDecl *getOwner() const { return Owner; }
  void setOwner(RoutineDecl *R) { Owner = R; }
  /// A routine's variable that a routine nested in it may use: it lives in
  /// the routine's frame record, which nested routines reach through
  /// their static link.
  bool isCaptured() const { return Captured; }
  void setCaptured() { Captured = true; }
  /// `x: TFoo owned` (objfpc): a routine's variable that owns its object,
  /// which is freed when the variable gets another one and when the
  /// routine ends, however it ends; `move x` gives the object up.
  bool isOwned() const { return Owned; }
  void setOwned(bool O = true) { Owned = O; }
  /// Declared among the statements (a VarDeclStmt's): its scope is the
  /// rest of the statement sequence. Sema adds it to its block's
  /// declarations, after the others.
  bool isInline() const { return Inline; }
  void setInline() { Inline = true; }
  /// `const x := e` among the statements (objfpc): set once, where it is
  /// declared, and not changed afterwards (Sema).
  bool isReadOnly() const { return ReadOnly; }
  void setReadOnly() { ReadOnly = true; }
  /// `x: T absolute y`: x has no storage of its own, it is another view of
  /// the variable, field or element y. Null for other variables.
  Expr *getAbsolute() const { return Absolute; }
  void setAbsolute(Expr *E) { Absolute = E; }
  VarDecl *getAbsoluteRoot() const { return AbsoluteRoot; }
  /// A parameter's default value (`b: integer = 1`, objfpc), passed by the
  /// calls that leave it out. Only trailing parameters have one.
  Expr *getDefault() const { return Default; }
  void setDefault(Expr *E) { Default = E; }
  void setAbsoluteRoot(VarDecl *V) { AbsoluteRoot = V; }
  Expr *getInit() const { return Init; }
  void setInit(Expr *E) { Init = E; }
  /// A typed constant (static storage, initialized once), as opposed to an
  /// initialized variable (`var x: T = v;`, objfpc), which is an ordinary
  /// variable set to its initial value whenever its routine is entered.
  bool isTypedConst() const { return Init != nullptr && !InitializedVar; }
  bool isInitializedVar() const { return InitializedVar; }
  void setInitializedVar(Expr *E) {
    Init = E;
    InitializedVar = true;
  }
  bool isClassVar() const { return IsClassVar; }
  bool isExternal() const { return External; }
  bool isCVar() const { return CVar; }
  void setCVar() { CVar = true; }
  void setExternal(Expr *Lib, Expr *Name) {
    External = true;
    LibraryExpr = Lib;
    NameExpr = Name;
  }
  Expr *getLibraryExpr() const { return LibraryExpr; }
  Expr *getNameExpr() const { return NameExpr; }
  llvm::StringRef getLibraryName() const { return LibraryName; }
  void setLibraryName(llvm::StringRef L) { LibraryName = L; }
  /// The C symbol (the name as written unless `name 'sym'`).
  llvm::StringRef getExternalName() const {
    return ExternalName.empty() ? getName().Spelling : ExternalName;
  }
  void setExternalName(llvm::StringRef S) { ExternalName = S; }
  void setClassVar() { IsClassVar = true; }
  ClassType *getVarClass() const { return VarClass; }
  void setVarClass(ClassType *C) { VarClass = C; }
  const ConstValue &getInitValue() const { return InitValue; }
  void setInitValue(ConstValue V) { InitValue = V; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Var; }
};

/// const N = 10;  -- also predeclared true/false/maxint and enum values.
class ConstDecl final : public Decl {
  Expr *Init;         ///< null for predeclared constants and enum values
  Type *Ty = nullptr; ///< set by Sema (or at creation)
  ConstValue Value;

public:
  ConstDecl(SourceLocation Loc, Identifier Name, Expr *Init)
      : Decl(DK_Const, Loc, Name), Init(Init) {}
  ConstDecl(SourceLocation Loc, Identifier Name, Type *Ty, ConstValue V)
      : Decl(DK_Const, Loc, Name), Init(nullptr), Ty(Ty), Value(V) {}
  Expr *getInit() const { return Init; }
  Type *getType() const { return Ty; }
  const ConstValue &getValue() const { return Value; }
  void setValue(Type *T, ConstValue V) { Ty = T; Value = V; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Const; }
};

/// type T = ...;  -- also the predeclared integer/real/boolean/char.
class TypeDecl final : public Decl {
  TypeSpec *TS;       ///< null for predeclared types
  Type *Ty = nullptr;

public:
  TypeDecl(SourceLocation Loc, Identifier Name, TypeSpec *TS)
      : Decl(DK_Type, Loc, Name), TS(TS) {}
  TypeDecl(SourceLocation Loc, Identifier Name, Type *Ty)
      : Decl(DK_Type, Loc, Name), TS(nullptr), Ty(Ty) {}
  TypeSpec *getTypeSpec() const { return TS; }
  Type *getType() const { return Ty; }
  void setType(Type *T) { Ty = T; }
  /// generic TList<T> = ... (objfpc): a template. Its tokens, and those of
  /// each method implementation (TList.Add), are kept by the parser, which
  /// parses them again for every specialization (specialize TList<integer>).
  bool isGeneric() const { return Generic; }
  void setGeneric(llvm::ArrayRef<Identifier> Params, void *Tokens) {
    Generic = true;
    TypeParams = Params;
    GenericTokens = Tokens;
  }
  llvm::ArrayRef<Identifier> getTypeParams() const { return TypeParams; }
  void *getGenericTokens() const { return GenericTokens; }
  void *getGenericScope() const { return GenericScope; }
  void setGenericScope(void *S) { GenericScope = S; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Type; }

private:
  bool Generic = false;
  llvm::ArrayRef<Identifier> TypeParams;
  void *GenericTokens = nullptr; ///< the parser's GenericTokens
  void *GenericScope = nullptr;  ///< Sema: the scope it is declared in
};

/// A record field.
class FieldDecl final : public Decl {
  TypeSpec *TS;
  Type *Ty = nullptr;

public:
  FieldDecl(Identifier Name, TypeSpec *TS)
      : Decl(DK_Field, Name.Loc, Name), TS(TS) {}
  TypeSpec *getTypeSpec() const { return TS; }
  Type *getType() const { return Ty; }
  void setType(Type *T) { Ty = T; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Field; }
};

/// label 10;  (the name is the label's value, e.g. "10" for `0010`)
class LabelDecl final : public Decl {
  RoutineDecl *Owner = nullptr;   ///< null for program-level labels
  LabeledStmt *Target = nullptr;  ///< the statement it prefixes
  /// The statement sequence (compound / repeat body) that directly contains
  /// Target, or null. Gotos must be inside Target or inside this sequence.
  const void *Sequence = nullptr;
  bool TopLevel = false;          ///< in the block's outermost sequence
  bool NonLocal = false;          ///< target of a goto from a nested routine
  unsigned Index = 0;             ///< 1-based id for non-local gotos

public:
  explicit LabelDecl(Identifier Name) : Decl(DK_Label, Name.Loc, Name) {}
  RoutineDecl *getOwner() const { return Owner; }
  void setOwner(RoutineDecl *R) { Owner = R; }
  LabeledStmt *getTarget() const { return Target; }
  const void *getSequence() const { return Sequence; }
  bool isTopLevel() const { return TopLevel; }
  void setTarget(LabeledStmt *S, const void *Seq, bool Top) {
    Target = S;
    Sequence = Seq;
    TopLevel = Top;
  }
  bool isNonLocal() const { return NonLocal; }
  void setNonLocal() { NonLocal = true; }
  unsigned getIndex() const { return Index; }
  void setIndex(unsigned I) { Index = I; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Label; }
};

class Block;

/// A routine that is a method (objfpc): declared in a class, implemented as
/// `procedure TFoo.Name ...` at program or unit level.
/// Operator: a record's `class operator` (a static function).
enum class MethodKind : unsigned char {
  None, Method, Constructor, Destructor, Operator
};

/// A class constructor or destructor (objfpc, classes and advanced
/// records): a static class procedure without parameters that the program
/// or unit declaring the type runs at its start (before its initialization
/// part) or end; not a member (it cannot be called).
enum class ClassInitKind : unsigned char { None, Constructor, Destructor };

/// The methods of the predeclared class TObject, which have no Pascal body.
/// Predeclared methods without a Pascal body: TObject's, and
/// Exception.Create (objfpc).
enum class TObjectMethod : unsigned char {
  None, Create, Destroy, Free, ClassName, ExceptionCreate, ExceptionCreateFmt,
  ClassType, ClassParent, InheritsFrom, InstanceSize
};

/// procedure P(params); block;     function F(params): T; block;
/// A `forward` declaration and its later definition are two nodes; Sema
/// moves the definition's block onto the forward one (the canonical decl)
/// and marks the definition as a redeclaration. A method's canonical decl is
/// its heading in the class; its implementation is a redeclaration.
class RoutineDecl final : public Decl {
  bool IsFunction;
  llvm::ArrayRef<VarDecl *> Params;
  bool HasParamList;            ///< parentheses were written
  TypeSpec *ResultTS;           ///< functions only (may be omitted after forward)
  Type *ResultTy = nullptr;     ///< set by Sema
  Block *Body;                  ///< null for a forward declaration
  bool IsForward;
  bool UsesManaged = false;
  bool HasTry = false;
  bool Static = false;
  bool Inline = false;
  CallConv CC = CallConv::Default; ///< the directive written
  bool InheritedCDecl = false;     ///< an override of a cdecl method
  RoutineDecl *Canonical = nullptr; ///< set on a definition of a forward decl
  /// objfpc: `generic function F<T, U>...`, a template: its type parameters
  /// and its tokens (the parser's GenericTokens), parsed again for each
  /// specialization; not checked or compiled itself (Sema: the scope it is
  /// declared in, and the declaration with its body).
  llvm::ArrayRef<Identifier> TypeParams;
  bool Generic = false;
  const void *GenericTokens = nullptr;
  void *GenericScope = nullptr;
  bool TemplateMember = false;
  bool Lambda = false, LambdaDone = false, LambdaBad = false;
  bool Closure = false, HeapFrame = false, MethodLambda = false;
  bool CopyFactory = false, Copied = false, CopyAll = false;
  bool SpawnBlock = false;
  TaskType *Async = nullptr;
  VarDecl *LambdaSelf = nullptr;
  Identifier Capture;
  RoutineDecl *GenericDef = nullptr;
  RoutineDecl *Parent = nullptr;    ///< enclosing routine; null at program level
  unsigned Level = 1;               ///< 1 = declared in the program block
  VarDecl *ResultVar = nullptr;     ///< functions: storage for the result
  /// Turbo/FPC `external ['lib'] [name 'sym']`: a C function, no body.
  bool IsExternal = false;
  llvm::StringRef ExternalName;     ///< the C symbol (empty: the Pascal name)
  /// external [Library] [name Name]: the library to link with (empty:
  /// none), resolved by Sema when written as a constant expression.
  llvm::StringRef LibraryName;
  Expr *LibraryExpr = nullptr, *NameExpr = nullptr;
  bool VarArgs = false;             ///< `varargs`: C variadic (externals)
  // Methods (objfpc).
  MethodKind MK = MethodKind::None;
  Identifier ClassName;             ///< `TFoo` of an implementation `TFoo.Bar`
  llvm::ArrayRef<Identifier> OuterClassNames;
  bool Virtual = false, Override = false, Abstract = false;
  bool ClassMethod = false;
  ClassInitKind ClassInit = ClassInitKind::None;
  /// Overloading (objfpc): the `overload` directive, the next routine of
  /// the same name in the same scope or class (set by Sema), and this
  /// one's number among them (1, 2, ...; 0 if not overloaded), which goes
  /// into its symbol.
  bool OverloadDirective = false;
  InterfaceType *Intf = nullptr;
  RoutineDecl *NextOverload = nullptr;
  unsigned OverloadIndex = 0;
  ClassType *Class = nullptr;       ///< set by Sema: the class declaring it
  VarDecl *SelfVar = nullptr;       ///< set by Sema: the hidden parameter
  RoutineDecl *Impl = nullptr;      ///< set by Sema: a method's implementation
  int VMTIndex = -1;                ///< set by Sema: slot of a virtual method
  TObjectMethod Builtin = TObjectMethod::None;
  SourceLocation StaticDirectiveLoc; ///< see getStaticDirectiveLoc
  bool ResultName = false;           ///< see hasResultName

public:
  RoutineDecl(SourceLocation Loc, Identifier Name, bool IsFunction,
              llvm::ArrayRef<VarDecl *> Params, bool HasParamList,
              TypeSpec *ResultTS, Block *Body, bool IsForward)
      : Decl(DK_Routine, Loc, Name), IsFunction(IsFunction), Params(Params),
        HasParamList(HasParamList), ResultTS(ResultTS), Body(Body),
        IsForward(IsForward) {}

  bool isFunction() const { return IsFunction; }
  bool isProcedure() const { return !IsFunction && !Async; }
  llvm::ArrayRef<VarDecl *> getParams() const { return Params; }
  void setParams(llvm::ArrayRef<VarDecl *> P) { Params = P; }
  bool hasParamList() const { return HasParamList; }
  TypeSpec *getResultTypeSpec() const { return ResultTS; }
  Type *getResultType() const { return ResultTy; }
  void setResultType(Type *T) { ResultTy = T; }
  Block *getBody() const { return Body; }
  void setBody(Block *B) { Body = B; }
  bool isForward() const { return IsForward; }
  RoutineDecl *getCanonical() const { return Canonical; }
  void setCanonical(RoutineDecl *C) { Canonical = C; }
  bool isRedeclaration() const { return Canonical != nullptr; }
  RoutineDecl *getParent() const { return Parent; }
  unsigned getLevel() const { return Level; }
  void setParent(RoutineDecl *P) {
    Parent = P;
    Level = P ? P->Level + 1 : 1;
  }
  VarDecl *getResultVar() const { return ResultVar; }
  void setResultVar(VarDecl *V) { ResultVar = V; }
  /// Does the body compute values of managed types (COM interfaces)? Then
  /// code generation releases the references it holds when an exception
  /// leaves the routine.
  bool usesManaged() const { return UsesManaged; }
  void setUsesManaged() { UsesManaged = true; }
  /// Does the body have a try statement? Then an exception continues in
  /// the routine after a _longjmp, so code generation keeps all its
  /// variables in memory.
  bool hasTry() const { return HasTry; }
  void setHasTry() { HasTry = true; }
  /// Nested routines get a hidden pointer to the parent's frame.
  bool hasStaticLink() const { return Parent != nullptr; }
  bool isExternal() const { return IsExternal; }
  llvm::StringRef getExternalName() const { return ExternalName; }
  void setExternalName(llvm::StringRef Sym) { ExternalName = Sym; }
  llvm::StringRef getLibraryName() const { return LibraryName; }
  void setLibraryName(llvm::StringRef L) { LibraryName = L; }
  /// The library and symbol name expressions as written (null if absent).
  Expr *getLibraryExpr() const { return LibraryExpr; }
  Expr *getNameExpr() const { return NameExpr; }
  void setExternalExprs(Expr *Lib, Expr *Name) {
    LibraryExpr = Lib;
    NameExpr = Name;
  }
  bool isVarArgs() const { return VarArgs; }
  void setVarArgs() { VarArgs = true; }
  void setExternal(llvm::StringRef Sym) {
    IsExternal = true;
    ExternalName = Sym;
  }

  MethodKind getMethodKind() const { return MK; }
  void setMethodKind(MethodKind K) { MK = K; }
  bool isOperator() const { return MK == MethodKind::Operator; }
  /// Methods have the hidden parameter Self, except a record's class
  /// methods and operators (static).
  bool hasSelf() const { return (isMethod() && !Static) || MethodLambda; }
  bool isStatic() const { return Static; }
  void setStatic(bool S = true) { Static = S; }
  /// The `inline` directive (with {$inline on}): code generation asks
  /// LLVM to inline the routine (inlinehint).
  bool isInline() const { return Inline; }
  void setInline() { Inline = true; }
  /// A routine or method implementation's `static` directive (Free Pascal:
  /// repeats the declaration's; ignored on plain routines); invalid if none.
  SourceLocation getStaticDirectiveLoc() const { return StaticDirectiveLoc; }
  /// Does `Result` name the function's result in its body ({$modeswitch
  /// result}, on in objfpc mode, where the body is)?
  bool hasResultName() const { return ResultName; }
  void setResultName(bool B) { ResultName = B; }
  void setStaticDirectiveLoc(SourceLocation L) { StaticDirectiveLoc = L; }
  CallConv getCallConv() const { return CC; }
  void setCallConv(CallConv C) { CC = C; }
  /// Whether it is called as 'cdecl' (written, or inherited by an override).
  bool isCDecl() const { return CC == CallConv::CDecl || InheritedCDecl; }
  void setInheritedCDecl() { InheritedCDecl = true; }
  bool isMethod() const { return MK != MethodKind::None; }
  bool isConstructor() const { return MK == MethodKind::Constructor; }
  bool isDestructor() const { return MK == MethodKind::Destructor; }
  const Identifier &getClassName() const { return ClassName; }
  void setClassName(Identifier N) { ClassName = N; }
  /// TOuter of an implementation TOuter.TInner.Bar: the classes or records
  /// enclosing ClassName, outermost first.
  llvm::ArrayRef<Identifier> getOuterClassNames() const { return OuterClassNames; }
  void setOuterClassNames(llvm::ArrayRef<Identifier> N) { OuterClassNames = N; }
  bool isGeneric() const { return Generic; }
  void setGeneric(llvm::ArrayRef<Identifier> Params, const void *Tokens) {
    Generic = true;
    TypeParams = Params;
    GenericTokens = Tokens;
  }
  /// A specialization: an ordinary routine from now on.
  void clearGeneric() { Generic = false; }
  /// The implementation of a generic type's method (TList.Add): a
  /// template too, parsed again for each specialization of the type.
  bool isTemplateMember() const { return TemplateMember; }
  void setTemplateMember() { TemplateMember = true; }
  /// The routine of a lambda (function(x) => e): hidden, nested in the
  /// routine the lambda is written in, with parameter and result types
  /// that Sema may take from where it is used (isLambdaDone once it has).
  /// getCapture: the first name it uses of the routines around it (valid:
  /// it must not outlive them).
  bool isLambda() const { return Lambda; }
  void setLambda() { Lambda = true; }
  bool isLambdaDone() const { return LambdaDone; }
  void setLambdaDone() { LambdaDone = true; }
  /// It does not fit where it is used (reported): its value is an error.
  bool isLambdaBad() const { return LambdaBad; }
  void setLambdaBad() { LambdaBad = true; }
  const Identifier &getCapture() const { return Capture; }
  void setCapture(Identifier Id) { Capture = Id; }
  /// A lambda made a `reference to` value (a closure): it may outlive the
  /// routines around it, whose frames it uses are then on the heap.
  bool isClosure() const { return Closure; }
  void setClosure() { Closure = true; }
  /// Its frame record is on the heap, reference counted (a closure made in
  /// it uses it): freed when the routine has returned and no closure
  /// holds it any more.
  bool hasHeapFrame() const { return HeapFrame; }
  void setHeapFrame() { HeapFrame = true; }
  /// A lambda made a method pointer (`of object`): a routine with a Self
  /// of its own, of the class of the method it is written in, and that
  /// method's Self (getLambdaSelf) as its object. It is not a method of
  /// the class (isMethod is false) but has a Self (hasSelf).
  bool isMethodLambda() const { return MethodLambda; }
  /// A lambda that copies what it uses, function[k, s](x) => e: the
  /// lambda (isCopied) is made by a hidden function (isCopyFactory,
  /// $copyN), nested where it is written, whose value parameters k and s
  /// are the copies (read-only) and whose result is the lambda; the
  /// expression is the call $copyN(k, s). Each call makes the copies anew.
  bool isCopyFactory() const { return CopyFactory; }
  void setCopyFactory() { CopyFactory = true; }
  bool isCopied() const { return Copied; }
  void setCopied() { Copied = true; }
  /// function[=](x) => e: the factory's parameters are the names its body
  /// uses (as the parser saw them); Sema keeps those that are variables or
  /// parameters of the routines around it.
  bool isCopyAll() const { return CopyAll; }
  void setCopyAll() { CopyAll = true; }
  /// An async routine (result `async T` / `async`): its calls are tasks,
  /// of type getAsyncType(); its body is that of a function returning T
  /// (getResultType) or of a procedure.
  bool isAsync() const { return Async != nullptr; }
  TaskType *getAsyncType() const { return Async; }
  void setAsync(TaskType *T) { Async = T; }
  void setFunction(bool F) { IsFunction = F; }
  /// The hidden async procedure of `spawn begin ... end`.
  bool isSpawnBlock() const { return SpawnBlock; }
  void setSpawnBlock() { SpawnBlock = true; }
  VarDecl *getLambdaSelf() const { return LambdaSelf; }
  void setMethodLambda(VarDecl *OuterSelf) {
    MethodLambda = true;
    LambdaSelf = OuterSelf;
  }
  llvm::ArrayRef<Identifier> getTypeParams() const { return TypeParams; }
  const void *getGenericTokens() const { return GenericTokens; }
  void *getGenericScope() const { return GenericScope; }
  void setGenericScope(void *S) { GenericScope = S; }
  /// The generic declaration with the body (itself, or the definition of a
  /// forward or interface heading).
  RoutineDecl *getGenericDefinition() { return GenericDef ? GenericDef : this; }
  void setGenericDefinition(RoutineDecl *R) { GenericDef = R; }
  /// `class procedure` / `class function` (objfpc): Self is a class
  /// reference, not an object.
  bool isClassMethod() const { return ClassMethod; }
  void setClassMethod() { ClassMethod = true; }
  /// `class constructor` / `class destructor` (see ClassInitKind).
  ClassInitKind getClassInit() const { return ClassInit; }
  bool isClassInit() const { return ClassInit != ClassInitKind::None; }
  void setClassInit(ClassInitKind K) { ClassInit = K; }
  bool hasOverloadDirective() const { return OverloadDirective; }
  void setOverloadDirective() { OverloadDirective = true; }
  RoutineDecl *getNextOverload() const { return NextOverload; }
  bool isOverloaded() const { return OverloadIndex != 0; }
  unsigned getOverloadIndex() const { return OverloadIndex; }
  /// Appends R to this overload set (this: its first routine).
  void addOverload(RoutineDecl *R) {
    if (!OverloadIndex)
      OverloadIndex = 1;
    RoutineDecl *Last = this;
    while (Last->NextOverload)
      Last = Last->NextOverload;
    R->OverloadIndex = Last->OverloadIndex + 1;
    Last->NextOverload = R;
  }
  bool isVirtual() const { return Virtual; }
  bool isOverride() const { return Override; }
  bool isAbstract() const { return Abstract; }
  void setDirectives(bool V, bool O, bool A) {
    Virtual = V;
    Override = O;
    Abstract = A;
  }
  ClassType *getClass() const { return Class; }
  void setClass(ClassType *C) { Class = C; }
  /// A method heading of an interface (its slot: getVMTIndex).
  InterfaceType *getInterface() const { return Intf; }
  void setInterface(InterfaceType *I) { Intf = I; }
  RoutineDecl *getImplementation() const { return Impl; }
  void setImplementation(RoutineDecl *R) { Impl = R; }
  /// Where the body is: a method's implementation, else the declaration.
  SourceLocation getBodyLocation() const {
    return Impl ? Impl->getLocation() : getLocation();
  }
  VarDecl *getSelfVar() const { return SelfVar; }
  void setSelfVar(VarDecl *V) { SelfVar = V; }
  int getVMTIndex() const { return VMTIndex; }
  void setVMTIndex(int I) { VMTIndex = I; }
  bool isDispatched() const { return VMTIndex >= 0; }
  TObjectMethod getBuiltin() const { return Builtin; }
  void setBuiltin(TObjectMethod B) { Builtin = B; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Routine; }
};

/// property Name: T read R write W;  (objfpc) R is a field or a function
/// without parameters, W a field or a procedure with one parameter.
class PropertyDecl final : public Decl {
  TypeSpec *TS;
  Type *Ty = nullptr;
  Identifier ReadName, WriteName; ///< invalid when omitted
  Decl *Read = nullptr, *Write = nullptr; ///< set by Sema
  /// An array property `P[i: T]`: the index parameters, passed to the
  /// accessors (methods) first.
  llvm::ArrayRef<VarDecl *> Params;
  Expr *IndexSpec = nullptr;    ///< `index N`: passed to the accessors next
  Expr *DefaultValue = nullptr; ///< `default V` (for streaming; no effect)
  bool IsDefault = false;       ///< `...; default;`: obj[i] means obj.P[i]
  bool NoDefault = false;       ///< `nodefault` (no effect)
  /// `class property` (objfpc, classes and advanced records): its accessors
  /// are class variables or static class methods, so it is used without
  /// an object (through the type, a class reference, an object or Self).
  bool ClassProperty = false;

public:
  PropertyDecl(Identifier Name, TypeSpec *TS, Identifier ReadName,
               Identifier WriteName)
      : Decl(DK_Property, Name.Loc, Name), TS(TS), ReadName(ReadName),
        WriteName(WriteName) {}
  TypeSpec *getTypeSpec() const { return TS; }
  Type *getType() const { return Ty; }
  void setType(Type *T) { Ty = T; }
  const Identifier &getReadName() const { return ReadName; }
  const Identifier &getWriteName() const { return WriteName; }
  Decl *getRead() const { return Read; }
  Decl *getWrite() const { return Write; }
  void setAccessors(Decl *R, Decl *W) {
    Read = R;
    Write = W;
  }
  llvm::ArrayRef<VarDecl *> getParams() const { return Params; }
  bool isArrayProperty() const { return !Params.empty(); }
  void setParams(llvm::ArrayRef<VarDecl *> P) { Params = P; }
  Expr *getIndexSpec() const { return IndexSpec; }
  void setIndexSpec(Expr *E) { IndexSpec = E; }
  Expr *getDefaultValue() const { return DefaultValue; }
  void setDefaultValue(Expr *E) { DefaultValue = E; }
  bool isDefault() const { return IsDefault; }
  void setDefault() { IsDefault = true; }
  bool isNoDefault() const { return NoDefault; }
  void setNoDefault() { NoDefault = true; }
  bool isClassProperty() const { return ClassProperty; }
  void setClassProperty() { ClassProperty = true; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Property; }
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

/// ISO 7185 block: declarations followed by the statement part. Decls keeps
/// source order (const, type, var and routine declarations, interleaved in
/// --mode=tp).
class Block {
  llvm::ArrayRef<Decl *> Decls;
  CompoundStmt *Body;

public:
  Block(llvm::ArrayRef<Decl *> Decls, CompoundStmt *Body)
      : Decls(Decls), Body(Body) {}
  llvm::ArrayRef<Decl *> getDecls() const { return Decls; }
  /// Sema adds the specializations of generic routines a module uses.
  void setDecls(llvm::ArrayRef<Decl *> D) { Decls = D; }
  CompoundStmt *getBody() const { return Body; }
};

/// program Name(input, output); [uses U1, U2;] <block> .
class ProgramDecl final : public Decl {
  llvm::ArrayRef<Identifier> Params;
  llvm::ArrayRef<Identifier> Uses; ///< Turbo/FPC units
  Block *Body;

public:
  ProgramDecl(SourceLocation Loc, Identifier Name,
              llvm::ArrayRef<Identifier> Params, Block *Body,
              llvm::ArrayRef<Identifier> Uses = {})
      : Decl(DK_Program, Loc, Name), Params(Params), Uses(Uses), Body(Body) {}
  llvm::ArrayRef<Identifier> getParams() const { return Params; }
  llvm::ArrayRef<Identifier> getUses() const { return Uses; }
  Block *getBlock() const { return Body; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Program; }
};

/// unit Name; interface [uses ...] decls implementation [uses ...] decls
/// [begin|initialization stmts] end.     (Turbo/FPC)
/// Interface routine headings are forward declarations completed in the
/// implementation part.
class UnitDecl final : public Decl {
  llvm::ArrayRef<Identifier> InterfaceUses, ImplementationUses;
  Block *Interface;      ///< declarations only (Body is empty)
  Block *Implementation; ///< declarations + initialization statements

public:
  UnitDecl(SourceLocation Loc, Identifier Name,
           llvm::ArrayRef<Identifier> InterfaceUses, Block *Interface,
           llvm::ArrayRef<Identifier> ImplementationUses, Block *Implementation)
      : Decl(DK_Unit, Loc, Name), InterfaceUses(InterfaceUses),
        ImplementationUses(ImplementationUses), Interface(Interface),
        Implementation(Implementation) {}
  llvm::ArrayRef<Identifier> getInterfaceUses() const { return InterfaceUses; }
  llvm::ArrayRef<Identifier> getImplementationUses() const {
    return ImplementationUses;
  }
  Block *getInterface() const { return Interface; }
  Block *getImplementation() const { return Implementation; }
  static bool classof(const Decl *D) { return D->getKind() == DK_Unit; }
};

inline FieldDecl *FieldExpr::getFieldDecl() const {
  return llvm::dyn_cast_or_null<FieldDecl>(Member);
}

/// Is E (checked) an ordinal field or element of a bitpacked record or array
/// (Free Pascal): a part of a byte, assignable but without an address?
bool isBitPackedComponent(Expr *E);

} // namespace pcc

#endif // PCC_AST_AST_H
