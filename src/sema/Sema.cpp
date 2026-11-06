//===- Sema.cpp - Semantic analysis -------------------------------------===//
//
// One pass over the AST, in declaration order (Pascal requires
// declare-before-use; `forward` covers mutual recursion):
//
//   * scopes: standard scope > program block > routine blocks > with scopes
//   * declarations: constants are folded, types are built (name
//     equivalence), routines get their parent/level/result slot
//   * statements and expressions are type-checked against ISO 7185
//     compatibility (6.4.5) and assignment-compatibility (6.4.6) rules
//
//===----------------------------------------------------------------------===//

#include "pcc/Sema/Sema.h"
#include "pcc/AST/ASTContext.h"
#include "pcc/AST/ASTVisitor.h"
#include "pcc/Basic/Diagnostics.h"
#include "pcc/Basic/LangOptions.h"
#include "pcc/Sema/Scope.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <cmath>
#include <memory>
#include <optional>

using namespace pcc;

namespace {

/// Predeclared names from PLAN.md 4.5 that later milestones implement. Using
/// one gives "not supported yet" instead of "undeclared identifier".
bool isFutureBuiltin(llvm::StringRef Name) {
  static const llvm::StringSet<> Names = {
      "text", "input", "output",                               // files (M10)
      "reset", "rewrite", "get", "put", "page", "eof", "eoln", // files (M10)
      "pack", "unpack"};                                       // M10
  return Names.contains(Name);
}

/// A folded constant.
struct Constant {
  Type *Ty;
  ConstValue Value;
};

class Checker : public StmtVisitor<Checker>,
                public ExprVisitor<Checker, Type *> {
  ASTContext &Ctx;
  DiagnosticsEngine &Diags;
  const LangOptions &LangOpts;

  Scope Standard;              ///< predeclared entities
  Scope *Cur = &Standard;
  RoutineDecl *CurRoutine = nullptr; ///< null in the program block

  /// Enclosing for statements, innermost last. Their control variables must
  /// not be assigned in the loop body (ISO 7185 6.8.3.9 "threatened").
  llvm::SmallVector<ForStmt *, 4> ActiveFors;

  /// Functions whose result has been assigned somewhere in their body.
  llvm::DenseSet<RoutineDecl *> ResultAssigned;

  /// `^T` where T was not declared yet: resolved at the end of the type
  /// section (ISO 7185 6.2.2.9 allows pointers to later types).
  llvm::SmallVector<std::pair<PointerTypeSpec *, PointerType *>, 4>
      PendingPointers;

  Type *errorType() { return Ctx.getErrorType(); }
  bool isISO() const { return LangOpts.Mode == LangMode::ISO; }

  //===--------------------------------------------------------------------===//
  // Standard scope
  //===--------------------------------------------------------------------===//

  Identifier predeclaredName(llvm::StringRef N) {
    llvm::StringRef Interned = Ctx.getStrings().intern(N);
    return {Interned, Interned, SourceLocation()};
  }

  void declareStandardScope() {
    auto Ty = [&](llvm::StringRef N, Type *T) {
      Standard.insert(N, Ctx.create<TypeDecl>(SourceLocation(),
                                              predeclaredName(N), T));
    };
    Ty("integer", Ctx.getIntegerType());
    Ty("real", Ctx.getRealType());
    Ty("boolean", Ctx.getBooleanType());
    Ty("char", Ctx.getCharType());

    auto Const = [&](llvm::StringRef N, Type *T, int64_t V) {
      Standard.insert(N, Ctx.create<ConstDecl>(SourceLocation(),
                                               predeclaredName(N), T,
                                               ConstValue::ordinal(V)));
    };
    Const("false", Ctx.getBooleanType(), 0);
    Const("true", Ctx.getBooleanType(), 1);
    Const("maxint", Ctx.getIntegerType(), PascalMaxInt);

#define BUILTIN_PROC(ID, NAME)                                                 \
  Standard.insert(NAME, Ctx.create<BuiltinDecl>(predeclaredName(NAME),         \
                                                BuiltinKind::ID, true));
#define BUILTIN_FUNC(ID, NAME)                                                 \
  Standard.insert(NAME, Ctx.create<BuiltinDecl>(predeclaredName(NAME),         \
                                                BuiltinKind::ID, false));
#include "pcc/AST/Builtins.def"
  }

  //===--------------------------------------------------------------------===//
  // Diagnostics helpers
  //===--------------------------------------------------------------------===//

  void diagUndeclared(const Identifier &Id) {
    if (isFutureBuiltin(Id.Name))
      Diags.error(Id.Loc, "'" + Id.Spelling + "' is not supported yet",
                  {Id.getRange()});
    else
      Diags.error(Id.Loc, "use of undeclared identifier '" + Id.Spelling + "'",
                  {Id.getRange()});
  }

  void noteDeclaredHere(Decl *D) {
    if (!D->isPredeclared())
      Diags.note(D->getLocation(),
                 "'" + D->getName().Spelling + "' declared here");
  }

  void declare(Decl *D) {
    const Identifier &Id = D->getName();
    if (Decl *Prev = Cur->insert(Id.Name, D)) {
      Diags.error(Id.Loc, "redefinition of '" + Id.Spelling + "'",
                  {Id.getRange()});
      if (!Prev->isPredeclared())
        Diags.note(Prev->getLocation(), "previous definition is here");
    }
  }

  static std::string quoted(Type *T) { return "'" + T->getName() + "'"; }

  Type *check(Expr *E) {
    Type *T = visitExpr(E);
    E->setType(T);
    return T;
  }

  //===--------------------------------------------------------------------===//
  // Type relations (ISO 7185 6.4.5 / 6.4.6)
  //===--------------------------------------------------------------------===//

  /// Compatible types: identical, ordinal types with the same host, or
  /// string types of the same length.
  static bool areCompatible(Type *A, Type *B) {
    if (A == B)
      return true;
    if ((A->isPointer() && B->isNil()) || (A->isNil() && B->isPointer()))
      return true;
    if (A->isSet() && B->isSet()) {
      auto *SA = llvm::cast<SetType>(A), *SB = llvm::cast<SetType>(B);
      return SA->isEmptySet() || SB->isEmptySet() ||
             SA->getElementType()->getBase() == SB->getElementType()->getBase();
    }
    if (A->isOrdinal() && B->isOrdinal())
      return A->getBase() == B->getBase();
    if (A->isStringType() && B->isStringType())
      return A->getStringLength() == B->getStringLength();
    return false;
  }

  /// Can a value of type Value be assigned to a variable of type Target?
  static bool isAssignable(Type *Target, Type *Value) {
    if (Target == Value)
      return true;
    if (Target->isReal() && Value->isIntegerLike())
      return true;
    if (Target->isPointer() && Value->isNil())
      return true;
    if (Target->isSet() && Value->isSet())
      return areCompatible(Target, Value); // range-checked at runtime
    if (Target->isOrdinal() && Value->isOrdinal())
      return Target->getBase() == Value->getBase(); // range-checked at runtime
    if (Target->isStringType() && Value->isStringType() &&
        !Target->isStringLiteral())
      return Target->getStringLength() == Value->getStringLength();
    return false;
  }

  /// Can E be used where a variable is required (assignment target, `var`
  /// argument, with record)?
  static bool isVariableReference(Expr *E) {
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
      Decl *D = N->getDecl();
      return D && (llvm::isa<VarDecl>(D) || llvm::isa<FieldDecl>(D));
    }
    if (auto *I = llvm::dyn_cast<IndexExpr>(E))
      return isVariableReference(I->getBase());
    if (auto *F = llvm::dyn_cast<FieldExpr>(E))
      return isVariableReference(F->getBase());
    // p^ is a variable even when p is a function result.
    return llvm::isa<DerefExpr>(E);
  }

  //===--------------------------------------------------------------------===//
  // Constant folding
  //===--------------------------------------------------------------------===//

