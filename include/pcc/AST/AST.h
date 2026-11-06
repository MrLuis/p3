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

class Decl;
class FieldDecl;
class LabelDecl;
class RoutineDecl;
class Type;
class TypeSpec;
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

enum class BinaryOp {
  // adding operators (Xor: Turbo/FPC)
  Add, Sub, Or, Xor,
  // multiplying operators (Shl, Shr: Turbo/FPC)
  Mul, RealDiv, IntDiv, Mod, And, Shl, Shr,
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

/// 3.14
class RealLiteralExpr final : public Expr {
  double Value;

public:
  RealLiteralExpr(SourceRange R, double V) : Expr(EK_RealLiteral, R), Value(V) {}
  double getValue() const { return Value; }
  static bool classof(const Expr *E) { return E->getKind() == EK_RealLiteral; }
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
  static bool classof(const Expr *E) { return E->getKind() == EK_Binary; }
};

/// Function call: abs(x), f(1, 2).
class CallExpr final : public Expr {
  Identifier Callee;
  llvm::ArrayRef<Expr *> Args;
  Decl *D = nullptr; ///< set by Sema: BuiltinDecl or RoutineDecl

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

/// a[i]. `a[i, j]` is parsed as `a[i][j]`.
class IndexExpr final : public Expr {
  Expr *Base;
  Expr *Index;

public:
  IndexExpr(SourceRange R, Expr *Base, Expr *Index)
      : Expr(EK_Index, R), Base(Base), Index(Index) {}
  Expr *getBase() const { return Base; }
  Expr *getIndex() const { return Index; }
  static bool classof(const Expr *E) { return E->getKind() == EK_Index; }
};

/// r.f
class FieldExpr final : public Expr {
  Expr *Base;
  Identifier Field;
  FieldDecl *FD = nullptr; ///< set by Sema

public:
  FieldExpr(SourceRange R, Expr *Base, Identifier Field)
      : Expr(EK_Field, R), Base(Base), Field(Field) {}
  Expr *getBase() const { return Base; }
  const Identifier &getField() const { return Field; }
  FieldDecl *getFieldDecl() const { return FD; }
  void setFieldDecl(FieldDecl *F) { FD = F; }
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

/// @x  (Turbo/FPC): an untyped pointer to a variable.
class AddrOfExpr final : public Expr {
  Expr *Operand;

public:
  AddrOfExpr(SourceRange R, Expr *Operand) : Expr(EK_AddrOf, R), Operand(Operand) {}
  Expr *getOperand() const { return Operand; }
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
  static bool classof(const Stmt *S) { return S->getKind() == SK_Assign; }
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
  Expr *getInit() const { return Init; }
  Expr *getFinal() const { return Final; }
  bool isDownto() const { return Downto; }
  Stmt *getBody() const { return Body; }
  static bool classof(const Stmt *S) { return S->getKind() == SK_For; }
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
/// address once per with statement.
struct WithContext {
  Expr *Record;
  explicit WithContext(Expr *Record) : Record(Record) {}
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

//===----------------------------------------------------------------------===//
// Written types
//===----------------------------------------------------------------------===//

class TypeSpec {
public:
  enum TypeSpecKind {
    TS_Named, TS_Enum, TS_Subrange, TS_Array, TS_Record, TS_Pointer, TS_Set,
    TS_File, TS_Routine, TS_String
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

public:
  explicit NamedTypeSpec(Identifier Name)
      : TypeSpec(TS_Named, Name.getRange()), Name(Name) {}
  const Identifier &getName() const { return Name; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Named; }
};

/// (red, green, blue)
class EnumTypeSpec final : public TypeSpec {
  llvm::ArrayRef<Identifier> Names;

public:
  EnumTypeSpec(SourceRange R, llvm::ArrayRef<Identifier> Names)
      : TypeSpec(TS_Enum, R), Names(Names) {}
  llvm::ArrayRef<Identifier> getNames() const { return Names; }
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

public:
  ArrayTypeSpec(SourceRange R, llvm::ArrayRef<TypeSpec *> Indexes,
                TypeSpec *Element, bool Packed)
      : TypeSpec(TS_Array, R), Indexes(Indexes), Element(Element),
        Packed(Packed) {}
  llvm::ArrayRef<TypeSpec *> getIndexes() const { return Indexes; }
  TypeSpec *getElement() const { return Element; }
  bool isPacked() const { return Packed; }
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

public:
  RecordTypeSpec(SourceRange R, FieldList *Fields, bool Packed)
      : TypeSpec(TS_Record, R), Fields(Fields), Packed(Packed) {}
  FieldList *getFields() const { return Fields; }
  bool isPacked() const { return Packed; }
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
  bool IsFunction;
  llvm::ArrayRef<VarDecl *> Params;
  TypeSpec *Result; ///< functions only

public:
  RoutineTypeSpec(SourceRange R, bool IsFunction,
                  llvm::ArrayRef<VarDecl *> Params, TypeSpec *Result)
      : TypeSpec(TS_Routine, R), IsFunction(IsFunction), Params(Params),
        Result(Result) {}
  bool isFunction() const { return IsFunction; }
  llvm::ArrayRef<VarDecl *> getParams() const { return Params; }
  TypeSpec *getResult() const { return Result; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_Routine; }
};

/// string [ '[' MaxLen ']' ]  (Turbo/FPC)
class StringTypeSpec final : public TypeSpec {
  Expr *MaxLen; ///< null for plain `string` (255)

public:
  StringTypeSpec(SourceRange R, Expr *MaxLen)
      : TypeSpec(TS_String, R), MaxLen(MaxLen) {}
  Expr *getMaxLength() const { return MaxLen; }
  static bool classof(const TypeSpec *T) { return T->getKind() == TS_String; }
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

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

class Decl {
public:
  enum DeclKind {
    DK_Program, DK_Var, DK_Const, DK_Type, DK_Field, DK_Routine, DK_Builtin,
    DK_Label, DK_Unit
  };

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
  ConstValue InitValue;         ///< set by Sema

public:
  VarDecl(Identifier Name, TypeSpec *TS, VarKind VK = Variable)
      : Decl(DK_Var, Name.Loc, Name), TS(TS), VK(VK) {}
  TypeSpec *getTypeSpec() const { return TS; }
  Type *getType() const { return Ty; }
  void setType(Type *T) { Ty = T; }
  VarKind getVarKind() const { return VK; }
  bool isParam() const {
    return VK == ValueParam || VK == VarParam || VK == ConstParam;
  }
  bool isVarParam() const { return VK == VarParam; }
  bool isConstParam() const { return VK == ConstParam; }
  RoutineDecl *getOwner() const { return Owner; }
  void setOwner(RoutineDecl *R) { Owner = R; }
  Expr *getInit() const { return Init; }
  void setInit(Expr *E) { Init = E; }
  bool isTypedConst() const { return Init != nullptr; }
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
  static bool classof(const Decl *D) { return D->getKind() == DK_Type; }
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

/// procedure P(params); block;     function F(params): T; block;
/// A `forward` declaration and its later definition are two nodes; Sema
/// moves the definition's block onto the forward one (the canonical decl)
/// and marks the definition as a redeclaration.
class RoutineDecl final : public Decl {
  bool IsFunction;
  llvm::ArrayRef<VarDecl *> Params;
  bool HasParamList;            ///< parentheses were written
  TypeSpec *ResultTS;           ///< functions only (may be omitted after forward)
  Type *ResultTy = nullptr;     ///< set by Sema
  Block *Body;                  ///< null for a forward declaration
  bool IsForward;
  RoutineDecl *Canonical = nullptr; ///< set on a definition of a forward decl
  RoutineDecl *Parent = nullptr;    ///< enclosing routine; null at program level
  unsigned Level = 1;               ///< 1 = declared in the program block
  VarDecl *ResultVar = nullptr;     ///< functions: storage for the result
  /// Turbo/FPC `external ['lib'] [name 'sym']`: a C function, no body.
  bool IsExternal = false;
  llvm::StringRef ExternalName;     ///< the C symbol (empty: the Pascal name)

public:
  RoutineDecl(SourceLocation Loc, Identifier Name, bool IsFunction,
              llvm::ArrayRef<VarDecl *> Params, bool HasParamList,
              TypeSpec *ResultTS, Block *Body, bool IsForward)
      : Decl(DK_Routine, Loc, Name), IsFunction(IsFunction), Params(Params),
        HasParamList(HasParamList), ResultTS(ResultTS), Body(Body),
        IsForward(IsForward) {}

  bool isFunction() const { return IsFunction; }
  bool isProcedure() const { return !IsFunction; }
  llvm::ArrayRef<VarDecl *> getParams() const { return Params; }
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
  /// Nested routines get a hidden pointer to the parent's frame.
  bool hasStaticLink() const { return Parent != nullptr; }
  bool isExternal() const { return IsExternal; }
  llvm::StringRef getExternalName() const { return ExternalName; }
  void setExternal(llvm::StringRef Sym) {
    IsExternal = true;
    ExternalName = Sym;
  }
  static bool classof(const Decl *D) { return D->getKind() == DK_Routine; }
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

} // namespace pcc

#endif // PCC_AST_AST_H
