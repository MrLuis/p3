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
  (void)Name; // every name of PLAN.md 4.5 is implemented
  return false;
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
  /// Units: interface and implementation scopes, by canonical unit name.
  std::vector<std::unique_ptr<Scope>> UnitScopes;
  llvm::StringMap<Scope *> UnitInterfaces;
  Scope *CurUnitInterface = nullptr; ///< while checking an implementation
  RoutineDecl *CurRoutine = nullptr; ///< null in the program block

  /// Enclosing for statements, innermost last. Their control variables must
  /// not be assigned in the loop body (ISO 7185 6.8.3.9 "threatened").
  llvm::SmallVector<ForStmt *, 4> ActiveFors;

  unsigned LoopDepth = 0; ///< for break / continue

  /// Functions whose result has been assigned somewhere in their body.
  llvm::DenseSet<RoutineDecl *> ResultAssigned;

  /// Variables referenced anywhere (read or written).
  llvm::DenseSet<Decl *> Used;

  /// `^T` where T was not declared yet: resolved at the end of the type
  /// section (ISO 7185 6.2.2.9 allows pointers to later types).
  llvm::SmallVector<std::pair<PointerTypeSpec *, PointerType *>, 4>
      PendingPointers;

  // goto / labels (ISO 7185 6.8.1). Containers holds the enclosing statement
  // sequences and labeled statements of the statement being checked; each
  // goto remembers them and is validated once all labels are defined.
  llvm::SmallVector<const void *, 16> Containers;
  const void *DirectSequence = nullptr; ///< sequence of the current statement
  CompoundStmt *RoutineBody = nullptr;  ///< outermost sequence of the block
  struct PendingGoto {
    GotoStmt *S;
    llvm::SmallVector<const void *, 16> Containers;
    RoutineDecl *From;
  };
  std::vector<PendingGoto> PendingGotos;
  llvm::DenseMap<RoutineDecl *, unsigned> NonLocalLabelCount;

  static std::string labelKey(llvm::StringRef Name) { return "#" + Name.str(); }

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
    Ty("text", Ctx.getTextType());
    if (!isISO()) {
      Ty("pointer", Ctx.getUntypedPointerType());
      // Turbo/FPC integer types; longint is integer (32-bit), and double is
      // real.
      Ty("longint", Ctx.getIntegerType());
      Ty("int64", Ctx.getInt64Type());
      Ty("cardinal", Ctx.getCardinalType());
      Ty("longword", Ctx.getCardinalType());
      Ty("byte", Ctx.getByteType());
      Ty("word", Ctx.getWordType());
      Ty("shortint", Ctx.getShortIntType());
      Ty("smallint", Ctx.getSmallIntType());
      Ty("double", Ctx.getRealType());
      // pchar: a pointer to char, for C strings (external routines).
      auto *PChar = Ctx.create<PointerType>(Ctx.getCharType());
      PChar->setDeclName("pchar");
      Ty("pchar", PChar);
    }

    // The standard files (ISO 7185 6.10).
    for (llvm::StringRef N : {"input", "output"}) {
      auto *V = Ctx.create<VarDecl>(predeclaredName(N), nullptr);
      V->setType(Ctx.getTextType());
      Standard.insert(N, V);
    }

    auto Const = [&](llvm::StringRef N, Type *T, int64_t V) {
      Standard.insert(N, Ctx.create<ConstDecl>(SourceLocation(),
                                               predeclaredName(N), T,
                                               ConstValue::ordinal(V)));
    };
    Const("false", Ctx.getBooleanType(), 0);
    Const("true", Ctx.getBooleanType(), 1);
    Const("maxint", Ctx.getIntegerType(), PascalMaxInt);

#define BUILTIN_PROC(ID, NAME)                                                 \
  if (!isTPOnly(BuiltinKind::ID) || !isISO())                                  \
    Standard.insert(NAME, Ctx.create<BuiltinDecl>(predeclaredName(NAME),       \
                                                  BuiltinKind::ID, true));
#define BUILTIN_FUNC(ID, NAME)                                                 \
  if (!isTPOnly(BuiltinKind::ID) || !isISO())                                  \
    Standard.insert(NAME, Ctx.create<BuiltinDecl>(predeclaredName(NAME),       \
                                                  BuiltinKind::ID, false));