  /// ISO 7185 6.3 constant: [sign] (number | constant-identifier) | string.
  static bool isISOConstantForm(Expr *E) {
    if (auto *U = llvm::dyn_cast<UnaryExpr>(E)) {
      if (U->getOp() == UnaryOp::Not)
        return false;
      E = U->getOperand();
      return llvm::isa<IntegerLiteralExpr>(E) || llvm::isa<RealLiteralExpr>(E) ||
             llvm::isa<NameRefExpr>(E);
    }
    return llvm::isa<IntegerLiteralExpr>(E) || llvm::isa<RealLiteralExpr>(E) ||
           llvm::isa<StringLiteralExpr>(E) || llvm::isa<NameRefExpr>(E);
  }

  static double asReal(const Constant &C) {
    return C.Value.K == ConstValue::Real ? C.Value.RealVal
                                         : static_cast<double>(C.Value.Int);
  }

  /// Folds an already type-checked expression. Returns nullopt if it is not
  /// constant (without diagnosing).
  std::optional<Constant> fold(Expr *E) {
    Type *T = E->getType();
    if (!T || T->isError())
      return std::nullopt;
    switch (E->getKind()) {
    case Expr::EK_IntegerLiteral:
      return Constant{T, ConstValue::ordinal(static_cast<int64_t>(
                             llvm::cast<IntegerLiteralExpr>(E)->getValue()))};
    case Expr::EK_RealLiteral:
      return Constant{T, ConstValue::real(llvm::cast<RealLiteralExpr>(E)->getValue())};
    case Expr::EK_StringLiteral: {
      llvm::StringRef S = llvm::cast<StringLiteralExpr>(E)->getValue();
      if (S.size() == 1)
        return Constant{T, ConstValue::ordinal(static_cast<uint8_t>(S[0]))};
      return Constant{T, ConstValue::string(S)};
    }
    case Expr::EK_NameRef: {
      auto *C = llvm::dyn_cast_or_null<ConstDecl>(
          llvm::cast<NameRefExpr>(E)->getDecl());
      if (!C || !C->getValue().isValid())
        return std::nullopt;
      return Constant{C->getType(), C->getValue()};
    }
    case Expr::EK_Paren:
      return fold(llvm::cast<ParenExpr>(E)->getInner());
    case Expr::EK_Unary: {
      auto *U = llvm::cast<UnaryExpr>(E);
      auto V = fold(U->getOperand());
      if (!V)
        return std::nullopt;
      switch (U->getOp()) {
      case UnaryOp::Plus:
        return Constant{T, V->Value};
      case UnaryOp::Minus:
        if (V->Value.K == ConstValue::Real)
          return Constant{T, ConstValue::real(-V->Value.RealVal)};
        return Constant{T, ConstValue::ordinal(-V->Value.Int)};
      case UnaryOp::Not:
        return Constant{T, ConstValue::ordinal(!V->Value.Int)};
      }
      return std::nullopt;
    }
    case Expr::EK_Binary: {
      auto *B = llvm::cast<BinaryExpr>(E);
      auto L = fold(B->getLHS()), R = fold(B->getRHS());
      if (!L || !R || L->Value.K == ConstValue::String ||
          R->Value.K == ConstValue::String)
        return std::nullopt;
      bool IsReal = L->Value.K == ConstValue::Real || R->Value.K == ConstValue::Real;
      int64_t A = L->Value.Int, C = R->Value.Int;
      double X = asReal(*L), Y = asReal(*R);
      auto Ord = [&](int64_t V) { return Constant{T, ConstValue::ordinal(V)}; };
      auto Real = [&](double V) { return Constant{T, ConstValue::real(V)}; };
      switch (B->getOp()) {
      case BinaryOp::Add: return IsReal ? Real(X + Y) : Ord(A + C);
      case BinaryOp::Sub: return IsReal ? Real(X - Y) : Ord(A - C);
      case BinaryOp::Mul: return IsReal ? Real(X * Y) : Ord(A * C);
      case BinaryOp::RealDiv:
        if (Y == 0)
          return std::nullopt;
        return Real(X / Y);
      case BinaryOp::IntDiv:
        if (C == 0)
          return std::nullopt;
        return Ord(A / C);
      case BinaryOp::Mod:
        if (C <= 0)
          return std::nullopt;
        return Ord(((A % C) + C) % C);
      case BinaryOp::And: return Ord(A && C);
      case BinaryOp::Or:  return Ord(A || C);
      case BinaryOp::Eq:  return Ord(IsReal ? X == Y : A == C);
      case BinaryOp::Ne:  return Ord(IsReal ? X != Y : A != C);
      case BinaryOp::Lt:  return Ord(IsReal ? X < Y : A < C);
      case BinaryOp::Le:  return Ord(IsReal ? X <= Y : A <= C);
      case BinaryOp::Gt:  return Ord(IsReal ? X > Y : A > C);
      case BinaryOp::Ge:  return Ord(IsReal ? X >= Y : A >= C);
      case BinaryOp::In:  return std::nullopt;
      }
      return std::nullopt;
    }
    case Expr::EK_Call: {
      auto *Call = llvm::cast<CallExpr>(E);
      auto *BD = llvm::dyn_cast_or_null<BuiltinDecl>(Call->getDecl());
      if (!BD || Call->getArgs().size() != 1)
        return std::nullopt;
      auto V = fold(Call->getArgs()[0]);
      if (!V || V->Value.K == ConstValue::String)
        return std::nullopt;
      auto Ord = [&](int64_t X) { return Constant{T, ConstValue::ordinal(X)}; };
      switch (BD->getBuiltinKind()) {
      case BuiltinKind::Ord: case BuiltinKind::Chr: return Ord(V->Value.Int);
      case BuiltinKind::Succ: return Ord(V->Value.Int + 1);
      case BuiltinKind::Pred: return Ord(V->Value.Int - 1);
      case BuiltinKind::Odd:  return Ord(V->Value.Int & 1);
      case BuiltinKind::Abs:
        if (V->Value.K == ConstValue::Real)
          return Constant{T, ConstValue::real(std::fabs(V->Value.RealVal))};
        return Ord(V->Value.Int < 0 ? -V->Value.Int : V->Value.Int);
      case BuiltinKind::Sqr:
        if (V->Value.K == ConstValue::Real)
          return Constant{T, ConstValue::real(V->Value.RealVal * V->Value.RealVal)};
        return Ord(V->Value.Int * V->Value.Int);
      case BuiltinKind::Trunc: return Ord(static_cast<int64_t>(asReal(*V)));
      case BuiltinKind::Round: return Ord(static_cast<int64_t>(std::round(asReal(*V))));
      default: return std::nullopt;
      }
    }
    default:
      return std::nullopt;
    }
  }

  /// Checks and folds a constant expression in a declaration (const value,
  /// subrange bound, case/variant label). Reports non-constants and, in ISO
  /// mode, constant expressions beyond the ISO `constant` syntax.
  std::optional<Constant> evaluateConstant(Expr *E, llvm::StringRef What) {
    Type *T = check(E);
    if (T->isError())
      return std::nullopt;
    auto C = fold(E);
    if (!C) {
      Diags.error(E->getBeginLoc(), What + " must be a constant",
                  {E->getRange()});
      return std::nullopt;
    }
    if (isISO() && !isISOConstantForm(E))
      Diags.error(E->getBeginLoc(),
                  "constant expressions are a Turbo/Free Pascal extension; "
                  "use --mode=tp",
                  {E->getRange()});
    if (C->Value.K == ConstValue::Ordinal && C->Ty->isIntegerLike() &&
        (C->Value.Int > PascalMaxInt || C->Value.Int < -PascalMaxInt - 1)) {
      Diags.error(E->getBeginLoc(), "constant value is out of range for integer",
                  {E->getRange()});
      return std::nullopt;
    }
    return C;
  }

  static std::string formatValue(Type *T, int64_t V) {
    Type *B = T->getBase();
    if (B->isChar())
      return llvm::isPrint(static_cast<char>(V))
                 ? "'" + std::string(1, static_cast<char>(V)) + "'"
                 : "chr(" + std::to_string(V) + ")";
    if (B->isBoolean())
      return V ? "true" : "false";
    if (auto *E = llvm::dyn_cast<EnumType>(B))
      if (V >= 0 && static_cast<size_t>(V) < E->getValues().size())
        return E->getValues()[V]->getName().Spelling.str();
    return std::to_string(V);
  }

  //===--------------------------------------------------------------------===//
  // Types
  //===--------------------------------------------------------------------===//

  Type *resolveType(TypeSpec *TS, llvm::StringRef DeclName = {}) {
    if (Type *T = TS->getResolved())
      return T;
    Type *T = buildType(TS);
    if (!DeclName.empty() && !T->isError() && !llvm::isa<BuiltinType>(T))
      T->setDeclName(DeclName);
    TS->setResolved(T);
    return T;
  }

  Type *buildType(TypeSpec *TS) {
    switch (TS->getKind()) {
    case TypeSpec::TS_Named: {
      const Identifier &Id = llvm::cast<NamedTypeSpec>(TS)->getName();
      Decl *D = Cur->lookup(Id.Name);
      if (!D) {
        if (isFutureBuiltin(Id.Name))
          Diags.error(Id.Loc, "type '" + Id.Spelling + "' is not supported yet",
                      {Id.getRange()});
        else
          Diags.error(Id.Loc, "unknown type name '" + Id.Spelling + "'",
                      {Id.getRange()});
        return errorType();
      }
      if (auto *TD = llvm::dyn_cast<TypeDecl>(D))
        return TD->getType() ? TD->getType() : errorType();
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' is a " + D->getKindName() +
                      ", not a type",
                  {Id.getRange()});
      noteDeclaredHere(D);
      return errorType();
    }

    case TypeSpec::TS_Enum: {
      auto *ES = llvm::cast<EnumTypeSpec>(TS);
      auto *E = Ctx.create<EnumType>();
      llvm::SmallVector<ConstDecl *, 8> Values;
      for (const Identifier &Id : ES->getNames()) {
        auto *C = Ctx.create<ConstDecl>(
            Id.Loc, Id, E, ConstValue::ordinal(static_cast<int64_t>(Values.size())));
        Values.push_back(C);
        declare(C);
      }
      E->setValues(Ctx.copyArray<ConstDecl *>(Values));
      return E;
    }

    case TypeSpec::TS_Subrange: {
      auto *SS = llvm::cast<SubrangeTypeSpec>(TS);
      auto Lo = evaluateConstant(SS->getLo(), "subrange bound");
      auto Hi = evaluateConstant(SS->getHi(), "subrange bound");
      if (!Lo || !Hi)
        return errorType();
      if (!Lo->Ty->isOrdinal() || !Hi->Ty->isOrdinal()) {
        Diags.error(SS->getBeginLoc(),
                    "subrange bounds must be of an ordinal type",
                    {SS->getRange()});
        return errorType();
      }
      if (Lo->Ty->getBase() != Hi->Ty->getBase()) {
        Diags.error(SS->getBeginLoc(),
                    "subrange bounds have different types (" +
                        quoted(Lo->Ty->getBase()) + " and " +
                        quoted(Hi->Ty->getBase()) + ")",
                    {SS->getRange()});
        return errorType();
      }
      if (Lo->Value.Int > Hi->Value.Int) {
        Diags.error(SS->getBeginLoc(),
                    "subrange lower bound " +
                        formatValue(Lo->Ty, Lo->Value.Int) +
                        " is greater than upper bound " +
                        formatValue(Hi->Ty, Hi->Value.Int),
                    {SS->getRange()});
        return errorType();
      }
      return Ctx.create<SubrangeType>(Lo->Ty->getBase(), Lo->Value.Int,
                                      Hi->Value.Int);
    }

    case TypeSpec::TS_Array: {
      auto *AS = llvm::cast<ArrayTypeSpec>(TS);
      Type *Elem = resolveType(AS->getElement());
      llvm::SmallVector<Type *, 2> Indexes;
      bool OK = !Elem->isError();
      for (TypeSpec *IdxTS : AS->getIndexes()) {
        Type *Idx = resolveType(IdxTS);
        if (Idx->isError()) {
          OK = false;
        } else if (!Idx->isOrdinal()) {
          Diags.error(IdxTS->getBeginLoc(),
                      "array index type must be an ordinal type, not " +
                          quoted(Idx),
                      {IdxTS->getRange()});
          OK = false;
        } else if (Idx->isInteger()) {
          Diags.error(IdxTS->getBeginLoc(),
                      "array index type 'integer' is too large; use a "
                      "subrange such as 1..100",
                      {IdxTS->getRange()});
          OK = false;
        }
        Indexes.push_back(Idx);
      }
      if (!OK)
        return errorType();
      // array [A, B] of E == array [A] of array [B] of E
      Type *T = Elem;
      for (auto It = Indexes.rbegin(); It != Indexes.rend(); ++It)
        T = Ctx.create<ArrayType>(*It, T, AS->isPacked());
      return T;
    }

    case TypeSpec::TS_Pointer: {
      auto *PS = llvm::cast<PointerTypeSpec>(TS);
      const Identifier &Id = PS->getTarget();
      Decl *D = Cur->lookup(Id.Name);
      if (!D) {
        auto *P = Ctx.create<PointerType>(nullptr);
        PendingPointers.push_back({PS, P});
        return P;
      }
      if (auto *TD = llvm::dyn_cast<TypeDecl>(D))
        return Ctx.create<PointerType>(TD->getType() ? TD->getType()
                                                     : errorType());
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' is a " + D->getKindName() +
                      ", not a type",
                  {Id.getRange()});
      return errorType();
    }

    case TypeSpec::TS_Set: {
      auto *SS = llvm::cast<SetTypeSpec>(TS);
      Type *Elem = resolveType(SS->getElement());
      if (Elem->isError())
        return errorType();
      if (!Elem->isOrdinal()) {
        Diags.error(SS->getElement()->getBeginLoc(),
                    "set element type must be an ordinal type, not " +
                        quoted(Elem),
                    {SS->getElement()->getRange()});
        return errorType();
      }
      OrdinalBounds Bd = getOrdinalBounds(Elem);
      if (Bd.Lo < 0 || Bd.Hi > 255) {
        Diags.error(SS->getElement()->getBeginLoc(),
                    "set element type " + quoted(Elem) +
                        " has values outside 0..255; use a subrange",
                    {SS->getElement()->getRange()});
        return errorType();
      }
      return Ctx.create<SetType>(Elem);
    }