#include "pcc/AST/Builtins.def"
  }

  /// Turbo/FPC builtins, predeclared only with --mode=tp.
  static bool isTPOnly(BuiltinKind K) {
    switch (K) {
    case BuiltinKind::Break: case BuiltinKind::Continue: case BuiltinKind::Exit:
    case BuiltinKind::Inc: case BuiltinKind::Dec: case BuiltinKind::Delete:
    case BuiltinKind::Insert: case BuiltinKind::Length: case BuiltinKind::Copy:
    case BuiltinKind::Pos: case BuiltinKind::Concat: case BuiltinKind::UpCase:
    case BuiltinKind::GetMem: case BuiltinKind::FreeMem:
    case BuiltinKind::ReallocMem: case BuiltinKind::FillChar:
    case BuiltinKind::Move: case BuiltinKind::Halt: case BuiltinKind::SizeOf:
    case BuiltinKind::Low: case BuiltinKind::High: case BuiltinKind::Assigned:
    case BuiltinKind::ParamCount: case BuiltinKind::ParamStr:
      return true;
    default:
      return false;
    }
  }

  //===--------------------------------------------------------------------===//
  // Diagnostics helpers
  //===--------------------------------------------------------------------===//

  /// The visible name closest to Name (edit distance), for "did you mean".
  std::string suggest(llvm::StringRef Name) const {
    // Allow one edit for short names, else a third of the length (at least
    // two, so a swapped pair of letters is found).
    unsigned Best =
        (Name.size() <= 3 ? 1 : std::max<unsigned>(2, Name.size() / 3)) + 1;
    std::string Result;
    // Inner scopes win; within a scope, the closest name, and among equally
    // close ones the alphabetically first (independent of hash order, so
    // the self-hosted compiler can match it).
    for (const Scope *S = Cur; S; S = S->getParent()) {
      unsigned ScopeBest = Best;
      std::string ScopeResult;
      S->forEachName([&](llvm::StringRef Candidate) {
        if (Candidate.starts_with("#") || Candidate == Name)
          return;
        unsigned D = Name.edit_distance(Candidate, true, Best - 1);
        if (D < ScopeBest || (D == ScopeBest && D < Best && Candidate < ScopeResult)) {
          ScopeBest = D;
          ScopeResult = Candidate.str();
        }
      });
      if (ScopeBest < Best) {
        Best = ScopeBest;
        Result = ScopeResult;
      }
    }
    return Result;
  }

  std::string didYouMean(llvm::StringRef Name) const {
    std::string S = suggest(Name);
    if (S.empty())
      return "";
    // Show the name as it was declared (Point, not point).
    if (Decl *D = Cur->lookup(S); D && !D->getName().Spelling.empty())
      S = D->getName().Spelling.str();
    return "; did you mean '" + S + "'?";
  }

  void diagUndeclared(const Identifier &Id) {
    Diags.error(Id.Loc,
                "use of undeclared identifier '" + Id.Spelling + "'" +
                    didYouMean(Id.Name),
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

  //===--------------------------------------------------------------------===//
  // Integer types (Turbo/FPC rules; ISO has only `integer`)
  //===--------------------------------------------------------------------===//

  /// Integer operands narrower than 32 bits widen to integer.
  Type *promoteInteger(Type *T) {
    auto *I = llvm::cast<IntegerType>(T->getBase());
    return I->getBits() < 32 ? Ctx.getIntegerType() : I;
  }

  /// Is E a constant that a cardinal can hold?
  bool isCardinalConstant(Expr *E) {
    auto C = fold(E);
    return C && C->Value.K == ConstValue::Ordinal && C->Value.Int >= 0 &&
           C->Value.Int <= UINT32_MAX;
  }

  /// The type integer operands LE and RE are converted to for an operator:
  /// int64 if either is int64; cardinal if both are unsigned (or the other
  /// is a constant that fits); int64 when cardinal meets a signed value
  /// (so no value is lost, as in FPC); integer otherwise.
  Type *integerOperandType(Expr *LE, Type *L, Expr *RE, Type *R) {
    Type *PL = promoteInteger(L), *PR = promoteInteger(R);
    Type *I64 = Ctx.getInt64Type(), *Card = Ctx.getCardinalType();
    if (PL == I64 || PR == I64)
      return I64;
    if (PL == Card && PR == Card)
      return Card;
    if (PL == Card || PR == Card) {
      Expr *Other = PL == Card ? RE : LE;
      Type *OtherTy = PL == Card ? R : L;
      if (!OtherTy->isSignedIntegerLike() || isCardinalConstant(Other))
        return Card;
      return I64;
    }
    return Ctx.getIntegerType();
  }

  /// Type-checks E once (later calls return the cached type).
  Type *check(Expr *E) {
    if (Type *Cached = E->getType())
      return Cached;
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
    if (A->isPointer() && B->isPointer() &&
        (llvm::cast<PointerType>(A)->isUntyped() ||
         llvm::cast<PointerType>(B)->isUntyped()))
      return true; // Turbo/FPC untyped `pointer`
    if (A->isSet() && B->isSet()) {
      auto *SA = llvm::cast<SetType>(A), *SB = llvm::cast<SetType>(B);
      return SA->isEmptySet() || SB->isEmptySet() ||
             haveSameOrdinalHost(SA->getElementType(), SB->getElementType());
    }
    if (A->isOrdinal() && B->isOrdinal())
      return haveSameOrdinalHost(A, B);
    if (A->isStringType() && B->isStringType())
      return A->getStringLength() == B->getStringLength();
    return false;
  }

  /// Can a value of type Value be assigned to a variable of type Target?
  static bool isAssignable(Type *Target, Type *Value) {
    if (containsFile(Target) || containsFile(Value))
      return false; // files cannot be copied
    if (Target == Value)
      return true;
    if (Target->isReal() && Value->isIntegerLike())
      return true;
    if (Target->isPointer() && (Value->isNil() || Value->isPointer()))
      return areCompatible(Target, Value);
    if (Target->isShortString() && Value->isStringish())
      return true; // truncated to the target's length at runtime
    if (Target->isSet() && Value->isSet())
      return areCompatible(Target, Value); // range-checked at runtime
    // Procedural values (Turbo/FPC): congruent headings, or nil.
    if (auto *TR = llvm::dyn_cast<RoutineType>(Target)) {
      if (Value->isNil())
        return true;
      auto *VR = llvm::dyn_cast<RoutineType>(Value);
      return VR && congruent(TR->getParams(), TR->getResult(), VR->getParams(),
                             VR->getResult());
    }
    if (Target->isOrdinal() && Value->isOrdinal())
      return haveSameOrdinalHost(Target, Value); // range-checked at runtime
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

  /// The `const` parameter a designator is part of (x, x[i], x.f; not x^,
  /// whose target is a different variable), if any.
  static VarDecl *constParamRoot(Expr *E) {
    while (true) {
      if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
        E = I->getBase();
      } else if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
        E = F->getBase();
      } else if (auto *P = llvm::dyn_cast<ParenExpr>(E)) {
        E = P->getInner();
      } else {
        auto *N = llvm::dyn_cast<NameRefExpr>(E);
        auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(N->getDecl()) : nullptr;
        return V && V->isConstParam() ? V : nullptr;
      }
    }
  }

  /// Reports a change to a `const` parameter. Returns false if reported.
  bool checkModifiable(Expr *E) {
    VarDecl *V = constParamRoot(E);
    if (!V)
      return true;
    Diags.error(E->getBeginLoc(),
                "cannot modify constant parameter '" + V->getName().Spelling +
                    "'",
                {E->getRange()});
    noteDeclaredHere(V);
    return false;
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
        return Constant{T, ConstValue::ordinal(static_cast<int64_t>(
                               0 - static_cast<uint64_t>(V->Value.Int)))};
      case UnaryOp::Not: // boolean not, or bitwise in the operand's type
        if (T->getBase()->isBoolean())
          return Constant{T, ConstValue::ordinal(!V->Value.Int)};
        return Constant{T, ConstValue::ordinal(castOrdinalValue(~V->Value.Int, T))};
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
      // Wrap around in 64 bits (no undefined behavior); results that do not
      // fit their type are reported by evaluateConstant.
      auto UA = static_cast<uint64_t>(A), UC = static_cast<uint64_t>(C);
      bool Wide = T->isIntegerLike() &&
                  llvm::cast<IntegerType>(T->getBase())->getBits() == 64;
      switch (B->getOp()) {
      case BinaryOp::Add: return IsReal ? Real(X + Y) : Ord(static_cast<int64_t>(UA + UC));
      case BinaryOp::Sub: return IsReal ? Real(X - Y) : Ord(static_cast<int64_t>(UA - UC));
      case BinaryOp::Mul: return IsReal ? Real(X * Y) : Ord(static_cast<int64_t>(UA * UC));
      case BinaryOp::RealDiv:
        if (Y == 0)
          return std::nullopt;
        return Real(X / Y);
      case BinaryOp::IntDiv:
        if (C == 0 || (A == INT64_MIN && C == -1))
          return std::nullopt;
        return Ord(A / C);
      case BinaryOp::Mod:
        if (C <= 0)
          return std::nullopt;
        return Ord(((A % C) + C) % C);
      case BinaryOp::And: return T->isBoolean() ? Ord(A && C) : Ord(A & C);
      case BinaryOp::Or:  return T->isBoolean() ? Ord(A || C) : Ord(A | C);
      case BinaryOp::Xor: return Ord(A ^ C);
      case BinaryOp::Shl:
        if (Wide)
          return Ord(static_cast<int64_t>(UA << (C & 63)));
        return Ord(static_cast<int32_t>(static_cast<uint32_t>(A) << (C & 31)));
      case BinaryOp::Shr:
        if (Wide)
          return Ord(static_cast<int64_t>(UA >> (C & 63)));
        return Ord(static_cast<int32_t>(static_cast<uint32_t>(A) >> (C & 31)));
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
      if (llvm::isa_and_nonnull<TypeDecl>(Call->getDecl())) { // T(x)
        auto V = fold(Call->getArgs()[0]);
        if (!V || V->Value.K != ConstValue::Ordinal || !T->isOrdinal())
          return std::nullopt;
        return Constant{T, ConstValue::ordinal(castOrdinalValue(V->Value.Int, T))};
      }
      auto *BD = llvm::dyn_cast_or_null<BuiltinDecl>(Call->getDecl());
      if (!BD || Call->getArgs().size() != 1)
        return std::nullopt;
      if (BD->getBuiltinKind() == BuiltinKind::Low ||
          BD->getBuiltinKind() == BuiltinKind::High) {
        bool IsHigh = BD->getBuiltinKind() == BuiltinKind::High;
        Type *AT = Call->getArgs()[0]->getType();
        if (auto *SS = llvm::dyn_cast<ShortStringType>(AT))
          return Constant{T, ConstValue::ordinal(IsHigh ? SS->getMaxLength() : 0)};
        if (auto *A = llvm::dyn_cast<ArrayType>(AT))
          AT = A->getIndexType();
        OrdinalBounds Bd = getOrdinalBounds(AT);
        return Constant{T, ConstValue::ordinal(IsHigh ? Bd.Hi : Bd.Lo)};
      }
      auto V = fold(Call->getArgs()[0]);
      if (!V || V->Value.K == ConstValue::String)
        return std::nullopt;
      auto Ord = [&](int64_t X) { return Constant{T, ConstValue::ordinal(X)}; };
      // Integer results wrap around in 64 bits (no undefined behavior).
      auto U = static_cast<uint64_t>(V->Value.Int);
      switch (BD->getBuiltinKind()) {
      case BuiltinKind::Ord: case BuiltinKind::Chr: return Ord(V->Value.Int);
      case BuiltinKind::Succ: return Ord(static_cast<int64_t>(U + 1));
      case BuiltinKind::Pred: return Ord(static_cast<int64_t>(U - 1));
      case BuiltinKind::Odd:  return Ord(V->Value.Int & 1);
      case BuiltinKind::Abs:
        if (V->Value.K == ConstValue::Real)
          return Constant{T, ConstValue::real(std::fabs(V->Value.RealVal))};
        return Ord(V->Value.Int < 0 ? static_cast<int64_t>(0 - U) : V->Value.Int);
      case BuiltinKind::Sqr:
        if (V->Value.K == ConstValue::Real)
          return Constant{T, ConstValue::real(V->Value.RealVal * V->Value.RealVal)};
        return Ord(static_cast<int64_t>(U * U));
      case BuiltinKind::Trunc:
      case BuiltinKind::Round: {
        // ISO rounds halves away from zero; Turbo/FPC to even (as codegen).
        double X = asReal(*V);
        if (BD->getBuiltinKind() == BuiltinKind::Round)
          X = isISO() ? std::round(X) : std::nearbyint(X);
        else
          X = std::trunc(X);
        if (!(X >= -9223372036854775808.0 && X < 9223372036854775808.0))
          return std::nullopt; // out of range: left to the runtime check
        return Ord(static_cast<int64_t>(X));
      }
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
    if (C->Value.K == ConstValue::Ordinal && C->Ty->isIntegerLike()) {
      OrdinalBounds Bd = getOrdinalBounds(C->Ty->getBase());
      if (C->Value.Int < Bd.Lo || C->Value.Int > Bd.Hi) {
        // Turbo/FPC: integer constant expressions widen to int64.
        if (!isISO() && C->Ty->getBase()->isSignedIntegerLike()) {
          C->Ty = Ctx.getInt64Type();
          return C;
        }
        Diags.error(E->getBeginLoc(),
                    "constant value is out of range for " +
                        C->Ty->getBase()->getName(),
                    {E->getRange()});
        return std::nullopt;
      }
    }
    return C;
  }

  /// The value of a typecast T(V): integer types wrap to their width
  /// (byte(300) = 44); other ordinal types keep the value.
  static int64_t castOrdinalValue(int64_t V, Type *T) {
    auto *I = llvm::dyn_cast<IntegerType>(T->getBase());
    if (!I || I->getBits() == 64)
      return V;
    uint64_t Mask = (uint64_t(1) << I->getBits()) - 1;
    uint64_t U = static_cast<uint64_t>(V) & Mask;
    if (I->isSigned() && (U >> (I->getBits() - 1)))
      U |= ~Mask; // sign-extend
    return static_cast<int64_t>(U);
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
          Diags.error(Id.Loc, "unknown type name '" + Id.Spelling + "'" +
                        didYouMean(Id.Name),
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
      if (!haveSameOrdinalHost(Lo->Ty, Hi->Ty)) {
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
      // Integer bounds of different types (1..5000000000): the wider host.
      Type *Host = Lo->Ty->getBase();
      if (Host->isInteger() &&
          llvm::cast<IntegerType>(Hi->Ty->getBase())->getBits() >
              llvm::cast<IntegerType>(Host)->getBits())
        Host = Hi->Ty->getBase();
      return Ctx.create<SubrangeType>(Host, Lo->Value.Int, Hi->Value.Int);
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
        } else if (Idx->isInteger() &&
                   llvm::cast<IntegerType>(Idx)->getBits() >= 32) {
          Diags.error(IdxTS->getBeginLoc(),
                      "array index type " + quoted(Idx) +
                          " is too large; use a subrange such as 1..100",
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
      // Always resolved at the end of the type section: `^T` binds to a T
      // declared later in the same block even if an outer T is visible.
      auto *P = Ctx.create<PointerType>(nullptr);
      PendingPointers.push_back({llvm::cast<PointerTypeSpec>(TS), P});
      return P;
    }

    case TypeSpec::TS_Routine: {
      auto *RS = llvm::cast<RoutineTypeSpec>(TS);
      for (VarDecl *P : RS->getParams())
        P->setType(resolveType(P->getTypeSpec()));
      Type *Result = nullptr;
      if (RS->isFunction()) {
        Result = resolveType(RS->getResult());
        if (!Result->isError() && !Result->isValidResultType()) {
          Diags.error(RS->getResult()->getBeginLoc(),
                      "function result type must be a simple or pointer "
                      "type, not " + quoted(Result),
                      {RS->getResult()->getRange()});
          Result = errorType();
        }
      }
      return Ctx.create<RoutineType>(RS->getParams(), Result);
    }

    case TypeSpec::TS_String: {
      auto *SS = llvm::cast<StringTypeSpec>(TS);
      if (!SS->getMaxLength())
        return Ctx.getShortStringType(255);
      auto C = evaluateConstant(SS->getMaxLength(), "string length");
      if (!C)
        return errorType();
      if (!C->Ty->isIntegerLike() || C->Value.Int < 1 || C->Value.Int > 255) {
        Diags.error(SS->getMaxLength()->getBeginLoc(),
                    "string length must be an integer in 1..255",
                    {SS->getMaxLength()->getRange()});
        return errorType();
      }
      return Ctx.getShortStringType(static_cast<unsigned>(C->Value.Int));
    }

    case TypeSpec::TS_File: {
      auto *FS = llvm::cast<FileTypeSpec>(TS);
      Type *Elem = resolveType(FS->getElement());
      if (Elem->isError())
        return errorType();
      if (containsFile(Elem)) {
        Diags.error(FS->getElement()->getBeginLoc(),
                    "the components of a file cannot be files",
                    {FS->getElement()->getRange()});
        return errorType();
      }
      return Ctx.create<FileType>(Elem, false);
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
    if (Expr *Init = V->getInit()) {
      // Typed constants are static: they keep their value across calls.
      V->setOwner(nullptr);
      Used.insert(V);
      ConstValue Folded;
      if (!V->getType()->isError() && checkConstInit(Init, V->getType(), Folded))
        V->setInitValue(Folded);
    }
    declare(V);
  }

  /// Is T an array of char (any index type) that a string constant of
  /// length N initializes (Turbo/FPC)?
  static bool isCharArrayOfLength(Type *T, uint64_t N) {
    auto *A = llvm::dyn_cast<ArrayType>(T);
    if (!A || !A->getElementType()->isChar())
      return false;
    OrdinalBounds Bd = getOrdinalBounds(A->getIndexType());
    return static_cast<uint64_t>(Bd.Hi - Bd.Lo + 1) == N;
  }

  /// Checks the initial value of a typed constant of type T: a constant
  /// expression (folded into Folded) or, for arrays and records, an
  /// aggregate whose elements are checked recursively.
  bool checkConstInit(Expr *Init, Type *T, ConstValue &Folded) {
    if (auto *Agg = llvm::dyn_cast<AggregateExpr>(Init)) {
      Agg->setType(T);
      auto Elems = Agg->getElements();
      if (auto *A = llvm::dyn_cast<ArrayType>(T)) {
        OrdinalBounds Bd = getOrdinalBounds(A->getIndexType());
        uint64_t Want = static_cast<uint64_t>(Bd.Hi - Bd.Lo + 1);
        if (Agg->isRecordForm() || Elems.size() != Want) {
          Diags.error(Init->getBeginLoc(),
                      "initializer for " + quoted(T) + " must list " +
                          std::to_string(Want) + " element" +
                          (Want == 1 ? "" : "s") + ", separated by ','",
                      {Init->getRange()});
          return false;
        }
        bool OK = true;
        for (AggregateElement *El : Elems)
          OK &= checkConstInit(El->Value, A->getElementType(), El->Folded);
        return OK;
      }
      if (auto *R = llvm::dyn_cast<RecordType>(T)) {
        if (R->getFieldList()->Variant) {
          Diags.error(Init->getBeginLoc(),
                      "typed constants of records with variant parts are not "
                      "supported",
                      {Init->getRange()});
          return false;
        }
        if (!Agg->isRecordForm()) {
          Diags.error(Init->getBeginLoc(),
                      "initializer for record " + quoted(T) +
                          " must name its fields: (field: value; ...)",
                      {Init->getRange()});
          return false;
        }
        bool OK = true;
        llvm::StringSet<> Seen;
        for (AggregateElement *El : Elems) {
          FieldDecl *F = R->findField(El->Field.Name);
          if (!F) {
            Diags.error(El->Field.Loc,
                        "no field named '" + El->Field.Spelling + "' in " +
                            quoted(T),
                        {El->Field.getRange()});
            OK = false;
            continue;
          }
          if (!Seen.insert(El->Field.Name).second) {
            Diags.error(El->Field.Loc,
                        "field '" + El->Field.Spelling + "' is initialized twice",
                        {El->Field.getRange()});
            OK = false;
            continue;
          }
          OK &= checkConstInit(El->Value, F->getType(), El->Folded);
        }
        return OK;
      }
      if (Elems.size() == 1 && !Agg->isRecordForm()) // (1 + 2) for a scalar
        return checkConstInit(Elems[0]->Value, T, Folded);
      Diags.error(Init->getBeginLoc(),
                  "a parenthesized list cannot initialize " + quoted(T),
                  {Init->getRange()});
      return false;
    }

    auto C = evaluateConstant(Init, "typed constant value");
    if (!C)
      return false;
    bool CharArray = C->Value.K == ConstValue::String &&
                     isCharArrayOfLength(T, C->Value.Str.size());
    if (!CharArray && !(T->isSimple() || T->isShortString() || T->isStringType())) {
      Diags.error(Init->getBeginLoc(),
                  "typed constants of type " + quoted(T) +
                      " need a parenthesized initializer",
                  {Init->getRange()});
      return false;
    }
    if (!CharArray && !isAssignable(T, C->Ty)) {
      Diags.error(Init->getBeginLoc(),
                  "cannot initialize " + quoted(T) + " with a value of type " +
                      quoted(C->Ty),
                  {Init->getRange()});
      return false;
    }
    if (C->Value.K == ConstValue::Ordinal && T->isOrdinal()) {
      OrdinalBounds Bd = getOrdinalBounds(T);
      if (C->Value.Int < Bd.Lo || C->Value.Int > Bd.Hi) {
        Diags.error(Init->getBeginLoc(),
                    "value " + formatValue(C->Ty, C->Value.Int) +
                        " is out of range for " + quoted(T),
                    {Init->getRange()});
        return false;
      }
    }
    Folded = C->Value;
    return true;
  }

  /// Resolves a routine's parameter and result types.
  void resolveSignature(RoutineDecl *R) {
    for (VarDecl *P : R->getParams()) {
      P->setOwner(R);
      P->setType(resolveType(P->getTypeSpec()));
      if (!P->isVarParam() && containsFile(P->getType()))
        Diags.error(P->getLocation(),
                    "file parameter '" + P->getName().Spelling +
                        "' must be a 'var' parameter",
                    {P->getName().getRange()});
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
      // Turbo/FPC also return strings and records (without files).
      if (!ResultTy->isError() && !ResultTy->isValidResultType() &&
          !ResultTy->isShortString() &&
          !(!isISO() && ResultTy->isRecord() && !containsFile(ResultTy))) {
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
    auto *Fwd = llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookupOwn(Id.Name));
    if (!Fwd && CurUnitInterface && !CurRoutine) // an interface heading
      Fwd = llvm::dyn_cast_or_null<RoutineDecl>(
          CurUnitInterface->lookupOwn(Id.Name));
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
    if (R->isExternal())
      checkExternal(R);
    if (R->getBody())
      checkRoutineBody(R);
  }

  /// External (C) routines: declared at the outermost level, with
  /// parameters and result of types that have a C equivalent.
  void checkExternal(RoutineDecl *R) {
    if (CurRoutine)
      Diags.error(R->getName().Loc,
                  "external routine '" + R->getName().Spelling +
                      "' must be declared at program or unit level",
                  {R->getName().getRange()});
    auto CType = [](Type *T) {
      return T->isError() || T->isReal() || T->isPointer() ||
             (T->isOrdinal() && !T->getBase()->isBoolean());
    };
    for (VarDecl *P : R->getParams())
      if (P->getType() && !P->isVarParam() && !CType(P->getType()))
        Diags.error(P->getLocation(),
                    "parameter '" + P->getName().Spelling + "' of external routine '" +
                        R->getName().Spelling + "' has type " +
                        quoted(P->getType()) +
                        "; use integer, real, char, pointer or var parameters",
                    {P->getName().getRange()});
    if (R->isFunction() && !CType(R->getResultType()))
      Diags.error(R->getName().Loc,
                  "external function '" + R->getName().Spelling +
                      "' cannot return " + quoted(R->getResultType()),
                  {R->getName().getRange()});
  }

  /// A string constant passed where a pchar is expected (Turbo/FPC): the
  /// compiler supplies a NUL-terminated copy.
  bool isPCharLiteral(Type *Target, Expr *Value) {
    auto *P = llvm::dyn_cast<PointerType>(Target);
    if (isISO() || !P || P->isUntyped() || !P->getPointee() ||
        !P->getPointee()->isChar())
      return false;
    if (llvm::isa<StringLiteralExpr>(Value))
      return true;
    auto *N = llvm::dyn_cast<NameRefExpr>(Value);
    auto *C = N ? llvm::dyn_cast_or_null<ConstDecl>(N->getDecl()) : nullptr;
    return C && (C->getValue().K == ConstValue::String ||
                 (C->getType() && C->getType()->getBase()->isChar()));
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
      Decl *D = Cur->lookup(Id.Name);
      auto *TD = llvm::dyn_cast_or_null<TypeDecl>(D);
      if (TD && TD->getType()) {
        P->setPointee(TD->getType());
        continue;
      }
      if (D)
        Diags.error(Id.Loc,
                    "'" + Id.Spelling + "' is a " + D->getKindName() +
                        ", not a type",
                    {Id.getRange()});
      else
        Diags.error(Id.Loc, "unknown type name '" + Id.Spelling + "'" +
                        didYouMean(Id.Name),
                    {Id.getRange()});
      P->setPointee(errorType());
    }
    PendingPointers.clear();
  }

  void checkBlock(Block &B) {
    declareAll(B);
    finishBlock(B, /*WarnUnused=*/true);
  }

  void declareAll(Block &B) {
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
      case Decl::DK_Label: {
        auto *L = llvm::cast<LabelDecl>(D);
        L->setOwner(CurRoutine);
        if (Decl *Prev = Cur->insert(labelKey(L->getName().Name), L)) {
          Diags.error(L->getLocation(),
                      "label " + L->getName().Name + " is declared twice",
                      {L->getName().getRange()});
          Diags.note(Prev->getLocation(), "previous declaration is here");
        }
        break;
      }
      default: llvm_unreachable("unexpected declaration in block");
      }
    }
    resolvePendingPointers();
  }

  void finishBlock(Block &B, bool WarnUnused) {
    // ISO 7185 6.1.4: every forward declaration needs a definition in the
    // same block.
    for (Decl *D : B.getDecls())
      if (auto *R = llvm::dyn_cast<RoutineDecl>(D);
          R && R->isForward() && !R->getBody())
        Diags.error(R->getName().Loc,
                    "'" + R->getName().Spelling +
                        "' is declared forward but never defined",
                    {R->getName().getRange()});
    CompoundStmt *SavedBody = RoutineBody;
    RoutineBody = B.getBody();
    visitStmt(B.getBody());
    RoutineBody = SavedBody;

    for (Decl *D : B.getDecls())
      if (auto *V = llvm::dyn_cast<VarDecl>(D); WarnUnused && V && !Used.count(V))
        Diags.warning("unused-variable", V->getLocation(),
                      "variable '" + V->getName().Spelling +
                          "' is declared but never used",
                      {V->getName().getRange()});

    // ISO 7185 6.8.1: every declared label prefixes exactly one statement.
    for (Decl *D : B.getDecls())
      if (auto *L = llvm::dyn_cast<LabelDecl>(D);
          L && !L->getTarget() &&
          Cur->lookupLocal(labelKey(L->getName().Name)) == L) // not a duplicate
        Diags.error(L->getLocation(),
                    "label " + L->getName().Name +
                        " is declared but does not label any statement",
                    {L->getName().getRange()});
  }

  void checkGotos() {
    for (PendingGoto &G : PendingGotos) {
      LabelDecl *L = G.S->getDecl();
      if (!L || !L->getTarget())
        continue;
      bool OK;
      if (L->getOwner() == G.From) {
        OK = llvm::is_contained(G.Containers, L->getTarget()) ||
             (L->getSequence() &&
              llvm::is_contained(G.Containers, L->getSequence()));
      } else {
        // A goto out of a nested routine may only target the outermost
        // statement sequence of the enclosing block.
        OK = L->isTopLevel();
        if (OK) {
          L->setNonLocal();
          if (!L->getIndex())
            L->setIndex(++NonLocalLabelCount[L->getOwner()]);
        }
      }
      if (!OK) {
        Diags.error(G.S->getBeginLoc(),
                    "'goto " + L->getName().Name +
                        "' jumps into a statement that does not contain it",
                    {G.S->getRange()});
        Diags.note(L->getTarget()->getBeginLoc(),
                   "label " + L->getName().Name +
                       " is here; it can only be reached from its own "
                       "statement sequence");
      }
    }
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
    }
  }

  /// ISO 7185 6.10: program parameters other than input/output must be
  /// declared as file variables in the program block. They are bound to the
  /// command-line arguments in order.
  void checkProgramParamDecls(ProgramDecl &P) {
    for (const Identifier &Param : P.getParams()) {
      if (Param.Name == "input" || Param.Name == "output")
        continue;
      Decl *D = Cur->lookupLocal(Param.Name);
      auto *V = llvm::dyn_cast_or_null<VarDecl>(D);
      if (V)
        Used.insert(V); // bound to a command-line argument
      if (V && V->getType() && (V->getType()->isFile() || V->getType()->isError()))
        continue;
      Diags.error(Param.Loc,
                  "program parameter '" + Param.Spelling +
                      "' must be declared as a file variable in the program",
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

  /// ISO 7185 6.6.3.6: a procedure/function passed as an argument must have
  /// congruent parameter lists (same kinds, identical types) and result.
  static bool congruent(llvm::ArrayRef<VarDecl *> A, Type *AResult,
                        llvm::ArrayRef<VarDecl *> B, Type *BResult) {
    if (A.size() != B.size() || (AResult == nullptr) != (BResult == nullptr) ||
        (AResult && AResult != BResult))
      return false;
    for (size_t I = 0; I < A.size(); ++I) {
      if (A[I]->getVarKind() != B[I]->getVarKind())
        return false;
      Type *TA = A[I]->getType(), *TB = B[I]->getType();
      auto *RA = llvm::dyn_cast_or_null<RoutineType>(TA);
      auto *RB = llvm::dyn_cast_or_null<RoutineType>(TB);
      if (RA && RB) {
        if (!congruent(RA->getParams(), RA->getResult(), RB->getParams(),
                       RB->getResult()))
          return false;
      } else if (TA != TB) {
        return false;
      }
    }
    return true;
  }

  /// An argument for a procedural/functional parameter: the name of a
  /// routine (or of another procedural parameter) with a congruent heading.
  void checkRoutineArg(Expr *Arg, VarDecl *P, RoutineType *Formal) {
    const char *Kind = Formal->isFunction() ? "function" : "procedure";
    auto *N = llvm::dyn_cast<NameRefExpr>(Arg);
    Decl *D = N ? Cur->lookup(N->getName().Name) : nullptr;
    llvm::ArrayRef<VarDecl *> Params;
    Type *Result = nullptr;
    bool Found = false;
    if (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D)) {
      Params = R->getParams();
      Result = R->isFunction() ? R->getResultType() : nullptr;
      Found = true;
    } else if (auto *V = llvm::dyn_cast_or_null<VarDecl>(D)) {
      if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(V->getType())) {
        Params = RT->getParams();
        Result = RT->getResult();
        Found = true;
      }
    }
    if (!Found) {
      Diags.error(Arg->getBeginLoc(),
                  llvm::Twine("argument for ") + Kind + " parameter '" +
                      P->getName().Spelling + "' must be the name of a " +
                      Kind +
                      (llvm::isa_and_nonnull<BuiltinDecl>(D)
                           ? " (required procedures and functions cannot be "
                             "passed)"
                           : ""),
                  {Arg->getRange()});
      return;
    }
    N->setDecl(D);
    N->setType(P->getType());
    if (!congruent(Formal->getParams(), Formal->getResult(), Params, Result))
      Diags.error(Arg->getBeginLoc(),
                  "'" + N->getName().Spelling +
                      "' does not match the heading of parameter '" +
                      P->getName().Spelling + "' (" + quoted(Formal) + ")",
                  {Arg->getRange()});
  }

  /// Checks the arguments of a call to a user routine.
  void checkCallArgs(RoutineDecl *R, const Identifier &Callee,
                     llvm::ArrayRef<Expr *> Args, SourceRange CallRange) {
    checkCallArgs(R->getParams(), R, Callee, Args, CallRange);
  }

  void checkCallArgs(llvm::ArrayRef<VarDecl *> Params, Decl *CalleeDecl,
                     const Identifier &Callee, llvm::ArrayRef<Expr *> Args,
                     SourceRange CallRange) {
    Decl *R = CalleeDecl;
    if (Args.size() != Params.size()) {
      Diags.error(Callee.Loc,
                  llvm::Twine(Args.size() < Params.size() ? "too few"
                                                          : "too many") +
                      " arguments in call to '" + Callee.Spelling +
                      "': expected " + llvm::Twine(Params.size()) + ", got " +
                      llvm::Twine(Args.size()),
                  {CallRange});
      if (R)
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
      if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(P->getType())) {
        checkRoutineArg(Arg, P, RT);
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
        if (!checkModifiable(Arg))
          continue;
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
      if (!isAssignable(PT, AT) && !isPCharLiteral(PT, Arg))
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
           T->getBase()->isBoolean() || T->isStringType() || T->isShortString();
  }

  /// If the first argument of read/write/... is a file, checks and returns
  /// it; otherwise returns null.
  FileType *checkFileArg(ProcCallStmt *S) {
    if (S->getArgs().empty() || llvm::isa<FormattedExpr>(S->getArgs()[0]))
      return nullptr;
    Expr *First = S->getArgs()[0];
    Type *T = check(First);
    auto *F = llvm::dyn_cast<FileType>(T);
    if (F && !isVariableReference(First)) {
      Diags.error(First->getBeginLoc(), "file argument must be a variable",
                  {First->getRange()});
      return nullptr;
    }
    return F;
  }

  void checkWrite(ProcCallStmt *S, bool IsWriteln) {
    const Identifier &Callee = S->getCallee();
    FileType *File = checkFileArg(S);
    auto Args = S->getArgs().drop_front(File ? 1 : 0);
    if (!IsWriteln && Args.empty())
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' requires at least one argument",
                  {Callee.getRange()});
    if (File && !File->isText()) {
      if (IsWriteln)
        Diags.error(Callee.Loc, "'writeln' requires a text file",
                    {Callee.getRange()});
      for (Expr *Arg : Args) {
        if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
          Diags.error(F->getWidth()->getBeginLoc(),
                      "field widths are only allowed for text files",
                      {F->getWidth()->getRange()});
          continue;
        }
        Type *T = check(Arg);
        if (!T->isError() && !isAssignable(File->getElementType(), T))
          Diags.error(Arg->getBeginLoc(),
                      "cannot write a value of type " + quoted(T) +
                          " to a " + quoted(File),
                      {Arg->getRange()});
      }
      return;
    }

    for (Expr *Arg : Args) {
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
    FileType *File = checkFileArg(S);
    auto Args = S->getArgs().drop_front(File ? 1 : 0);
    if (!IsReadln && Args.empty())
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' requires at least one argument",
                  {Callee.getRange()});
    if (File && !File->isText()) {
      if (IsReadln)
        Diags.error(Callee.Loc, "'readln' requires a text file",
                    {Callee.getRange()});
      for (Expr *Arg : Args) {
        Type *T = check(Arg);
        if (T->isError())
          continue;
        if (!isVariableReference(Arg))
          Diags.error(Arg->getBeginLoc(),
                      "argument to '" + Callee.Spelling + "' must be a variable",
                      {Arg->getRange()});
        else if (!isAssignable(T, File->getElementType()))
          Diags.error(Arg->getBeginLoc(),
                      "cannot read a component of " + quoted(File) +
                          " into a variable of type " + quoted(T),
                      {Arg->getRange()});
        else
          checkNotForControlVar(Arg);
      }
      return;
    }

    for (Expr *Arg : Args) {
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
      if (!T->isIntegerLike() && !T->getBase()->isChar() && !T->isReal() &&
          !T->isShortString())
        Diags.error(Arg->getBeginLoc(),
                    "cannot read a value of type " + quoted(T) +
                        " (only integer, real and char)",
                    {Arg->getRange()});
      else if (checkModifiable(Arg))
        checkNotForControlVar(Arg);
    }
  }

  /// reset/rewrite/get/put(f) and page[(f)].
  void checkFileProc(ProcCallStmt *S, bool IsPage) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    if (IsPage && Args.empty())
      return; // page(output)
    if (Args.size() != 1) {
      Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 1 file argument",
                  {S->getRange()});
      return;
    }
    Type *T = check(Args[0]);
    if (T->isError())
      return;
    if (!T->isFile() || !isVariableReference(Args[0]))
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects a file variable, not a " +
                      "value of type " + quoted(T),
                  {Args[0]->getRange()});
    else if (IsPage && !T->isTextFile())
      Diags.error(Args[0]->getBeginLoc(), "'page' requires a text file",
                  {Args[0]->getRange()});
  }

  /// pack(a, i, z): z := a[i..]   unpack(z, a, i): a[i..] := z
  void checkPackUnpack(ProcCallStmt *S, bool IsPack) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    if (Args.size() != 3) {
      Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 3 arguments",
                  {S->getRange()});
      return;
    }
    Expr *Unpacked = IsPack ? Args[0] : Args[1];
    Expr *Index = IsPack ? Args[1] : Args[2];
    Expr *Packed = IsPack ? Args[2] : Args[0];
    Type *UT = check(Unpacked), *IT = check(Index), *PT = check(Packed);
    if (UT->isError() || IT->isError() || PT->isError())
      return;
    auto *UA = llvm::dyn_cast<ArrayType>(UT);
    auto *PA = llvm::dyn_cast<ArrayType>(PT);
    if (!UA || UA->isPacked() || !isVariableReference(Unpacked)) {
      Diags.error(Unpacked->getBeginLoc(), "expected an unpacked array variable",
                  {Unpacked->getRange()});
      return;
    }
    if (!PA || !PA->isPacked() || !isVariableReference(Packed)) {
      Diags.error(Packed->getBeginLoc(), "expected a packed array variable",
                  {Packed->getRange()});
      return;
    }
    if (UA->getElementType() != PA->getElementType())
      Diags.error(Packed->getBeginLoc(),
                  "packed and unpacked arrays must have the same component type",
                  {Packed->getRange()});
    if (!areCompatible(IT, UA->getIndexType()))
      Diags.error(Index->getBeginLoc(),
                  "index of type " + quoted(IT) + " does not match " +
                      quoted(UA->getIndexType()),
                  {Index->getRange()});
  }

  /// new(p [, tag constants]) / dispose(p [, tag constants]). The tags
  /// select record variants (ISO 6.6.5.3); storage is always allocated for
  /// the largest variant, so they are only checked.
  /// delete(var s, index, count) / insert(source, var s, index)
  void checkDeleteInsert(ProcCallStmt *S, bool IsInsert) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    if (Args.size() != 3) {
      Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 3 arguments",
                  {S->getRange()});
      return;
    }
    Expr *Str = IsInsert ? Args[1] : Args[0];
    for (Expr *A : Args) {
      Type *T = check(A);
      if (T->isError())
        continue;
      if (A == Str) {
        if (!T->isShortString() || !isVariableReference(A))
          Diags.error(A->getBeginLoc(), "expected a string variable",
                      {A->getRange()});
        else
          checkModifiable(A);
      } else if (IsInsert && A == Args[0]) {
        if (!T->isStringish())
          Diags.error(A->getBeginLoc(), "expected a string", {A->getRange()});
      } else if (!T->isIntegerLike()) {
        Diags.error(A->getBeginLoc(), "expected an integer", {A->getRange()});
      }
    }
  }

  /// inc(x [, n]) / dec(x [, n]): x an ordinal variable, n an integer.
  void checkIncDec(ProcCallStmt *S) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    if (Args.empty() || Args.size() > 2) {
      Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 1 or 2 arguments",
                  {S->getRange()});
      return;
    }
    Type *T = check(Args[0]);
    if (!T->isError() && (!T->isOrdinal() || !isVariableReference(Args[0])))
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects an ordinal variable",
                  {Args[0]->getRange()});
    else if (!T->isError() && checkModifiable(Args[0]))
      checkNotForControlVar(Args[0]);
    if (Args.size() == 2) {
      Type *N = check(Args[1]);
      if (!N->isError() && !N->isIntegerLike())
        Diags.error(Args[1]->getBeginLoc(), "step must be an integer",
                    {Args[1]->getRange()});
    }
  }

  void checkNewDispose(ProcCallStmt *S, bool IsNew) {
    const Identifier &Callee = S->getCallee();
    if (S->getArgs().empty()) {
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects a pointer argument",
                  {S->getRange()});
      return;
    }
    Expr *Arg = S->getArgs()[0];
    Type *T = check(Arg);
    if (!T->isError()) {
      if (!T->isPointer())
        Diags.error(Arg->getBeginLoc(),
                    "'" + Callee.Spelling + "' expects a pointer " +
                        (IsNew ? "variable" : "") + ", not a value of type " +
                        quoted(T),
                    {Arg->getRange()});
      else if (IsNew && !isVariableReference(Arg))
        Diags.error(Arg->getBeginLoc(), "argument to 'new' must be a variable",
                    {Arg->getRange()});
      else if (IsNew)
        checkModifiable(Arg);
    }
    for (Expr *Tag : S->getArgs().drop_front())
      evaluateConstant(Tag, "variant selector");
  }

  //===--------------------------------------------------------------------===//
  // Built-in function calls
  //===--------------------------------------------------------------------===//

  Type *checkStringFunction(CallExpr *E, BuiltinDecl *BD,
                            llvm::ArrayRef<Type *> ArgTypes) {
    const Identifier &Callee = E->getCallee();
    auto Args = E->getArgs();
    for (Type *T : ArgTypes)
      if (T->isError())
        return errorType();
    auto Bad = [&](Expr *A, llvm::StringRef What) {
      Diags.error(A->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects " + What, {A->getRange()});
      return errorType();
    };
    auto Arity = [&](size_t N) {
      if (Args.size() == N)
        return true;
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects " + llvm::Twine(N) +
                      " argument" + (N == 1 ? "" : "s"),
                  {E->getRange()});
      return false;
    };
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Length:
      if (!Arity(1))
        return errorType();
      return ArgTypes[0]->isStringish() ? Ctx.getIntegerType()
                                        : Bad(Args[0], "a string");
    case BuiltinKind::UpCase:
      if (!Arity(1))
        return errorType();
      return ArgTypes[0]->getBase()->isChar() ? Ctx.getCharType()
                                              : Bad(Args[0], "a char");
    case BuiltinKind::Pos:
      if (!Arity(2))
        return errorType();
      for (size_t I = 0; I < 2; ++I)
        if (!ArgTypes[I]->isStringish())
          return Bad(Args[I], "strings");
      return Ctx.getIntegerType();
    case BuiltinKind::Copy:
      if (!Arity(3))
        return errorType();
      if (!ArgTypes[0]->isStringish())
        return Bad(Args[0], "a string");
      if (!ArgTypes[1]->isIntegerLike() || !ArgTypes[2]->isIntegerLike())
        return Bad(Args[1], "integer index and count");
      return Ctx.getShortStringType();
    default: // Concat
      if (Args.empty()) {
        Diags.error(Callee.Loc, "'concat' expects strings", {E->getRange()});
        return errorType();
      }
      for (size_t I = 0; I < Args.size(); ++I)
        if (!ArgTypes[I]->isStringish())
          return Bad(Args[I], "strings");
      return Ctx.getShortStringType();
    }
  }

  Type *checkBuiltinFunction(CallExpr *E, BuiltinDecl *BD,
                             llvm::ArrayRef<Type *> ArgTypes) {
    const Identifier &Callee = E->getCallee();
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Length: case BuiltinKind::Copy: case BuiltinKind::Pos:
    case BuiltinKind::Concat: case BuiltinKind::UpCase:
      return checkStringFunction(E, BD, ArgTypes);
    default:
      break;
    }
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

    if (BD->getBuiltinKind() == BuiltinKind::Eof ||
        BD->getBuiltinKind() == BuiltinKind::Eoln) {
      bool IsEoln = BD->getBuiltinKind() == BuiltinKind::Eoln;
      if (!T->isFile() || !isVariableReference(Arg) ||
          (IsEoln && !T->isTextFile())) {
        Diags.error(Arg->getBeginLoc(),
                    "'" + Callee.Spelling + "' expects a " +
                        (IsEoln ? "text " : "") + "file variable, not " +
                        quoted(T),
                    {Arg->getRange()});
        return errorType();
      }
      return Ctx.getBooleanType();
    }

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
      return T->isReal() ? Ctx.getRealType() : promoteInteger(T);
    case BuiltinKind::Odd:
      return Expect(T->isIntegerLike(), "an integer") ? Ctx.getBooleanType()
                                                      : errorType();
    case BuiltinKind::Ord:
      if (!Expect(T->isOrdinal(), "an ordinal"))
        return errorType();
      return T->isIntegerLike() ? promoteInteger(T) : Ctx.getIntegerType();
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
      if (!Expect(T->isNumeric(), "a real"))
        return errorType();
      // Turbo/FPC: the result is an int64.
      return isISO() ? Ctx.getIntegerType() : Ctx.getInt64Type();
    default:
      llvm_unreachable("not a builtin function");
    }
  }

  /// T(x) (Turbo/FPC): between ordinal types (integer types wrap, nothing
  /// is range-checked) and between pointer types.
  Type *checkTypecast(CallExpr *E, TypeDecl *TD) {
    Type *T = TD->getType() ? TD->getType() : errorType();
    auto Args = E->getArgs();
    if (Args.size() != 1) {
      Diags.error(E->getCallee().Loc,
                  "a typecast to " + quoted(T) + " takes exactly one value",
                  {E->getRange()});
      for (Expr *A : Args)
        check(A);
      return errorType();
    }
    Type *AT = check(Args[0]);
    if (T->isError() || AT->isError())
      return errorType();
    if ((T->isOrdinal() && AT->isOrdinal()) ||
        (T->isPointer() && (AT->isPointer() || AT->isNil())))
      return T;
    Diags.error(E->getBeginLoc(),
                "invalid typecast from " + quoted(AT) + " to " + quoted(T) +
                    " (only between ordinal types and between pointer types)",
                {E->getRange()});
    return errorType();
  }

  /// A type name or an expression as the argument of sizeof/low/high:
  /// returns its type (also stored on the argument).
  Type *checkTypeOrValue(Expr *Arg) {
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Arg))
      if (auto *TD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(N->getName().Name))) {
        N->setDecl(TD);
        Type *T = TD->getType() ? TD->getType() : errorType();
        N->setType(T);
        return T;
      }
    return check(Arg);
  }

  /// Built-in functions whose arguments are not ordinary values (type
  /// names, procedural variables) or that take no arguments. Returns
  /// nullopt for the others.
  std::optional<Type *> checkSpecialBuiltin(CallExpr *E, BuiltinDecl *BD) {
    const Identifier &Callee = E->getCallee();
    auto Args = E->getArgs();
    auto Arity = [&](size_t N) {
      if (Args.size() == N)
        return true;
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects " + llvm::Twine(N) +
                      " argument" + (N == 1 ? "" : "s"),
                  {E->getRange()});
      for (Expr *A : Args)
        check(A);
      return false;
    };
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::SizeOf: {
      if (!Arity(1))
        return errorType();
      Type *T = checkTypeOrValue(Args[0]);
      if (T->isError())
        return errorType();
      if (containsFile(T) || T->isStringLiteral() || T->isNil() ||
          (llvm::isa<SetType>(T) && llvm::cast<SetType>(T)->isEmptySet())) {
        Diags.error(Args[0]->getBeginLoc(),
                    "'sizeof' cannot be applied to " + quoted(T),
                    {Args[0]->getRange()});
        return errorType();
      }
      return Ctx.getInt64Type();
    }
    case BuiltinKind::Low:
    case BuiltinKind::High: {
      if (!Arity(1))
        return errorType();
      Type *T = checkTypeOrValue(Args[0]);
      if (T->isError())
        return errorType();
      if (T->isShortString())
        return Ctx.getIntegerType();
      if (auto *A = llvm::dyn_cast<ArrayType>(T))
        return A->getIndexType();
      if (T->isOrdinal())
        return T;
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling +
                      "' expects an ordinal type, an array or a string, not " +
                      quoted(T),
                  {Args[0]->getRange()});
      return errorType();
    }
    case BuiltinKind::Assigned: {
      if (!Arity(1))
        return errorType();
      Type *T;
      // A procedural variable is tested, not called.
      auto *N = llvm::dyn_cast<NameRefExpr>(Args[0]);
      auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(Cur->lookup(N->getName().Name))
                  : nullptr;
      if (V && llvm::isa_and_nonnull<RoutineType>(V->getType())) {
        N->setDecl(V);
        N->setType(V->getType());
        Used.insert(V);
        T = V->getType();
      } else {
        T = check(Args[0]);
      }
      if (T->isError())
        return errorType();
      if (!T->isPointer() && !llvm::isa<RoutineType>(T)) {
        Diags.error(Args[0]->getBeginLoc(),
                    "'assigned' expects a pointer or a procedural variable, not " +
                        quoted(T),
                    {Args[0]->getRange()});
        return errorType();
      }
      return Ctx.getBooleanType();
    }
    case BuiltinKind::ParamCount:
      if (!Arity(0))
        return errorType();
      return Ctx.getIntegerType();
    case BuiltinKind::ParamStr: {
      if (!Arity(1))
        return errorType();
      Type *T = check(Args[0]);
      if (!T->isError() && !T->isIntegerLike()) {
        Diags.error(Args[0]->getBeginLoc(),
                    "'paramstr' expects an integer, not " + quoted(T),
                    {Args[0]->getRange()});
        return errorType();
      }
      return Ctx.getShortStringType();
    }
    default:
      return std::nullopt;
    }
  }

  /// getmem(p, n) freemem(p) reallocmem(p, n) fillchar(x, n, v)
  /// move(src, dst, n) halt[(code)]  (Turbo/FPC)
  void checkMemoryProc(ProcCallStmt *S, BuiltinKind K) {
    const Identifier &Callee = S->getCallee();
    auto Args = S->getArgs();
    size_t Want = K == BuiltinKind::FreeMem ? 1
                  : K == BuiltinKind::GetMem || K == BuiltinKind::ReallocMem ? 2
                                                                            : 3;
    if (K == BuiltinKind::Halt) {
      if (Args.size() > 1)
        Diags.error(Callee.Loc, "'halt' expects at most 1 argument",
                    {S->getRange()});
      else if (Args.size() == 1) {
        Type *T = check(Args[0]);
        if (!T->isError() && !T->isIntegerLike())
          Diags.error(Args[0]->getBeginLoc(),
                      "'halt' expects an integer exit code, not " + quoted(T),
                      {Args[0]->getRange()});
      }
      return;
    }
    if (Args.size() != Want) {
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects " + llvm::Twine(Want) +
                      " argument" + (Want == 1 ? "" : "s"),
                  {S->getRange()});
      for (Expr *A : Args)
        check(A);
      return;
    }
    auto Expect = [&](Expr *A, bool OK, llvm::StringRef What) {
      if (!OK)
        Diags.error(A->getBeginLoc(),
                    "'" + Callee.Spelling + "' expects " + What,
                    {A->getRange()});
    };
    llvm::SmallVector<Type *, 3> T;
    for (Expr *A : Args)
      T.push_back(check(A));
    for (Type *X : T)
      if (X->isError())
        return;
    switch (K) {
    case BuiltinKind::GetMem:
    case BuiltinKind::ReallocMem:
      Expect(Args[0], T[0]->isPointer() && isVariableReference(Args[0]),
             "a pointer variable");
      Expect(Args[1], T[1]->isIntegerLike(), "an integer size");
      if (T[0]->isPointer() && isVariableReference(Args[0]))
        checkModifiable(Args[0]);
      return;
    case BuiltinKind::FreeMem:
      Expect(Args[0], T[0]->isPointer(), "a pointer");
      return;
    case BuiltinKind::FillChar:
      Expect(Args[0], isVariableReference(Args[0]) && !containsFile(T[0]),
             "a variable");
      Expect(Args[1], T[1]->isIntegerLike(), "an integer count");
      Expect(Args[2], T[2]->isOrdinal(), "an ordinal fill value");
      if (isVariableReference(Args[0]))
        checkModifiable(Args[0]);
      return;
    default: // Move
      Expect(Args[0], isVariableReference(Args[0]) && !containsFile(T[0]),
             "a source variable");
      Expect(Args[1], isVariableReference(Args[1]) && !containsFile(T[1]),
             "a destination variable");
      Expect(Args[2], T[2]->isIntegerLike(), "an integer count");
      if (isVariableReference(Args[1]))
        checkModifiable(Args[1]);
      return;
    }
  }