    case TypeSpec::TS_Record: {
      auto *RS = llvm::cast<RecordTypeSpec>(TS);
      llvm::SmallVector<FieldDecl *, 16> All;
      llvm::StringMap<FieldDecl *> Seen;
      if (!resolveFieldList(RS->getFields(), All, Seen))
        return errorType();
      return Ctx.create<RecordType>(RS->getFields(),
                                    Ctx.copyArray<FieldDecl *>(All),
                                    RS->isPacked());
    }
    }
    return errorType();
  }

  bool addField(FieldDecl *F, llvm::SmallVectorImpl<FieldDecl *> &All,
                llvm::StringMap<FieldDecl *> &Seen) {
    auto [It, Inserted] = Seen.try_emplace(F->getName().Name, F);
    if (!Inserted) {
      Diags.error(F->getLocation(),
                  "duplicate field name '" + F->getName().Spelling + "'",
                  {F->getName().getRange()});
      Diags.note(It->second->getLocation(), "previous field is here");
      return false;
    }
    All.push_back(F);
    return true;
  }

  bool resolveFieldList(FieldList *FL, llvm::SmallVectorImpl<FieldDecl *> &All,
                        llvm::StringMap<FieldDecl *> &Seen) {
    bool OK = true;
    for (FieldDecl *F : FL->Fixed) {
      F->setType(resolveType(F->getTypeSpec()));
      OK &= !F->getType()->isError();
      OK &= addField(F, All, Seen);
    }
    VariantPart *VP = FL->Variant;
    if (!VP)
      return OK;

    Type *TagTy = resolveType(VP->TagTypeSpec);
    if (!TagTy->isError() && !TagTy->isOrdinal()) {
      Diags.error(VP->TagTypeSpec->getBeginLoc(),
                  "variant tag type must be an ordinal type, not " +
                      quoted(TagTy),
                  {VP->TagTypeSpec->getRange()});
      TagTy = errorType();
    }
    VP->TagType = TagTy;
    OK &= !TagTy->isError();
    if (VP->Tag) {
      VP->Tag->setType(TagTy);
      OK &= addField(VP->Tag, All, Seen);
    }
    for (Variant *V : VP->Variants) {
      for (CaseLabel *L : V->Labels)
        OK &= checkLabel(L, TagTy, "variant label");
      OK &= resolveFieldList(V->Fields, All, Seen);
    }
    return OK;
  }

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  void declareConst(ConstDecl *C) {
    if (auto V = evaluateConstant(C->getInit(), "constant value"))
      C->setValue(V->Ty, V->Value);
    else
      C->setValue(errorType(), ConstValue());
    declare(C);
  }

  void declareType(TypeDecl *TD) {
    TD->setType(resolveType(TD->getTypeSpec(), TD->getName().Spelling));
    declare(TD);
  }

  void declareVar(VarDecl *V) {
    V->setType(resolveType(V->getTypeSpec()));
    V->setOwner(CurRoutine);
    declare(V);
  }

  /// Resolves a routine's parameter and result types.
  void resolveSignature(RoutineDecl *R) {
    for (VarDecl *P : R->getParams()) {
      P->setOwner(R);
      P->setType(resolveType(P->getTypeSpec()));
    }
    if (!R->isFunction())
      return;
    Type *ResultTy = errorType();
    if (!R->getResultTypeSpec()) {
      Diags.error(R->getName().Loc,
                  "function '" + R->getName().Spelling + "' needs a result type",
                  {R->getName().getRange()});
    } else {
      ResultTy = resolveType(R->getResultTypeSpec());
      // ISO 7185 6.6.2: the result type is a simple type (or a pointer).
      if (!ResultTy->isError() && !ResultTy->isValidResultType()) {
        Diags.error(R->getResultTypeSpec()->getBeginLoc(),
                    "function result type must be a simple or pointer type, not " +
                        quoted(ResultTy),
                    {R->getResultTypeSpec()->getRange()});
        ResultTy = errorType();
      }
    }
    R->setResultType(ResultTy);
    auto *Result = Ctx.create<VarDecl>(R->getName(), R->getResultTypeSpec(),
                                       VarDecl::Result);
    Result->setType(ResultTy);
    Result->setOwner(R);
    R->setResultVar(Result);
  }

  static bool sameSignature(RoutineDecl *A, RoutineDecl *B) {
    if (A->getParams().size() != B->getParams().size())
      return false;
    for (size_t I = 0; I < A->getParams().size(); ++I) {
      VarDecl *P = A->getParams()[I], *Q = B->getParams()[I];
      if (P->getVarKind() != Q->getVarKind() || P->getType() != Q->getType())
        return false;
    }
    return true;
  }

  void declareRoutine(RoutineDecl *R) {
    R->setParent(CurRoutine);
    const Identifier &Id = R->getName();

    // The definition of an earlier `forward` declaration?
    auto *Fwd = llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookupLocal(Id.Name));
    if (Fwd && Fwd->isForward() && !Fwd->getBody() && R->getBody()) {
      if (Fwd->isFunction() != R->isFunction()) {
        Diags.error(Id.Loc,
                    "'" + Id.Spelling + "' was declared forward as a " +
                        Fwd->getKindName(),
                    {Id.getRange()});
        Diags.note(Fwd->getLocation(), "forward declaration is here");
        return;
      }
      // ISO omits the parameter list here; if repeated, it must match.
      if (R->hasParamList() || R->getResultTypeSpec()) {
        for (VarDecl *P : R->getParams())
          P->setType(resolveType(P->getTypeSpec()));
        Type *RT = R->getResultTypeSpec() ? resolveType(R->getResultTypeSpec())
                                          : nullptr;
        if (!sameSignature(Fwd, R) || (RT && RT != Fwd->getResultType())) {
          Diags.error(Id.Loc,
                      "heading of '" + Id.Spelling +
                          "' does not match its forward declaration",
                      {Id.getRange()});
          Diags.note(Fwd->getLocation(), "forward declaration is here");
        }
      }
      Fwd->setBody(R->getBody());
      R->setCanonical(Fwd);
      checkRoutineBody(Fwd);
      return;
    }

    resolveSignature(R);
    declare(R);
    if (R->getBody())
      checkRoutineBody(R);
  }

  void checkRoutineBody(RoutineDecl *R) {
    Scope RoutineScope(Cur);
    Scope *SavedScope = Cur;
    RoutineDecl *SavedRoutine = CurRoutine;
    Cur = &RoutineScope;
    CurRoutine = R;

    for (VarDecl *P : R->getParams())
      if (Decl *Prev = Cur->insert(P->getName().Name, P)) {
        Diags.error(P->getLocation(),
                    "duplicate parameter name '" + P->getName().Spelling + "'",
                    {P->getName().getRange()});
        Diags.note(Prev->getLocation(), "previous parameter is here");
      }

    checkBlock(*R->getBody());

    if (R->isFunction() && !ResultAssigned.count(R) &&
        !R->getResultType()->isError())
      Diags.warning(R->getName().Loc,
                    "function '" + R->getName().Spelling +
                        "' never assigns its result",
                    {R->getName().getRange()});

    Cur = SavedScope;
    CurRoutine = SavedRoutine;
  }

  void resolvePendingPointers() {
    for (auto &[PS, P] : PendingPointers) {
      const Identifier &Id = PS->getTarget();
      auto *TD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(Id.Name));
      if (TD && TD->getType()) {
        P->setPointee(TD->getType());
        continue;
      }
      Diags.error(Id.Loc, "unknown type name '" + Id.Spelling + "'",
                  {Id.getRange()});
      P->setPointee(errorType());
    }
    PendingPointers.clear();
  }

  void checkBlock(Block &B) {
    for (Decl *D : B.getDecls()) {
      if (Diags.shouldStop())
        return;
      if (!llvm::isa<TypeDecl>(D))
        resolvePendingPointers();
      switch (D->getKind()) {
      case Decl::DK_Const:   declareConst(llvm::cast<ConstDecl>(D)); break;
      case Decl::DK_Type:    declareType(llvm::cast<TypeDecl>(D)); break;
      case Decl::DK_Var:     declareVar(llvm::cast<VarDecl>(D)); break;
      case Decl::DK_Routine: declareRoutine(llvm::cast<RoutineDecl>(D)); break;
      default: llvm_unreachable("unexpected declaration in block");
      }
    }
    resolvePendingPointers();
    // ISO 7185 6.1.4: every forward declaration needs a definition in the
    // same block.
    for (Decl *D : B.getDecls())
      if (auto *R = llvm::dyn_cast<RoutineDecl>(D);
          R && R->isForward() && !R->getBody())
        Diags.error(R->getName().Loc,
                    "'" + R->getName().Spelling +
                        "' is declared forward but never defined",
                    {R->getName().getRange()});
    visitStmt(B.getBody());
  }

  void checkProgramParams(ProgramDecl &P) {
    llvm::StringMap<SourceLocation> Seen;
    for (const Identifier &Param : P.getParams()) {
      auto [It, Inserted] = Seen.try_emplace(Param.Name, Param.Loc);
      if (!Inserted) {
        Diags.error(Param.Loc,
                    "duplicate program parameter '" + Param.Spelling + "'",
                    {Param.getRange()});
        Diags.note(It->second, "previous occurrence is here");
        continue;
      }
      // ISO 7185 6.10: other parameters must be declared as file variables
      // in the program block (supported from M10).
      if (Param.Name != "input" && Param.Name != "output")
        Diags.error(Param.Loc,
                    "program parameter '" + Param.Spelling +
                        "' is not declared (only 'input' and 'output' are "
                        "supported yet)",
                    {Param.getRange()});
    }
  }

  //===--------------------------------------------------------------------===//
  // Statement helpers
  //===--------------------------------------------------------------------===//

  void checkCondition(Expr *Cond, llvm::StringRef Keyword) {
    Type *T = check(Cond);
    if (!T->isError() && !T->getBase()->isBoolean())
      Diags.error(Cond->getBeginLoc(),
                  "condition of '" + Keyword + "' must be of type 'boolean', "
                  "not " + quoted(T),
                  {Cond->getRange()});
  }

  /// Reports an assignment (or read / var argument) to the control variable
  /// of an enclosing for statement. Returns true if one was reported.
  bool checkNotForControlVar(Expr *Target) {
    auto *N = llvm::dyn_cast<NameRefExpr>(Target);
    if (!N || !N->getDecl())
      return false;
    for (ForStmt *F : ActiveFors) {
      if (F->getVar()->getDecl() != N->getDecl())
        continue;
      Diags.error(N->getBeginLoc(),
                  "cannot assign to '" + N->getName().Spelling +
                      "' inside a 'for' loop that it controls",
                  {N->getRange()});
      Diags.note(F->getBeginLoc(), "'for' loop is here");
      return true;
    }
    return false;
  }

  /// Checks one case or variant label against the selector/tag type.
  bool checkLabel(CaseLabel *L, Type *SelTy, llvm::StringRef What) {
    bool OK = true;
    for (Expr *E : {L->Lo, L->Hi}) {
      if (!E)
        continue;
      auto C = evaluateConstant(E, What);
      if (!C) {
        OK = false;
        continue;
      }
      if (!SelTy->isError() && !areCompatible(C->Ty, SelTy)) {
        Diags.error(E->getBeginLoc(),
                    What + " of type " + quoted(C->Ty) + " does not match " +
                        (What == "case label" ? "selector" : "tag") +
                        " type " + quoted(SelTy),
                    {E->getRange()});
        OK = false;
        continue;
      }
      (E == L->Lo ? L->LoValue : L->HiValue) = C->Value.Int;
    }
    if (!L->Hi)
      L->HiValue = L->LoValue;
    if (OK && L->LoValue > L->HiValue) {
      Diags.error(L->Lo->getBeginLoc(), "case label range is empty",
                  {L->getRange()});
      OK = false;
    }
    if (OK && !SelTy->isError()) {
      OrdinalBounds B = getOrdinalBounds(SelTy);
      if (L->LoValue < B.Lo || L->HiValue > B.Hi) {
        Diags.error(L->Lo->getBeginLoc(),
                    What + " is outside the range of type " + quoted(SelTy),
                    {L->getRange()});
        OK = false;
      }
    }
    return OK;
  }

  /// Checks the arguments of a call to a user routine.
  void checkCallArgs(RoutineDecl *R, const Identifier &Callee,
                     llvm::ArrayRef<Expr *> Args, SourceRange CallRange) {
    auto Params = R->getParams();
    if (Args.size() != Params.size()) {
      Diags.error(Callee.Loc,
                  llvm::Twine(Args.size() < Params.size() ? "too few"
                                                          : "too many") +
                      " arguments in call to '" + Callee.Spelling +
                      "': expected " + llvm::Twine(Params.size()) + ", got " +
                      llvm::Twine(Args.size()),
                  {CallRange});
      noteDeclaredHere(R);
      for (Expr *A : Args)
        if (!llvm::isa<FormattedExpr>(A))
          check(A);
      return;
    }

    for (size_t I = 0; I < Args.size(); ++I) {
      Expr *Arg = Args[I];
      VarDecl *P = Params[I];
      if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
        Diags.error(F->getWidth()->getBeginLoc(),
                    "field widths are only allowed in 'write' and 'writeln'",
                    {F->getWidth()->getRange()});
        continue;
      }
      Type *AT = check(Arg);
      Type *PT = P->getType();
      if (AT->isError() || !PT || PT->isError())
        continue;
      const Identifier &PName = P->getName();

      if (P->isVarParam()) {
        if (!isVariableReference(Arg)) {
          Diags.error(Arg->getBeginLoc(),
                      "argument for 'var' parameter '" + PName.Spelling +
                          "' must be a variable",
                      {Arg->getRange()});
          continue;
        }
        // ISO 7185 6.6.3.3: actual and formal var parameters have the
        // *same* type.
        if (AT != PT)
          Diags.error(Arg->getBeginLoc(),
                      "argument of type " + quoted(AT) + " for 'var' "
                      "parameter '" + PName.Spelling + "' must have exactly "
                      "type " + quoted(PT),
                      {Arg->getRange()});
        else
          checkNotForControlVar(Arg);
        continue;
      }
      if (!isAssignable(PT, AT))
        Diags.error(Arg->getBeginLoc(),
                    "cannot pass a value of type " + quoted(AT) +
                        " to parameter '" + PName.Spelling + "' of type " +
                        quoted(PT),
                    {Arg->getRange()});
    }
  }

  //===--------------------------------------------------------------------===//
  // Built-in procedure calls
  //===--------------------------------------------------------------------===//

  static bool isWritable(Type *T) {
    return T->isIntegerLike() || T->isReal() || T->getBase()->isChar() ||
           T->getBase()->isBoolean() || T->isStringType();
  }

  void checkWrite(ProcCallStmt *S, bool IsWriteln) {
    const Identifier &Callee = S->getCallee();
    if (!IsWriteln && S->getArgs().empty())
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' requires at least one argument",
                  {Callee.getRange()});

    for (Expr *Arg : S->getArgs()) {
      Expr *Value = Arg, *Width = nullptr, *Precision = nullptr;
      auto *F = llvm::dyn_cast<FormattedExpr>(Arg);
      if (F) {
        Value = F->getValue();
        Width = F->getWidth();
        Precision = F->getPrecision();
      }

      Type *T = check(Value);
      if (F)
        F->setType(T);
      if (!T->isError() && !isWritable(T))
        Diags.error(Value->getBeginLoc(),
                    "cannot write a value of type " + quoted(T),
                    {Value->getRange()});

      for (Expr *W : {Width, Precision}) {
        if (!W)
          continue;
        Type *WT = check(W);
        if (!WT->isError() && !WT->isIntegerLike())
          Diags.error(W->getBeginLoc(),
                      llvm::Twine(W == Width ? "field width" : "precision") +
                          " must be an integer, not " + quoted(WT),
                      {W->getRange()});
      }
      // ISO 6.9.3.4: only real values take a fraction-digits argument.
      if (Precision && !T->isError() && !T->isReal())
        Diags.error(Precision->getBeginLoc(),
                    "a precision (':n') is only allowed for real values",
                    {Precision->getRange()});
    }
  }

  void checkRead(ProcCallStmt *S, bool IsReadln) {
    const Identifier &Callee = S->getCallee();
    if (!IsReadln && S->getArgs().empty())
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' requires at least one argument",
                  {Callee.getRange()});

    for (Expr *Arg : S->getArgs()) {
      if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
        Diags.error(F->getWidth()->getBeginLoc(),
                    "field widths are only allowed in 'write' and 'writeln'",
                    {F->getWidth()->getRange()});
        continue;
      }
      Type *T = check(Arg);
      if (T->isError())
        continue;
      if (!isVariableReference(Arg)) {
        Diags.error(Arg->getBeginLoc(),
                    "argument to '" + Callee.Spelling + "' must be a variable",
                    {Arg->getRange()});
        continue;
      }
      if (!T->isIntegerLike() && !T->getBase()->isChar() && !T->isReal())
        Diags.error(Arg->getBeginLoc(),
                    "cannot read a value of type " + quoted(T) +
                        " (only integer, real and char)",
                    {Arg->getRange()});
      else
        checkNotForControlVar(Arg);
    }
  }

  void checkNewDispose(ProcCallStmt *S) {
    const Identifier &Callee = S->getCallee();
    if (S->getArgs().size() != 1) {
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects 1 argument" +
                      (S->getArgs().size() > 1
                           ? " (variant selectors are not supported)"
                           : ""),
                  {S->getRange()});
      return;
    }
    Expr *Arg = S->getArgs()[0];
    Type *T = check(Arg);
    if (T->isError())
      return;
    if (!T->isPointer())
      Diags.error(Arg->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects a pointer variable, not " +
                      "a value of type " + quoted(T),
                  {Arg->getRange()});
    else if (!isVariableReference(Arg))
      Diags.error(Arg->getBeginLoc(),
                  "argument to '" + Callee.Spelling + "' must be a variable",
                  {Arg->getRange()});
  }

  //===--------------------------------------------------------------------===//
  // Built-in function calls
  //===--------------------------------------------------------------------===//

  Type *checkBuiltinFunction(CallExpr *E, BuiltinDecl *BD,
                             llvm::ArrayRef<Type *> ArgTypes) {
    const Identifier &Callee = E->getCallee();
    if (ArgTypes.size() != 1) {
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects 1 argument, got " +
                      llvm::Twine(ArgTypes.size()),
                  {E->getRange()});
      return errorType();
    }
    Type *T = ArgTypes[0];
    if (T->isError())
      return errorType();
    Expr *Arg = E->getArgs()[0];

    auto Expect = [&](bool OK, llvm::StringRef What) {
      if (!OK)
        Diags.error(Arg->getBeginLoc(),
                    "'" + Callee.Spelling + "' expects " + What +
                        " argument, not " + quoted(T),
                    {Arg->getRange()});
      return OK;
    };

    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Abs:
    case BuiltinKind::Sqr:
      if (!Expect(T->isNumeric(), "a numeric"))
        return errorType();
      return T->isReal() ? Ctx.getRealType() : Ctx.getIntegerType();
    case BuiltinKind::Odd:
      return Expect(T->isIntegerLike(), "an integer") ? Ctx.getBooleanType()
                                                      : errorType();
    case BuiltinKind::Ord:
      return Expect(T->isOrdinal(), "an ordinal") ? Ctx.getIntegerType()
                                                  : errorType();
    case BuiltinKind::Chr:
      return Expect(T->isIntegerLike(), "an integer") ? Ctx.getCharType()
                                                      : errorType();
    case BuiltinKind::Succ:
    case BuiltinKind::Pred:
      return Expect(T->isOrdinal(), "an ordinal") ? T->getBase() : errorType();
    case BuiltinKind::Sin: case BuiltinKind::Cos: case BuiltinKind::Exp:
    case BuiltinKind::Ln: case BuiltinKind::Sqrt: case BuiltinKind::Arctan:
      return Expect(T->isNumeric(), "a numeric") ? Ctx.getRealType()
                                                 : errorType();
    case BuiltinKind::Trunc:
    case BuiltinKind::Round:
      return Expect(T->isNumeric(), "a real") ? Ctx.getIntegerType()
                                              : errorType();
    default:
      llvm_unreachable("not a builtin function");
    }
  }

public:
  Checker(ASTContext &Ctx, DiagnosticsEngine &Diags, const LangOptions &LO)
      : Ctx(Ctx), Diags(Diags), LangOpts(LO) {
    declareStandardScope();
  }

  void checkProgram(ProgramDecl &P) {
    checkProgramParams(P);
    Scope ProgramScope(&Standard);
    Cur = &ProgramScope;
    checkBlock(*P.getBlock());
    Cur = &Standard;
  }

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  void visitEmptyStmt(EmptyStmt *) {}

  void visitCompoundStmt(CompoundStmt *S) {
    for (Stmt *Child : S->getBody()) {
      if (Diags.shouldStop())
        return;
      visitStmt(Child);
    }
  }

  void visitProcCallStmt(ProcCallStmt *S) {
    const Identifier &Callee = S->getCallee();
    Decl *D = Cur->lookup(Callee.Name);
    if (!D) {
      diagUndeclared(Callee);
      return;
    }
    S->setDecl(D);

    if (auto *BD = llvm::dyn_cast<BuiltinDecl>(D); BD && BD->isProcedure()) {
      switch (BD->getBuiltinKind()) {
      case BuiltinKind::Write:   return checkWrite(S, /*IsWriteln=*/false);
      case BuiltinKind::Writeln: return checkWrite(S, /*IsWriteln=*/true);
      case BuiltinKind::Read:    return checkRead(S, /*IsReadln=*/false);
      case BuiltinKind::Readln:  return checkRead(S, /*IsReadln=*/true);
      case BuiltinKind::New:
      case BuiltinKind::Dispose: return checkNewDispose(S);
      default: llvm_unreachable("not a builtin procedure");
      }
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isProcedure())
      return checkCallArgs(R, Callee, S->getArgs(), S->getRange());

    if (llvm::isa<BuiltinDecl>(D) || llvm::isa<RoutineDecl>(D))
      Diags.error(Callee.Loc,
                  "function '" + Callee.Spelling +
                      "' cannot be called as a statement; use its result",
                  {Callee.getRange()});
    else
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' is a " + D->getKindName() +
                      ", not a procedure",
                  {Callee.getRange()});
    noteDeclaredHere(D);
  }

  /// Resolves an assignment target that is a plain name: a variable, a field
  /// (inside with), or the result of an enclosing function.
  Type *checkNameTarget(NameRefExpr *Target) {
    const Identifier &Id = Target->getName();
    WithContext *W = nullptr;
    Decl *D = Cur->lookup(Id.Name, &W);
    if (!D) {
      diagUndeclared(Id);
      return errorType();
    }

    if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isFunction()) {
      for (RoutineDecl *Enc = CurRoutine; Enc; Enc = Enc->getParent())
        if (Enc == R) {
          Target->setDecl(R->getResultVar());
          ResultAssigned.insert(R);
          return R->getResultType();
        }
      Diags.error(Id.Loc,
                  "cannot assign to function '" + Id.Spelling +
                      "' outside its body",
                  {Id.getRange()});
      return errorType();
    }

    Target->setDecl(D);
    Target->setWith(W);
    if (auto *V = llvm::dyn_cast<VarDecl>(D)) {
      checkNotForControlVar(Target);
      return V->getType() ? V->getType() : errorType();
    }
    if (auto *F = llvm::dyn_cast<FieldDecl>(D))
      return F->getType() ? F->getType() : errorType();

    Diags.error(Id.Loc,
                "cannot assign to " + llvm::Twine(D->getKindName()) + " '" +
                    Id.Spelling + "'",
                {Id.getRange()});
    noteDeclaredHere(D);
    return errorType();
  }

  void visitAssignStmt(AssignStmt *S) {
    Expr *Target = S->getTarget();
    Type *TargetTy;
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Target)) {
      TargetTy = checkNameTarget(N);
      N->setType(TargetTy);
    } else {
      TargetTy = check(Target);
      if (!TargetTy->isError() && !isVariableReference(Target)) {
        Diags.error(Target->getBeginLoc(), "expression is not assignable",
                    {Target->getRange()});
        TargetTy = errorType();
      }
    }

    Type *ValueTy = check(S->getValue());
    if (TargetTy->isError() || ValueTy->isError())
      return;
    if (!isAssignable(TargetTy, ValueTy))
      Diags.error(S->getAssignLoc(),
                  "assigning to " + quoted(TargetTy) +
                      " from incompatible type " + quoted(ValueTy),
                  {Target->getRange(), S->getValue()->getRange()});
  }

  void visitIfStmt(IfStmt *S) {
    checkCondition(S->getCond(), "if");
    visitStmt(S->getThen());
    if (S->getElse())
      visitStmt(S->getElse());
  }

  void visitWhileStmt(WhileStmt *S) {
    checkCondition(S->getCond(), "while");
    visitStmt(S->getBody());
  }

  void visitRepeatStmt(RepeatStmt *S) {
    for (Stmt *Child : S->getBody())
      visitStmt(Child);
    checkCondition(S->getCond(), "until");
  }

  void visitForStmt(ForStmt *S) {
    NameRefExpr *VarRef = S->getVar();
    const Identifier &Id = VarRef->getName();
    Type *VarTy = errorType();
    bool Usable = false;

    if (Decl *D = Cur->lookup(Id.Name)) {
      VarRef->setDecl(D);
      auto *V = llvm::dyn_cast<VarDecl>(D);
      if (!V) {
        Diags.error(Id.Loc,
                    "'for' loop control variable must be a variable, but '" +
                        Id.Spelling + "' is a " + D->getKindName(),
                    {Id.getRange()});
        noteDeclaredHere(D);
      } else if (V->getType() && !V->getType()->isError()) {
        VarTy = V->getType();
        if (!VarTy->isOrdinal()) {
          Diags.error(Id.Loc,
                      "'for' loop control variable must be of an ordinal "
                      "type, not " + quoted(VarTy),
                      {Id.getRange()});
        } else if (isISO() && (V->getOwner() != CurRoutine || V->isVarParam())) {
          // ISO 7185 6.8.3.9: declared in the closest enclosing block.
          Diags.error(Id.Loc,
                      "'for' loop control variable '" + Id.Spelling +
                          "' must be a local variable of the enclosing block",
                      {Id.getRange()});
          noteDeclaredHere(V);
        } else if (!checkNotForControlVar(VarRef)) {
          Usable = true;
        }
      }
    } else {
      diagUndeclared(Id);
    }
    VarRef->setType(VarTy);

    for (Expr *Bound : {S->getInit(), S->getFinal()}) {
      Type *T = check(Bound);
      if (Usable && !T->isError() && !areCompatible(T, VarTy))
        Diags.error(Bound->getBeginLoc(),
                    "'for' loop " +
                        llvm::Twine(Bound == S->getInit() ? "initial"
                                                          : "final") +
                        " value of type " + quoted(T) +
                        " does not match control variable of type " +
                        quoted(VarTy),
                    {Bound->getRange()});
    }

    if (Usable)
      ActiveFors.push_back(S);
    visitStmt(S->getBody());
    if (Usable)
      ActiveFors.pop_back();
  }

  void visitCaseStmt(CaseStmt *S) {
    Type *SelTy = check(S->getSelector());
    if (!SelTy->isError() && !SelTy->isOrdinal()) {
      Diags.error(S->getSelector()->getBeginLoc(),
                  "'case' selector must be of an ordinal type, not " +
                      quoted(SelTy),
                  {S->getSelector()->getRange()});
      SelTy = errorType();
    }
    // Labels are checked against the selector's host type: a subrange
    // selector may be compared with any value of its host.
    Type *LabelTy = SelTy->isError() ? SelTy : SelTy->getBase();

    // ISO 7185 6.8.3.5: no value may appear in more than one label.
    llvm::SmallVector<CaseLabel *, 16> Seen;
    for (CaseArm *Arm : S->getArms()) {
      for (CaseLabel *L : Arm->getLabels()) {
        if (!checkLabel(L, LabelTy, "case label") || SelTy->isError())
          continue;
        for (CaseLabel *Prev : Seen) {
          if (L->LoValue > Prev->HiValue || L->HiValue < Prev->LoValue)
            continue;
          int64_t Dup = std::max(L->LoValue, Prev->LoValue);
          Diags.error(L->Lo->getBeginLoc(),
                      "duplicate case label value " + formatValue(SelTy, Dup),
                      {L->getRange()});
          Diags.note(Prev->Lo->getBeginLoc(), "previous case label is here");
          break;
        }
        Seen.push_back(L);
      }
      visitStmt(Arm->getBody());
    }
    for (Stmt *Child : S->getElseBody())
      visitStmt(Child);
  }

  void visitWithStmt(WithStmt *S) {
    // Each record opens a scope; they nest left to right.
    llvm::SmallVector<std::unique_ptr<Scope>, 2> Scopes;
    Scope *Saved = Cur;
    for (WithContext *W : S->getRecords()) {
      Type *T = check(W->Record);
      if (T->isError())
        continue;
      auto *RT = llvm::dyn_cast<RecordType>(T);
      if (!RT) {
        Diags.error(W->Record->getBeginLoc(),
                    "'with' requires a record, not a value of type " +
                        quoted(T),
                    {W->Record->getRange()});
        continue;
      }
      if (!isVariableReference(W->Record)) {
        Diags.error(W->Record->getBeginLoc(),
                    "'with' requires a record variable",
                    {W->Record->getRange()});
        continue;
      }
      Scopes.push_back(std::make_unique<Scope>(Cur, RT, W));
      Cur = Scopes.back().get();
    }
    visitStmt(S->getBody());
    Cur = Saved;
  }

  //===--------------------------------------------------------------------===//
  // Expressions (each returns the expression's type)
  //===--------------------------------------------------------------------===//

  Type *visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    if (E->getValue() > static_cast<uint64_t>(PascalMaxInt))
      Diags.error(E->getBeginLoc(),
                  "integer literal exceeds maxint (2147483647)",
                  {E->getRange()});
    return Ctx.getIntegerType();
  }

  Type *visitRealLiteralExpr(RealLiteralExpr *) { return Ctx.getRealType(); }

  Type *visitStringLiteralExpr(StringLiteralExpr *E) {
    return E->isCharLiteral() ? Ctx.getCharType()
                              : Ctx.getStringLiteralType(E->getValue().size());
  }

  Type *visitNameRefExpr(NameRefExpr *E) {
    const Identifier &Id = E->getName();
    WithContext *W = nullptr;
    Decl *D = Cur->lookup(Id.Name, &W);
    if (!D) {
      diagUndeclared(Id);
      return errorType();
    }
    E->setDecl(D);
    E->setWith(W);

    switch (D->getKind()) {
    case Decl::DK_Var: {
      Type *T = llvm::cast<VarDecl>(D)->getType();
      return T ? T : errorType();
    }
    case Decl::DK_Field: {
      Type *T = llvm::cast<FieldDecl>(D)->getType();
      return T ? T : errorType();
    }
    case Decl::DK_Const: {
      Type *T = llvm::cast<ConstDecl>(D)->getType();
      return T ? T : errorType();
    }
    case Decl::DK_Routine: {
      auto *R = llvm::cast<RoutineDecl>(D);
      if (R->isProcedure()) {
        Diags.error(Id.Loc,
                    "procedure '" + Id.Spelling + "' does not return a value",
                    {Id.getRange()});
        return errorType();
      }
      if (!R->getParams().empty()) {
        Diags.error(Id.Loc,
                    "missing arguments in call to '" + Id.Spelling + "'",
                    {Id.getRange()});
        noteDeclaredHere(R);
        return errorType();
      }
      return R->getResultType(); // a call without arguments
    }
    case Decl::DK_Builtin:
      if (llvm::cast<BuiltinDecl>(D)->isFunction()) {
        Diags.error(Id.Loc, "missing argument in call to '" + Id.Spelling + "'",
                    {Id.getRange()});
        return errorType();
      }
      Diags.error(Id.Loc,
                  "procedure '" + Id.Spelling + "' does not return a value",
                  {Id.getRange()});
      return errorType();
    default:
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' is a " + D->getKindName() +
                      ", not a value",
                  {Id.getRange()});
      noteDeclaredHere(D);
      return errorType();
    }
  }

  Type *visitParenExpr(ParenExpr *E) { return check(E->getInner()); }

  Type *visitUnaryExpr(UnaryExpr *E) {
    Type *T = check(E->getOperand());
    if (T->isError())
      return errorType();
    if (E->getOp() == UnaryOp::Not) {
      if (T->getBase()->isBoolean())
        return Ctx.getBooleanType();
    } else if (T->isNumeric()) {
      return T->isReal() ? Ctx.getRealType() : Ctx.getIntegerType();
    }
    Diags.error(E->getOpLoc(),
                llvm::Twine("invalid operand to '") + getOpSpelling(E->getOp()) +
                    "' (" + quoted(T) + ")",
                {E->getOperand()->getRange()});
    return errorType();
  }

  Type *visitBinaryExpr(BinaryExpr *E) {
    Type *L = check(E->getLHS());
    Type *R = check(E->getRHS());
    if (L->isError() || R->isError())
      return errorType();

    BinaryOp Op = E->getOp();
    if (L->isSet() && R->isSet() && Op != BinaryOp::In) {
      if (!areCompatible(L, R)) {
        Diags.error(E->getOpLoc(),
                    "incompatible set types " + quoted(L) + " and " + quoted(R),
                    {E->getLHS()->getRange(), E->getRHS()->getRange()});
        return errorType();
      }
      switch (Op) {
      case BinaryOp::Add: case BinaryOp::Sub: case BinaryOp::Mul:
        return llvm::cast<SetType>(L)->isEmptySet() ? R : L;
      case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Le: case BinaryOp::Ge:
        return Ctx.getBooleanType(); // <= is "subset of", >= "superset of"
      default:
        break;
      }
    }
    if ((Op == BinaryOp::Eq || Op == BinaryOp::Ne) &&
        (L->isPointer() || L->isNil()) && (R->isPointer() || R->isNil()) &&
        (areCompatible(L, R) || (L->isNil() && R->isNil())))
      return Ctx.getBooleanType();

    switch (Op) {
    case BinaryOp::Add: case BinaryOp::Sub: case BinaryOp::Mul:
      if (L->isNumeric() && R->isNumeric())
        return L->isReal() || R->isReal() ? Ctx.getRealType()
                                          : Ctx.getIntegerType();
      break;
    case BinaryOp::RealDiv:
      if (L->isNumeric() && R->isNumeric())
        return Ctx.getRealType();
      break;
    case BinaryOp::IntDiv: case BinaryOp::Mod:
      if (L->isIntegerLike() && R->isIntegerLike())
        return Ctx.getIntegerType();
      break;
    case BinaryOp::And: case BinaryOp::Or:
      if (L->getBase()->isBoolean() && R->getBase()->isBoolean())
        return Ctx.getBooleanType();
      break;
    case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt:
    case BinaryOp::Le: case BinaryOp::Gt: case BinaryOp::Ge:
      if (L->isNumeric() && R->isNumeric())
        return Ctx.getBooleanType();
      if (L->isOrdinal() && R->isOrdinal() && areCompatible(L, R))
        return Ctx.getBooleanType();
      if (L->isStringType() && R->isStringType()) {
        if (areCompatible(L, R))
          return Ctx.getBooleanType();
        Diags.error(E->getOpLoc(),
                    "cannot compare strings of different lengths (" +
                        std::to_string(L->getStringLength()) + " and " +
                        std::to_string(R->getStringLength()) + ")",
                    {E->getLHS()->getRange(), E->getRHS()->getRange()});
        return errorType();
      }
      break;
    case BinaryOp::In:
      if (L->isOrdinal() && R->isSet() &&
          (llvm::cast<SetType>(R)->isEmptySet() ||
           llvm::cast<SetType>(R)->getElementType()->getBase() == L->getBase()))
        return Ctx.getBooleanType();
      break;
    }

    Diags.error(E->getOpLoc(),
                llvm::Twine("invalid operands to '") + getOpSpelling(Op) +
                    "' (" + quoted(L) + " and " + quoted(R) + ")",
                {E->getLHS()->getRange(), E->getRHS()->getRange()});
    return errorType();
  }

  Type *visitCallExpr(CallExpr *E) {
    const Identifier &Callee = E->getCallee();
    Decl *D = Cur->lookup(Callee.Name);
    if (!D) {
      diagUndeclared(Callee);
      for (Expr *Arg : E->getArgs())
        check(Arg);
      return errorType();
    }
    E->setDecl(D);

    if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) {
      if (R->isFunction()) {
        checkCallArgs(R, Callee, E->getArgs(), E->getRange());
        return R->getResultType();
      }
      for (Expr *Arg : E->getArgs())
        check(Arg);
      Diags.error(Callee.Loc,
                  "procedure '" + Callee.Spelling + "' does not return a value",
                  {Callee.getRange()});
      return errorType();
    }

    llvm::SmallVector<Type *, 4> ArgTypes;
    for (Expr *Arg : E->getArgs())
      ArgTypes.push_back(check(Arg));

    if (auto *BD = llvm::dyn_cast<BuiltinDecl>(D)) {
      if (BD->isFunction())
        return checkBuiltinFunction(E, BD, ArgTypes);
      Diags.error(Callee.Loc,
                  "procedure '" + Callee.Spelling +
                      "' does not return a value",
                  {Callee.getRange()});
      return errorType();
    }
    if (auto *V = llvm::dyn_cast<VarDecl>(D);
        V && V->getType() && V->getType()->isArray())
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' is an array; use '[' ']' to "
                  "index it",
                  {Callee.getRange()});
    else
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' is a " + D->getKindName() +
                      ", not a function",
                  {Callee.getRange()});
    noteDeclaredHere(D);
    return errorType();
  }

  Type *visitFormattedExpr(FormattedExpr *E) {
    check(E->getValue());
    Diags.error(E->getWidth()->getBeginLoc(),
                "field widths are only allowed in 'write' and 'writeln'",
                {E->getWidth()->getRange()});
    return errorType();
  }

  Type *visitIndexExpr(IndexExpr *E) {
    Type *BT = check(E->getBase());
    Type *IT = check(E->getIndex());
    if (BT->isError() || IT->isError())
      return errorType();
    auto *AT = llvm::dyn_cast<ArrayType>(BT);
    if (!AT) {
      Diags.error(E->getIndex()->getBeginLoc(),
                  "subscripted value is not an array (type " + quoted(BT) +
                      ")",
                  {E->getBase()->getRange()});
      return errorType();
    }
    Type *Idx = AT->getIndexType();
    if (!areCompatible(IT, Idx)) {
      Diags.error(E->getIndex()->getBeginLoc(),
                  "array index of type " + quoted(IT) +
                      " does not match index type " + quoted(Idx),
                  {E->getIndex()->getRange()});
      return errorType();
    }
    // Constant indices are checked at compile time.
    if (auto C = fold(E->getIndex())) {
      OrdinalBounds B = getOrdinalBounds(Idx);
      if (C->Value.Int < B.Lo || C->Value.Int > B.Hi)
        Diags.error(E->getIndex()->getBeginLoc(),
                    "array index " + formatValue(Idx, C->Value.Int) +
                        " is out of bounds " + formatValue(Idx, B.Lo) + ".." +
                        formatValue(Idx, B.Hi),
                    {E->getIndex()->getRange()});
    }
    return AT->getElementType();
  }

  Type *visitNilExpr(NilExpr *) { return Ctx.getNilType(); }

  Type *visitDerefExpr(DerefExpr *E) {
    Type *T = check(E->getBase());
    if (T->isError())
      return errorType();
    if (auto *P = llvm::dyn_cast<PointerType>(T))
      return P->getPointee() ? P->getPointee() : errorType();
    Diags.error(E->getBase()->getBeginLoc(),
                "'^' applied to a value of type " + quoted(T) +
                    ", which is not a pointer",
                {E->getBase()->getRange()});
    return errorType();
  }

  Type *visitSetCtorExpr(SetCtorExpr *E) {
    Type *Base = nullptr;
    bool OK = true;
    for (const SetElement &El : E->getElements()) {
      for (Expr *X : {El.Lo, El.Hi}) {
        if (!X)
          continue;
        Type *T = check(X);
        if (T->isError()) {
          OK = false;
          continue;
        }
        if (!T->isOrdinal()) {
          Diags.error(X->getBeginLoc(),
                      "set elements must be of an ordinal type, not " +
                          quoted(T),
                      {X->getRange()});
          OK = false;
          continue;
        }
        if (!Base) {
          Base = T->getBase();
        } else if (T->getBase() != Base) {
          Diags.error(X->getBeginLoc(),
                      "set element of type " + quoted(T) +
                          " does not match earlier elements of type " +
                          quoted(Base),
                      {X->getRange()});
          OK = false;
          continue;
        }
        if (auto C = fold(X); C && (C->Value.Int < 0 || C->Value.Int > 255)) {
          Diags.error(X->getBeginLoc(),
                      "set element " + formatValue(T, C->Value.Int) +
                          " is outside 0..255",
                      {X->getRange()});
          OK = false;
        }
      }
    }
    return OK ? Ctx.getSetConstructorType(Base) : errorType();
  }

  Type *visitFieldExpr(FieldExpr *E) {
    Type *BT = check(E->getBase());
    if (BT->isError())
      return errorType();
    auto *RT = llvm::dyn_cast<RecordType>(BT);
    const Identifier &F = E->getField();
    if (!RT) {
      Diags.error(F.Loc,
                  "'.' applied to a value of type " + quoted(BT) +
                      ", which is not a record",
                  {E->getBase()->getRange()});
      return errorType();
    }
    FieldDecl *FD = RT->findField(F.Name);
    if (!FD) {
      Diags.error(F.Loc,
                  "no field named '" + F.Spelling + "' in " + quoted(RT),
                  {F.getRange()});
      return errorType();
    }
    E->setFieldDecl(FD);
    return FD->getType() ? FD->getType() : errorType();
  }
};

} // namespace

bool Sema::check(ProgramDecl &P) {
  unsigned ErrorsBefore = Diags.getNumErrors();
  Checker(Ctx, Diags, LangOpts).checkProgram(P);
  return Diags.getNumErrors() == ErrorsBefore && !Diags.shouldStop();
}