public:
  Checker(ASTContext &Ctx, DiagnosticsEngine &Diags, const LangOptions &LO)
      : Ctx(Ctx), Diags(Diags), LangOpts(LO) {
    declareStandardScope();
  }

  /// Makes the interfaces of the units in Uses visible in S.
  void importUnits(Scope &S, llvm::ArrayRef<Identifier> Uses) {
    for (const Identifier &U : Uses) {
      if (Scope *I = UnitInterfaces.lookup(U.Name))
        S.addImport(I);
      // Missing units are reported by the driver.
    }
  }

  void checkUnit(UnitDecl &U) {
    auto Iface = std::make_unique<Scope>(&Standard);
    importUnits(*Iface, U.getInterfaceUses());
    Cur = Iface.get();
    declareAll(*U.getInterface());

    auto Impl = std::make_unique<Scope>(Iface.get());
    importUnits(*Impl, U.getImplementationUses());
    Cur = Impl.get();
    CurUnitInterface = Iface.get();
    declareAll(*U.getImplementation());
    for (Decl *D : U.getInterface()->getDecls())
      if (auto *R = llvm::dyn_cast<RoutineDecl>(D);
          R && !R->getBody() && !R->isExternal())
        Diags.error(R->getName().Loc,
                    "'" + R->getName().Spelling +
                        "' is declared in the interface of unit '" +
                        U.getName().Spelling + "' but not implemented",
                    {R->getName().getRange()});
    finishBlock(*U.getImplementation(), /*WarnUnused=*/true);
    CurUnitInterface = nullptr;

    // Interface variables may be used by other units: never "unused".
    for (Decl *D : U.getInterface()->getDecls())
      Used.insert(D);
    UnitInterfaces[U.getName().Name] = Iface.get();
    UnitScopes.push_back(std::move(Iface));
    UnitScopes.push_back(std::move(Impl));
    Cur = &Standard;
  }

  void checkProgram(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units) {
    for (UnitDecl *U : Units)
      checkUnit(*U);
    checkProgramParams(P);
    Scope ProgramScope(&Standard);
    importUnits(ProgramScope, P.getUses());
    Cur = &ProgramScope;
    checkBlock(*P.getBlock());
    checkProgramParamDecls(P);
    checkGotos();
    Cur = &Standard;
  }

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  void visitEmptyStmt(EmptyStmt *) {}

  void visitSequence(const void *Seq, llvm::ArrayRef<Stmt *> Body) {
    Containers.push_back(Seq);
    for (Stmt *Child : Body) {
      if (Diags.shouldStop())
        break;
      DirectSequence = Seq;
      visitStmt(Child);
    }
    DirectSequence = nullptr;
    Containers.pop_back();
  }

  void visitCompoundStmt(CompoundStmt *S) { visitSequence(S, S->getBody()); }

  void visitLabeledStmt(LabeledStmt *S) {
    const void *Seq = DirectSequence;
    DirectSequence = nullptr;
    const Identifier &Id = S->getLabel();
    auto *L = llvm::dyn_cast_or_null<LabelDecl>(Cur->lookup(labelKey(Id.Name)));
    if (!L || L->getOwner() != CurRoutine) {
      Diags.error(Id.Loc,
                  "label " + Id.Name + " is not declared in this block",
                  {Id.getRange()});
    } else if (L->getTarget()) {
      Diags.error(Id.Loc, "label " + Id.Name + " labels more than one statement",
                  {Id.getRange()});
      Diags.note(L->getTarget()->getBeginLoc(), "previous use is here");
    } else {
      S->setDecl(L);
      L->setTarget(S, Seq, Seq && Seq == RoutineBody);
    }
    Containers.push_back(S);
    visitStmt(S->getBody());
    Containers.pop_back();
  }

  void visitGotoStmt(GotoStmt *S) {
    DirectSequence = nullptr;
    const Identifier &Id = S->getLabel();
    auto *L = llvm::dyn_cast_or_null<LabelDecl>(Cur->lookup(labelKey(Id.Name)));
    if (!L) {
      Diags.error(Id.Loc, "use of undeclared label " + Id.Name, {Id.getRange()});
      return;
    }
    S->setDecl(L);
    PendingGotos.push_back({S, Containers, CurRoutine});
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
      case BuiltinKind::New:     return checkNewDispose(S, /*IsNew=*/true);
      case BuiltinKind::Dispose: return checkNewDispose(S, /*IsNew=*/false);
      case BuiltinKind::Reset: case BuiltinKind::Rewrite:
      case BuiltinKind::Get: case BuiltinKind::Put: case BuiltinKind::Page:
        return checkFileProc(S, BD->getBuiltinKind() == BuiltinKind::Page);
      case BuiltinKind::Break: case BuiltinKind::Continue:
        if (!S->getArgs().empty())
          Diags.error(Callee.Loc, "'" + Callee.Spelling + "' takes no arguments",
                      {S->getRange()});
        else if (!LoopDepth)
          Diags.error(Callee.Loc,
                      "'" + Callee.Spelling + "' is only allowed inside a loop",
                      {Callee.getRange()});
        return;
      case BuiltinKind::Exit: {
        auto Args = S->getArgs();
        if (Args.empty())
          return;
        // exit(value) (Turbo/FPC): sets the function result, then exits.
        if (Args.size() > 1 || !CurRoutine || !CurRoutine->isFunction()) {
          Diags.error(Callee.Loc,
                      Args.size() > 1
                          ? "'exit' takes at most one argument"
                          : "'exit' with a value is only allowed in a function",
                      {S->getRange()});
          for (Expr *A : Args)
            check(A);
          return;
        }
        Type *T = check(Args[0]);
        Type *RT = CurRoutine->getResultType();
        ResultAssigned.insert(CurRoutine);
        if (!T->isError() && !RT->isError() && !isAssignable(RT, T))
          Diags.error(Args[0]->getBeginLoc(),
                      "cannot return a value of type " + quoted(T) +
                          " from a function of type " + quoted(RT),
                      {Args[0]->getRange()});
        return;
      }
      case BuiltinKind::Inc: case BuiltinKind::Dec:
        return checkIncDec(S);
      case BuiltinKind::Delete: case BuiltinKind::Insert:
        return checkDeleteInsert(S, BD->getBuiltinKind() == BuiltinKind::Insert);
      case BuiltinKind::Pack: case BuiltinKind::Unpack:
        return checkPackUnpack(S, BD->getBuiltinKind() == BuiltinKind::Pack);
      case BuiltinKind::GetMem: case BuiltinKind::FreeMem:
      case BuiltinKind::ReallocMem: case BuiltinKind::FillChar:
      case BuiltinKind::Move: case BuiltinKind::Halt:
        return checkMemoryProc(S, BD->getBuiltinKind());
      default: llvm_unreachable("not a builtin procedure");
      }
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D);
        R && (R->isProcedure() || !isISO())) // TP: discard a function result
      return checkCallArgs(R, Callee, S->getArgs(), S->getRange());
    if (auto *V = llvm::dyn_cast<VarDecl>(D))
      if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(V->getType());
          RT && !RT->isFunction())
        return checkCallArgs(RT->getParams(), V, Callee, S->getArgs(),
                             S->getRange());

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
    Used.insert(D);
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

  void visitIndirectCallStmt(IndirectCallStmt *S) {
    DirectSequence = nullptr;
    IndirectCallExpr *Call = S->getCall();
    if (RoutineType *RT = checkIndirectCallee(Call))
      Call->setType(RT->isFunction() ? RT->getResult() : RT);
    else
      Call->setType(errorType());
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

    Type *ValueTy = checkValueFor(TargetTy, S->getValue());
    if (TargetTy->isError() || ValueTy->isError())
      return;
    if (!checkModifiable(Target))
      return;
    if (containsFile(TargetTy)) {
      Diags.error(S->getAssignLoc(), "files cannot be assigned",
                  {Target->getRange()});
      return;
    }
    if (!isAssignable(TargetTy, ValueTy))
      Diags.error(S->getAssignLoc(),
                  "assigning to " + quoted(TargetTy) +
                      " from incompatible type " + quoted(ValueTy),
                  {Target->getRange(), S->getValue()->getRange()});
  }

  void visitIfStmt(IfStmt *S) {
    DirectSequence = nullptr;
    checkCondition(S->getCond(), "if");
    visitStmt(S->getThen());
    if (S->getElse())
      visitStmt(S->getElse());
  }

  void visitWhileStmt(WhileStmt *S) {
    DirectSequence = nullptr;
    checkCondition(S->getCond(), "while");
    ++LoopDepth;
    visitStmt(S->getBody());
    --LoopDepth;
  }

  void visitRepeatStmt(RepeatStmt *S) {
    ++LoopDepth;
    visitSequence(S, S->getBody());
    --LoopDepth;
    checkCondition(S->getCond(), "until");
  }

  void visitForStmt(ForStmt *S) {
    DirectSequence = nullptr;
    NameRefExpr *VarRef = S->getVar();
    const Identifier &Id = VarRef->getName();
    Type *VarTy = errorType();
    bool Usable = false;

    if (Decl *D = Cur->lookup(Id.Name)) {
      VarRef->setDecl(D);
      Used.insert(D);
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
    ++LoopDepth;
    visitStmt(S->getBody());
    --LoopDepth;
    if (Usable)
      ActiveFors.pop_back();
  }

  void visitCaseStmt(CaseStmt *S) {
    DirectSequence = nullptr;
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
    visitSequence(S->getElseBody().data(), S->getElseBody());
  }

  void visitWithStmt(WithStmt *S) {
    DirectSequence = nullptr;
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
    if (E->getValue() <= static_cast<uint64_t>(PascalMaxInt))
      return Ctx.getIntegerType();
    // Turbo/FPC: larger literals are int64 constants.
    if (!isISO() && E->getValue() <= static_cast<uint64_t>(INT64_MAX))
      return Ctx.getInt64Type();
    Diags.error(E->getBeginLoc(),
                isISO() ? "integer literal exceeds maxint (2147483647)"
                        : "integer literal exceeds the range of int64",
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
    Used.insert(D);

    switch (D->getKind()) {
    case Decl::DK_Var: {
      Type *T = llvm::cast<VarDecl>(D)->getType();
      if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(T)) {
        // A functional parameter without arguments is a call.
        if (RT->isFunction() && RT->getParams().empty())
          return RT->getResult();
        Diags.error(Id.Loc,
                    RT->isFunction()
                        ? "missing arguments in call to '" + Id.Spelling + "'"
                        : "procedure '" + Id.Spelling +
                              "' does not return a value",
                    {Id.getRange()});
        return errorType();
      }
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
      // eof / eoln without arguments test `input`.
      if (auto K = llvm::cast<BuiltinDecl>(D)->getBuiltinKind();
          K == BuiltinKind::Eof || K == BuiltinKind::Eoln)
        return Ctx.getBooleanType();
      if (llvm::cast<BuiltinDecl>(D)->getBuiltinKind() == BuiltinKind::ParamCount)
        return Ctx.getIntegerType();
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
      if (!isISO() && T->isIntegerLike()) // Turbo/FPC: bitwise not, same type
        return T->getBase();
    } else if (T->isNumeric()) {
      if (T->isReal())
        return Ctx.getRealType();
      Type *P = promoteInteger(T);
      // -cardinal can be negative (FPC: int64).
      return E->getOp() == UnaryOp::Minus && P == Ctx.getCardinalType()
                 ? Ctx.getInt64Type()
                 : P;
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
    // Turbo/FPC strings: + concatenates, comparisons work for any lengths.
    if (!isISO() && L->isStringish() && R->isStringish()) {
      if (Op == BinaryOp::Add && !(L->isNumeric() || R->isNumeric()))
        return Ctx.getShortStringType();
      if (isRelational(Op) && Op != BinaryOp::In && needsStringCompare(L, R))
        return Ctx.getBooleanType();
    }
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

    bool BothIntegers = L->isIntegerLike() && R->isIntegerLike();
    auto IntegerOp = [&]() {
      Type *T = integerOperandType(E->getLHS(), L, E->getRHS(), R);
      E->setOperandType(T);
      return T;
    };
    switch (Op) {
    case BinaryOp::Add: case BinaryOp::Sub: case BinaryOp::Mul:
      if (BothIntegers)
        return IntegerOp();
      if (L->isNumeric() && R->isNumeric())
        return Ctx.getRealType();
      break;
    case BinaryOp::RealDiv:
      if (L->isNumeric() && R->isNumeric())
        return Ctx.getRealType();
      break;
    case BinaryOp::IntDiv: case BinaryOp::Mod:
      if (BothIntegers)
        return IntegerOp();
      break;
    case BinaryOp::And: case BinaryOp::Or: case BinaryOp::Xor:
      if (L->getBase()->isBoolean() && R->getBase()->isBoolean() &&
          (Op != BinaryOp::Xor || !isISO()))
        return Ctx.getBooleanType();
      // Turbo/FPC: bitwise on integers.
      if (!isISO() && BothIntegers)
        return IntegerOp();
      break;
    case BinaryOp::Shl: case BinaryOp::Shr:
      if (BothIntegers) { // the left operand's (promoted) type
        E->setOperandType(promoteInteger(L));
        return E->getOperandType();
      }
      break;
    case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt:
    case BinaryOp::Le: case BinaryOp::Gt: case BinaryOp::Ge:
      if (BothIntegers) {
        IntegerOp();
        return Ctx.getBooleanType();
      }
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
           haveSameOrdinalHost(llvm::cast<SetType>(R)->getElementType(), L)))
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

    if (auto *V = llvm::dyn_cast<VarDecl>(D))
      if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(V->getType());
          RT && RT->isFunction()) {
        checkCallArgs(RT->getParams(), V, Callee, E->getArgs(), E->getRange());
        return RT->getResult();
      }

    if (auto *TD = llvm::dyn_cast<TypeDecl>(D); TD && !isISO())
      return checkTypecast(E, TD);
    if (auto *BD = llvm::dyn_cast<BuiltinDecl>(D))
      if (auto T = checkSpecialBuiltin(E, BD))
        return *T;

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
    if (auto *SS = llvm::dyn_cast<ShortStringType>(BT)) {
      // s[0] is the length byte, s[1..N] the characters.
      if (!IT->isIntegerLike()) {
        Diags.error(E->getIndex()->getBeginLoc(), "string index must be an integer",
                    {E->getIndex()->getRange()});
        return errorType();
      }
      if (auto C = fold(E->getIndex());
          C && (C->Value.Int < 0 || C->Value.Int > SS->getMaxLength()))
        Diags.error(E->getIndex()->getBeginLoc(),
                    "string index " + std::to_string(C->Value.Int) +
                        " is out of bounds 0.." +
                        std::to_string(SS->getMaxLength()),
                    {E->getIndex()->getRange()});
      return Ctx.getCharType();
    }
    if (auto *SL = llvm::dyn_cast<StringLiteralType>(BT); SL && !isISO()) {
      // Turbo/FPC: a string constant can be indexed, 1..N.
      if (!IT->isIntegerLike()) {
        Diags.error(E->getIndex()->getBeginLoc(), "string index must be an integer",
                    {E->getIndex()->getRange()});
        return errorType();
      }
      if (auto C = fold(E->getIndex());
          C && (C->Value.Int < 1 ||
                static_cast<uint64_t>(C->Value.Int) > SL->getLength()))
        Diags.error(E->getIndex()->getBeginLoc(),
                    "string index " + std::to_string(C->Value.Int) +
                        " is out of bounds 1.." + std::to_string(SL->getLength()),
                    {E->getIndex()->getRange()});
      return Ctx.getCharType();
    }
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

  /// Checks a value assigned to a target of type TargetTy. A procedural
  /// variable named as the value of a procedural target is copied, not
  /// called.
  Type *checkValueFor(Type *TargetTy, Expr *Value) {
    if (llvm::isa<RoutineType>(TargetTy))
      if (auto *N = llvm::dyn_cast<NameRefExpr>(Value)) {
        auto *V = llvm::dyn_cast_or_null<VarDecl>(Cur->lookup(N->getName().Name));
        if (V && llvm::isa_and_nonnull<RoutineType>(V->getType())) {
          N->setDecl(V);
          N->setType(V->getType());
          Used.insert(V);
          return V->getType();
        }
      }
    return check(Value);
  }

  /// The procedural type of a call through a designator, or null after a
  /// diagnostic.
  RoutineType *checkIndirectCallee(IndirectCallExpr *E) {
    Type *T = check(E->getCallee());
    if (T->isError())
      return nullptr;
    auto *RT = llvm::dyn_cast<RoutineType>(T);
    if (!RT) {
      Diags.error(E->getCallee()->getBeginLoc(),
                  "called value of type " + quoted(T) +
                      " is not a procedure or function",
                  {E->getCallee()->getRange()});
      for (Expr *A : E->getArgs())
        check(A);
      return nullptr;
    }
    Identifier Name;
    Name.Spelling = "procedural value";
    Name.Loc = E->getBeginLoc();
    checkCallArgs(RT->getParams(), nullptr, Name, E->getArgs(), E->getRange());
    return RT;
  }

  Type *visitIndirectCallExpr(IndirectCallExpr *E) {
    RoutineType *RT = checkIndirectCallee(E);
    if (!RT)
      return errorType();
    if (!RT->isFunction()) {
      Diags.error(E->getBeginLoc(), "procedure does not return a value",
                  {E->getRange()});
      return errorType();
    }
    return RT->getResult();
  }

  Type *visitAggregateExpr(AggregateExpr *E) {
    Diags.error(E->getBeginLoc(),
                "a parenthesized list is only allowed as the value of a typed "
                "constant",
                {E->getRange()});
    return errorType();
  }

  Type *visitAddrOfExpr(AddrOfExpr *E) {
    // @routine (Turbo/FPC): a procedural value.
    if (auto *N = llvm::dyn_cast<NameRefExpr>(E->getOperand()))
      if (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookup(N->getName().Name))) {
        N->setDecl(R);
        if (R->getParent() || R->isExternal()) {
          Diags.error(N->getBeginLoc(),
                      "'@" + N->getName().Spelling + "': only routines declared "
                      "at program or unit level can be procedural values",
                      {N->getRange()});
          return errorType();
        }
        auto *T = Ctx.create<RoutineType>(
            R->getParams(), R->isFunction() ? R->getResultType() : nullptr);
        N->setType(T);
        return T;
      }
    Type *T = check(E->getOperand());
    if (T->isError())
      return errorType();
    if (!isVariableReference(E->getOperand())) {
      Diags.error(E->getOperand()->getBeginLoc(),
                  "'@' requires a variable", {E->getOperand()->getRange()});
      return errorType();
    }
    return Ctx.getUntypedPointerType();
  }

  Type *visitDerefExpr(DerefExpr *E) {
    Type *T = check(E->getBase());
    if (T->isError())
      return errorType();
    if (auto *P = llvm::dyn_cast<PointerType>(T)) {
      if (P->isUntyped()) {
        Diags.error(E->getBase()->getBeginLoc(),
                    "cannot dereference an untyped pointer",
                    {E->getBase()->getRange()});
        return errorType();
      }
      return P->getPointee() ? P->getPointee() : errorType();
    }
    if (auto *F = llvm::dyn_cast<FileType>(T))
      return F->getElementType(); // the buffer variable f^
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
        } else if (!haveSameOrdinalHost(T, Base)) {
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

bool Sema::check(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units) {
  unsigned ErrorsBefore = Diags.getNumErrors();
  Checker(Ctx, Diags, LangOpts).checkProgram(P, Units);
  return Diags.getNumErrors() == ErrorsBefore && !Diags.shouldStop();
}
