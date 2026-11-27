// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- SemaExpr.cpp - Semantic analysis -------------------------------------===//

#include "Checker.h"

using namespace p3;
using namespace p3::detail;


//===--------------------------------------------------------------------===//
// Expressions (each returns the expression's type)
//===--------------------------------------------------------------------===//

Type *Checker::visitIntegerLiteralExpr(IntegerLiteralExpr *E){
  if (E->getValue() <= static_cast<uint64_t>(PascalMaxInt))
    return Ctx.getIntegerType();
  // Turbo/FPC: larger literals are int64 constants; beyond int64, qword
  // ($FFFFFFFFFFFFFFFF and the like: the bits of an int64, as in FPC).
  if (!isISO() && (E->getValue() <= static_cast<uint64_t>(INT64_MAX) ||
                   E->isBased()))
    return Ctx.getInt64Type();
  if (!isISO())
    return Ctx.getQWordType();
  Diags.error(E->getBeginLoc(), "integer literal exceeds maxint (2147483647)",
              {E->getRange()});
  return Ctx.getIntegerType();
}


Type *Checker::visitStringLiteralExpr(StringLiteralExpr *E){
  if (E->isWideCharLiteral())
    return Ctx.getWideCharType();
  return E->isCharLiteral() ? Ctx.getCharType()
                            : Ctx.getStringLiteralType(E->getValue().size());
}


Type *Checker::visitNameRefExpr(NameRefExpr *E){
  if (E == CompoundResultRef) {
    E->setDecl(CompoundResultVar);
    return CompoundResultVar->getType();
  }
  const Identifier &Id = E->getName();
  WithContext *W = nullptr;
  Decl *D = Cur->lookup(Id.Name, &W);
  noteLambdaUse(D, W, Id);
  if (!D) {
    diagUndeclared(Id);
    return errorType();
  }
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) { // objfpc: overloads
    D = resolveOverload(R, {}, Id, Id.getRange());
    if (!D)
      return errorType();
  }
  E->setDecl(D);
  E->setWith(W);
  Used.insert(D);
  warnHints(D, Id);
  checkScopeAccess(D, W, Id);

  switch (D->getKind()) {
  case Decl::DK_Var: {
    Type *T = llvm::cast<VarDecl>(D)->getType();
    if (llvm::cast<VarDecl>(D)->isOwned())
      ownedUse(llvm::cast<VarDecl>(D), Id.getRange());
    if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(T)) {
      // A functional parameter without arguments is a call.
      if (RT->isFunction() && acceptsArgCount(RT->getParams(), 0))
        return RT->getResult();
      if (RT->isOfObject()) // nothing else it could be: the value
        return T;
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
  case Decl::DK_Property:
    if (!checkArrayPropertyUse(llvm::cast<PropertyDecl>(D), E, Id))
      return errorType();
    return checkPropertyRead(llvm::cast<PropertyDecl>(D), E, Id);
  case Decl::DK_Routine: {
    auto *R = llvm::cast<RoutineDecl>(D);
    if (ClassType *C = constructedThroughSelf(R, W);
        C && acceptsArgCount(R->getParams(), 0))
      return C;
    if (R->isAsync() && acceptsArgCount(R->getParams(), 0))
      return R->getAsyncType(); // its task
    if (R->isIterator() && acceptsArgCount(R->getParams(), 0))
      return R->getIteratorType(); // its iterator
    if (R->isProcedure()) {
      Diags.error(Id.Loc,
                  "procedure '" + Id.Spelling + "' does not return a value",
                  {Id.getRange()});
      return errorType();
    }
    if (!acceptsArgCount(R->getParams(), 0)) {
      Diags.error(Id.Loc,
                  "missing arguments in call to '" + Id.Spelling + "'",
                  {Id.getRange()});
      noteDeclaredHere(R);
      return errorType();
    }
    return R->getResultType(); // a call without arguments
  }
  case Decl::DK_Type:
    // objfpc: a class name as a value is that class (a class reference).
    if (auto *C = llvm::dyn_cast_or_null<ClassType>(
            llvm::cast<TypeDecl>(D)->getType());
        C && LangOpts.ObjKeywords)
      return classRefOf(C);
    Diags.error(Id.Loc,
                "'" + Id.Spelling + "' is a " + D->getKindName() +
                    ", not a value",
                {Id.getRange()});
    noteDeclaredHere(D);
    return errorType();
  case Decl::DK_Builtin:
    // eof / eoln without arguments test `input`.
    if (auto K = llvm::cast<BuiltinDecl>(D)->getBuiltinKind();
        K == BuiltinKind::Eof || K == BuiltinKind::Eoln)
      return Ctx.getBooleanType();
    if (llvm::cast<BuiltinDecl>(D)->getBuiltinKind() == BuiltinKind::ParamCount)
      return Ctx.getIntegerType();
    if (llvm::cast<BuiltinDecl>(D)->getBuiltinKind() == BuiltinKind::ExceptAddr)
      return Ctx.getUntypedPointerType();
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


/// move p: the object of the owned variable p of this routine, which
/// gives it up.
Type *Checker::visitMoveExpr(MoveExpr *E){
  NameRefExpr *N = E->getOperand();
  const Identifier &Id = N->getName();
  WithContext *W = nullptr;
  Decl *D = Cur->lookup(Id.Name, &W);
  noteLambdaUse(D, W, Id);
  if (!D) {
    diagUndeclared(Id);
    return errorType();
  }
  auto *V = llvm::dyn_cast<VarDecl>(D);
  if (!V || !(V->isOwned() || V->isIsolated()) || W) {
    Diags.error(Id.Loc,
                "'move' needs an owned or isolated variable, and '" +
                    Id.Spelling + "' is not one",
                {Id.getRange()});
    noteDeclaredHere(D);
    return errorType();
  }
  N->setDecl(V);
  N->setType(V->getType());
  Used.insert(V);
  if (!V->isOwned()) // isolated: its data, the variable left empty
    return V->getType()->isError() ? errorType() : V->getType();
  if (V->getOwner() != CurRoutine) {
    Diags.error(Id.Loc,
                "owned variable '" + Id.Spelling +
                    "' can only be moved by the routine it belongs to",
                {Id.getRange()});
    noteDeclaredHere(V);
    return errorType();
  }
  ownedUse(V, Id.getRange());
  ownedMove(V, E->getBeginLoc());
  return V->getType();
}

VectorType *Checker::booleanVector(unsigned N){
  VectorType *&V = BooleanVectors[N];
  if (!V)
    V = Ctx.create<VectorType>(Ctx.getBooleanType(), N);
  return V;
}


Type *Checker::visitUnaryExpr(UnaryExpr *E){
  Type *T = check(E->getOperand());
  if (T->isError())
    return errorType();
  if (memberTableOf(T)) // an advanced record's operator
    return checkRecordOperator(E);
  if (E->getOp() == UnaryOp::Not) {
    if (T->getBase()->isBoolean())
      return Ctx.getBooleanType();
    // vector[N] of T: lane by lane, of boolean or integer lanes
    if (auto *VT = llvm::dyn_cast<VectorType>(T);
        VT && (VT->getElementType()->getBase()->isBoolean() ||
               VT->getElementType()->isIntegerLike()))
      return T;
    if (!isISO() && T->isIntegerLike()) // Turbo/FPC: bitwise not, same type
      return T->getBase();
  } else if (T->isNumeric()) {
    if (T->isReal())
      return Ctx.getRealType();
    Type *P = promoteInteger(T);
    // -cardinal and -qword can be negative (FPC: int64).
    return E->getOp() == UnaryOp::Minus &&
                   (P == Ctx.getCardinalType() || isQWord(P))
               ? Ctx.getInt64Type()
               : P;
  }
  Diags.error(E->getOpLoc(),
              llvm::Twine("invalid operand to '") + getOpSpelling(E->getOp()) +
                  "' (" + quoted(T) + ")",
              {E->getOperand()->getRange()});
  return errorType();
}


/// obj is TFoo / obj as TFoo (objfpc); the left operand may also be a
/// COM interface (its object).
Type *Checker::checkIsAs(BinaryExpr *E){
  Type *L = check(E->getLHS());
  auto *LI = llvm::dyn_cast<InterfaceType>(L);
  bool COMObject = LI && LI->isCOM();
  const char *Op = getOpSpelling(E->getOp());
  ClassType *C = nullptr;
  Expr *RHS = E->getRHS();
  auto *N = llvm::dyn_cast<NameRefExpr>(RHS);
  TypeDecl *TD =
      N ? llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(N->getName().Name))
        : llvm::isa<FieldExpr>(RHS) ? typeNamedBy(RHS) // TOuter.TInner
                                    : nullptr;
  if (TD) {
    Type *T = TD->getType() ? TD->getType() : errorType();
    if (N)
      N->setDecl(TD);
    RHS->setType(T);
    if (T->isError())
      return errorType();
    C = llvm::dyn_cast<ClassType>(T);
    // obj is IFoo / obj as IFoo: does obj's class implement the interface?
    if (auto *I = llvm::dyn_cast<InterfaceType>(T)) {
      if (L->isError())
        return errorType();
      if (!L->isClass() && !COMObject) {
        Diags.error(E->getOpLoc(),
                    llvm::Twine("invalid operands to '") + Op + "' (" +
                        quoted(L) + " and " + quoted(I) +
                        "): the left operand must be an object or a COM "
                        "interface",
                    {E->getLHS()->getRange(), RHS->getRange()});
        return errorType();
      }
      return E->getOp() == BinaryOp::Is ? Ctx.getBooleanType() : I;
    }
  } else if (check(RHS)->isError()) {
    return errorType();
  }
  if (!C) {
    Diags.error(RHS->getBeginLoc(),
                llvm::Twine("the right operand of '") + Op +
                    "' must be the name of a class or interface type",
                {RHS->getRange()});
    return errorType();
  }
  if (L->isError())
    return errorType();
  if (COMObject) // its object's class is not known
    return E->getOp() == BinaryOp::Is ? Ctx.getBooleanType() : C;
  auto *LC = llvm::dyn_cast<ClassType>(L);
  if (!LC) {
    Diags.error(E->getOpLoc(),
                llvm::Twine("invalid operands to '") + Op + "' (" + quoted(L) +
                    " and " + quoted(C) + ")",
                {E->getLHS()->getRange(), RHS->getRange()});
    return errorType();
  }
  if (!C->inheritsFrom(LC) && !LC->inheritsFrom(C)) {
    Diags.error(E->getOpLoc(),
                llvm::Twine("invalid operands to '") + Op + "': " + quoted(LC) +
                    " and " + quoted(C) + " are unrelated classes",
                {E->getLHS()->getRange(), RHS->getRange()});
    return errorType();
  }
  return E->getOp() == BinaryOp::Is ? Ctx.getBooleanType() : C;
}


bool Checker::isMethodPointer(Type *T){
  auto *RT = llvm::dyn_cast<RoutineType>(T);
  return RT && RT->isOfObject();
}


/// An operand of = or <>: a method pointer variable is compared, not called.
Type *Checker::checkComparand(Expr *E){
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
    auto *V = llvm::dyn_cast_or_null<VarDecl>(Cur->lookup(N->getName().Name));
    if (V && V->getType() && isMethodPointer(V->getType())) {
      N->setDecl(V);
      N->setType(V->getType());
      Used.insert(V);
      return V->getType();
    }
  }
  return check(E);
}


/// The text of a string or char constant (S), if E is one.
bool Checker::constantText(Expr *E, std::string &S){
  auto C = fold(E);
  if (!C)
    return false;
  if (C->Value.K == ConstValue::String) {
    S = C->Value.Str.str();
    return true;
  }
  if (C->Value.K == ConstValue::Ordinal && C->Ty->getBase()->isChar()) {
    S = std::string(1, static_cast<char>(C->Value.Int));
    return true;
  }
  return false;
}


/// A + B of string or char constants: the constant it is (a hidden
/// constant declaration E refers to); its type, or null if it is not one.
Type *Checker::foldConcatenation(BinaryExpr *E){
  std::string A, B;
  if (!constantText(E->getLHS(), A) || !constantText(E->getRHS(), B))
    return nullptr;
  std::string S = A + B;
  Type *T;
  ConstValue V;
  if (S.size() == 1) {
    T = Ctx.getCharType();
    V = ConstValue::ordinal(static_cast<uint8_t>(S[0]));
  } else {
    T = Ctx.getStringLiteralType(S.size());
    V = ConstValue::string(Ctx.getStrings().intern(S));
  }
  Identifier Id = predeclaredName("(constant)");
  Id.Loc = E->getBeginLoc();
  auto *C = Ctx.create<ConstDecl>(SourceLocation(), Id, T, V);
  auto *Ref = Ctx.create<NameRefExpr>(Id);
  Ref->setDecl(C);
  Ref->setType(T);
  E->setConstRef(Ref);
  return T;
}


Type *Checker::visitBinaryExpr(BinaryExpr *E){
  if (E->getOp() == BinaryOp::Is || E->getOp() == BinaryOp::As)
    return checkIsAs(E);
  bool Equality = E->getOp() == BinaryOp::Eq || E->getOp() == BinaryOp::Ne;
  Type *L = Equality ? checkComparand(E->getLHS()) : check(E->getLHS());
  // a and b: b is evaluated where a holds (a or b: where it does not)
  bool Logic = (E->getOp() == BinaryOp::And || E->getOp() == BinaryOp::Or) &&
               anyNullSafety() && L->isBoolean();
  OwnedState BeforeRHS;
  if (Logic) {
    BeforeRHS = ownedGet();
    nullNarrowBy(E->getLHS(), E->getOp() == BinaryOp::And);
  }
  Type *R = Equality ? checkComparand(E->getRHS()) : check(E->getRHS());
  if (Logic)
    ownedJoin(BeforeRHS);
  if (L->isError() || R->isError())
    return errorType();
  if (Equality && (isMethodPointer(L) || isMethodPointer(R)) &&
      (L->isNil() || R->isNil() || isAssignable(L, R)))
    return Ctx.getBooleanType();
  if (memberTableOf(L) || memberTableOf(R)) // an advanced record's operator
    return checkRecordOperator(E);

  BinaryOp Op = E->getOp();
  // Turbo/FPC strings: + concatenates, comparisons work for any lengths.
  if (!isISO() && L->isStringish() && R->isStringish()) {
    // Of string constants: a constant, of any length (not a shortstring
    // of at most 255 characters).
    if (Op == BinaryOp::Add && !(L->isNumeric() || R->isNumeric()))
      if (Type *T = foldConcatenation(E))
        return T;
    if (Op == BinaryOp::Add && !(L->isNumeric() || R->isNumeric()))
      return L->isAnsiString() || R->isAnsiString() ? Ctx.getAnsiStringType()
                                                    : Ctx.getShortStringType();
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
      // The operand with the larger representation (its values hold the
      // other's); the left one of equals, the other one of [].
      if (llvm::cast<SetType>(L)->isEmptySet())
        return R;
      if (llvm::cast<SetType>(R)->isEmptySet())
        return L;
      return llvm::cast<SetType>(R)->getStorageBits() >
                     llvm::cast<SetType>(L)->getStorageBits()
                 ? R
                 : L;
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
  if ((Op == BinaryOp::Eq || Op == BinaryOp::Ne) &&
      (L->isClass() || R->isClass() || L->isClassRef() || R->isClassRef() ||
       L->isInterface() || R->isInterface() || L->isDynArray() || R->isDynArray()) &&
      areCompatible(L, R))
    return Ctx.getBooleanType(); // objects and classes (objfpc)

  if (!isISO() && (L->isPointer() || R->isPointer())) {
    // Turbo/FPC pointer arithmetic: p + n, n + p and p - n move p by n of
    // what it points to (bytes, for an untyped pointer); p - q counts
    // them; pointers are ordered by address. Under {$pointermath off},
    // arithmetic is rejected while relational ordering stays allowed.
    if ((Op == BinaryOp::Add || Op == BinaryOp::Sub) && L->isPointer() &&
        R->isIntegerLike()) {
      if (isPointerMathOff(E->getOpLoc())) {
        Diags.error(E->getOpLoc(),
                    "pointer arithmetic is disabled under {$pointermath off}",
                    {E->getLHS()->getRange(), E->getRHS()->getRange()});
        return errorType();
      }
      if (NilPointersHere)
        requireNotNil(E->getLHS(), "but pointer arithmetic is performed on it");
      return L;
    }
    if (Op == BinaryOp::Add && L->isIntegerLike() && R->isPointer()) {
      if (isPointerMathOff(E->getOpLoc())) {
        Diags.error(E->getOpLoc(),
                    "pointer arithmetic is disabled under {$pointermath off}",
                    {E->getLHS()->getRange(), E->getRHS()->getRange()});
        return errorType();
      }
      if (NilPointersHere)
        requireNotNil(E->getRHS(), "but pointer arithmetic is performed on it");
      return R;
    }
    if (L->isPointer() && R->isPointer() && areCompatible(L, R)) {
      if (Op == BinaryOp::Sub) {
        if (isPointerMathOff(E->getOpLoc())) {
          Diags.error(E->getOpLoc(),
                      "pointer arithmetic is disabled under {$pointermath off}",
                      {E->getLHS()->getRange(), E->getRHS()->getRange()});
          return errorType();
        }
        return Ctx.getInt64Type();
      }
      if (Op == BinaryOp::Lt || Op == BinaryOp::Le || Op == BinaryOp::Gt ||
          Op == BinaryOp::Ge)
        return Ctx.getBooleanType();
    }
  }

  bool BothIntegers = L->isIntegerLike() && R->isIntegerLike();
  auto IntegerOp = [&]() {
    Type *T = integerOperandType(E->getLHS(), L, E->getRHS(), R);
    E->setOperandType(T);
    return T;
  };
  // Can S (a non-vector numeric scalar) stand beside the vector VT
  // under an elementwise operator: the scalar broadcast to every lane
  // (llvm-23.md LB-6)? A real element takes any numeric (the ordinary
  // integer-to-real promotion); an integer element needs the scalar
  // compatible with it.
  auto VecScalar = [&](Type *VTy, Type *S) {
    auto *VT = llvm::dyn_cast<VectorType>(VTy);
    if (!VT || S->isVector())
      return false;
    Type *E = VT->getElementType();
    return E->isReal() ? S->isNumeric() : areCompatible(E, S);
  };
  // vector[N] of T: comparisons lane by lane, giving a vector of booleans
  // (a mask); and, or, xor of boolean or integer lanes.
  if (L->isVector() || R->isVector()) {
    bool SameShape =
        L->isVector() && R->isVector() &&
        llvm::cast<VectorType>(L)->getCount() ==
            llvm::cast<VectorType>(R)->getCount() &&
        areCompatible(llvm::cast<VectorType>(L)->getElementType(),
                      llvm::cast<VectorType>(R)->getElementType());
    bool Fits = SameShape || VecScalar(L, R) || VecScalar(R, L);
    auto *VT = llvm::cast<VectorType>(L->isVector() ? L : R);
    Type *Elem = VT->getElementType();
    switch (Op) {
    case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt:
    case BinaryOp::Le: case BinaryOp::Gt: case BinaryOp::Ge:
      if (Fits)
        return booleanVector(VT->getCount());
      break;
    case BinaryOp::And: case BinaryOp::Or: case BinaryOp::Xor:
      if (Fits && (Elem->getBase()->isBoolean() || Elem->isIntegerLike()))
        return VT;
      break;
    default:
      break;
    }
  }
  switch (Op) {
  case BinaryOp::Add: case BinaryOp::Sub: case BinaryOp::Mul:
    // vector[N] of T: the elementwise operators of two same-shaped
    // vectors (llvm-23.md LB-6). No per-lane overflow checks: LLVM's
    // vector adds carry no checked form (documented in the item).
    if (L->isVector() && R->isVector() &&
        llvm::cast<VectorType>(L)->getCount() ==
            llvm::cast<VectorType>(R)->getCount() &&
        areCompatible(llvm::cast<VectorType>(L)->getElementType(),
                      llvm::cast<VectorType>(R)->getElementType()))
      return L;
    if (VecScalar(L, R) || VecScalar(R, L))
      return L->isVector() ? L : R;
    if (BothIntegers)
      return IntegerOp();
    if (L->isNumeric() && R->isNumeric())
      return Ctx.getRealType();
    break;
  case BinaryOp::RealDiv:
    if (VecScalar(L, R) || VecScalar(R, L)) {
      // the lanes divide: only a real element's vector (/ is Pascal's
      // real division)
      auto *VT = llvm::cast<VectorType>(L->isVector() ? L : R);
      if (VT->getElementType()->isReal())
        return L->isVector() ? L : R;
    }
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
    if (L->isOrdinal() && R->isOrdinal() && areCompatible(L, R)) {
      if (L->getBase() != R->getBase() && L->isCharLike()) // char, widechar
        E->setOperandType(Ctx.getWideCharType());
      return Ctx.getBooleanType();
    }
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
  case BinaryOp::Is: case BinaryOp::As:
    break; // checkIsAs
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


Type *Checker::visitCallExpr(CallExpr *E){
  // A lambda that copies what it uses: its factory, declared now.
  if (auto *F = llvm::dyn_cast_or_null<RoutineDecl>(E->getDecl());
      F && F->isCopyFactory()) {
    if (!F->isLambdaDone() &&
        !completeCopyFactory(F, E, WantedCall == E ? WantedType : nullptr))
      return errorType();
    if (F->isLambdaBad())
      return errorType();
  }
  const Identifier &Callee = E->getCallee();
  WithContext *W = nullptr;
  Decl *D = E->getTypeArgs().empty() ? Cur->lookup(Callee.Name, &W)
                                     : specialize(Callee, E->getTypeArgs(), &W);
  noteLambdaUse(D, W, Callee);
  bool Generic = false; // F(args) for a generic F: its types inferred
  if (!D && E->getTypeArgs().empty())
    D = inferSpecialization(Callee, E->getArgs(),
                            WantedCall == E ? WantedType : nullptr, Generic, &W);
  if (!D) {
    if (E->getTypeArgs().empty() && !Generic)
      diagUndeclared(Callee);
    for (Expr *Arg : E->getArgs())
      check(argValue(Arg));
    return errorType();
  }
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) { // objfpc: overloads
    D = resolveOverload(R, E->getArgs(), Callee, E->getRange());
    if (!D)
      return errorType();
  }
  E->setDecl(D);
  warnHints(D, Callee);
  if (hasMembers(W)) { // a method of an object or advanced record
    E->setWith(W);
    checkScopeAccess(D, W, Callee);
  }

  if (RoutineType *RT = proceduralMember(D)) {
    // FCalc(x) in a method or a with statement: through the member
    auto *Ref = Ctx.create<NameRefExpr>(Callee);
    if (check(Ref)->isError())
      return errorType();
    E->setCalleeValue(Ref);
    checkCallArgs(RT->getParams(), nullptr, Callee,
                  orderedArgs(E, RT->getParams(), Callee), E->getRange());
    if (RT->isFunction())
      return RT->getResult();
    Diags.error(Callee.Loc,
                "procedure '" + Callee.Spelling + "' does not return a value",
                {Callee.getRange()});
    return errorType();
  }

  if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) {
    if (R->isAsync()) { // its task
      checkCallArgs(R, Callee, orderedArgs(E, R->getParams(), Callee),
                    E->getRange());
      return R->getAsyncType();
    }
    if (R->isIterator()) { // its iterator
      checkCallArgs(R, Callee, orderedArgs(E, R->getParams(), Callee),
                    E->getRange());
      return R->getIteratorType();
    }
    if (R->isFunction()) {
      checkCallArgs(R, Callee, orderedArgs(E, R->getParams(), Callee),
                    E->getRange());
      return R->getResultType();
    }
    if (ClassType *C = constructedThroughSelf(R, W)) {
      checkCallArgs(R, Callee, orderedArgs(E, R->getParams(), Callee),
                    E->getRange());
      return C;
    }
    for (Expr *Arg : E->getArgs())
      check(argValue(Arg));
    Diags.error(Callee.Loc,
                "procedure '" + Callee.Spelling + "' does not return a value",
                {Callee.getRange()});
    return errorType();
  }

  if (auto *V = llvm::dyn_cast<VarDecl>(D))
    if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(V->getType());
        RT && RT->isFunction()) {
      Used.insert(V); // (called: used)
      checkCallArgs(RT->getParams(), V, Callee,
                    orderedArgs(E, RT->getParams(), Callee), E->getRange());
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


/// `a ${x} b`: an ansistring; each value is one `write` can write, with
/// the field width and precision it takes there.
Type *Checker::visitTemplateExpr(TemplateExpr *E){
  llvm::ArrayRef<Expr *> Parts = E->getParts();
  for (size_t I = 0; I < Parts.size(); ++I) {
    if (I % 2 == 0)
      check(Parts[I]);
    else
      checkWritten(Parts[I], /*Template=*/true);
  }
  return Ctx.getAnsiStringType();
}


/// Name := value where the parameters have no names to give (a standard
/// routine, a typecast).
Type *Checker::visitNamedArgExpr(NamedArgExpr *E){
  check(E->getValue());
  Diags.error(E->getName().Loc,
              "a named argument is only allowed in a call to a declared "
              "routine",
              {E->getName().getRange()});
  return errorType();
}


Type *Checker::visitFormattedExpr(FormattedExpr *E){
  check(E->getValue());
  Diags.error(E->getWidth()->getBeginLoc(),
              "field widths are only allowed in 'write' and 'writeln'",
              {E->getWidth()->getRange()});
  return errorType();
}


Type *Checker::visitIndexExpr(IndexExpr *E){
  Expr *SavedBase = IndexBase;
  IndexBase = E->getBase();
  Type *BT = check(E->getBase());
  IndexBase = SavedBase;
  if (BT->isError())
    return errorType();
  if (LangOpts.ObjKeywords) {
    // An array property: obj.P[i], P[i], obj[i] (the default property).
    auto It = PendingIndexes.find(E->getBase());
    if (It != PendingIndexes.end()) {
      PendingIndex Pend = std::move(It->second);
      PendingIndexes.erase(It);
      return checkPropertyIndex(E, std::move(Pend));
    }
    // An advanced record's default property: r[i].
    if (ClassType *MT = BT->isRecord() ? memberTableOf(BT) : nullptr)
      if (PropertyDecl *P = defaultProperty(MT)) {
        Identifier Id = P->getName();
        Id.Loc = E->getBase()->getBeginLoc();
        checkMemberAccess(P, MT, Id);
        return checkPropertyIndex(E, {P, E->getBase(), {}});
      }
    if (auto *C = llvm::dyn_cast<ClassType>(BT)) {
      PropertyDecl *P = defaultProperty(C);
      if (!P) {
        Diags.error(E->getIndex()->getBeginLoc(),
                    "class " + quoted(C) + " has no default property",
                    {E->getBase()->getRange()});
        return errorType();
      }
      const ClassType *Owner = nullptr;
      C->findMember(P->getName().Name, &Owner);
      Identifier Id = P->getName();
      Id.Loc = E->getBase()->getBeginLoc();
      checkMemberAccess(P, Owner, Id);
      return checkPropertyIndex(E, {P, E->getBase(), {}});
    }
  }
  Type *IT = check(E->getIndex());
  if (IT->isError())
    return errorType();
  if (auto *DA = llvm::dyn_cast<DynArrayType>(BT)) { // a[0..high(a)]
    if (!IT->isIntegerLike()) {
      Diags.error(E->getIndex()->getBeginLoc(), "array index must be an integer",
                  {E->getIndex()->getRange()});
      return errorType();
    }
    return DA->getElementType();
  }
  if (auto *OA = llvm::dyn_cast<OpenArrayType>(BT)) { // a[0..high(a)]
    if (!IT->isIntegerLike()) {
      Diags.error(E->getIndex()->getBeginLoc(), "array index must be an integer",
                  {E->getIndex()->getRange()});
      return errorType();
    }
    return OA->getElementType();
  }
  if (auto *AC = llvm::dyn_cast<ArrayOfConstType>(BT)) { // a[0..high(a)]
    if (!IT->isIntegerLike()) {
      Diags.error(E->getIndex()->getBeginLoc(), "array index must be an integer",
                  {E->getIndex()->getRange()});
      return errorType();
    }
    return AC->getElementType();
  }
  if (BT->isAnsiString()) { // s[1..length(s)]
    if (!IT->isIntegerLike()) {
      Diags.error(E->getIndex()->getBeginLoc(), "string index must be an integer",
                  {E->getIndex()->getRange()});
      return errorType();
    }
    return Ctx.getCharType();
  }
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
  if (auto *PT = llvm::dyn_cast<PointerType>(BT);
      PT && !isISO() && !PT->isUntyped() && PT->getPointee()) {
    // Turbo/FPC: p[i] is (p + i)^.
    if (isPointerMathOff(E->getIndex()->getBeginLoc())) {
      Diags.error(E->getIndex()->getBeginLoc(),
                  "pointer indexing is disabled under {$pointermath off}",
                  {E->getBase()->getRange()});
      return errorType();
    }
    if (!IT->isIntegerLike()) {
      Diags.error(E->getIndex()->getBeginLoc(),
                  "pointer index must be an integer",
                  {E->getIndex()->getRange()});
      return errorType();
    }
    if (NilPointersHere)
      requireNotNil(E->getBase(), "but it is indexed");
    return PT->getPointee();
  }
  if (auto *VT = llvm::dyn_cast<VectorType>(BT)) {
    if (!IT->isIntegerLike()) {
      Diags.error(E->getIndex()->getBeginLoc(), "vector index must be an integer",
                  {E->getIndex()->getRange()});
      return errorType();
    }
    if (auto C = fold(E->getIndex()))
      if (C->Value.K == ConstValue::Ordinal &&
          (C->Value.Int < 1 ||
           static_cast<uint64_t>(C->Value.Int) > VT->getCount()))
        Diags.error(E->getIndex()->getBeginLoc(),
                    "vector index " + std::to_string(C->Value.Int) +
                        " is out of bounds 1.." +
                        std::to_string(VT->getCount()),
                    {E->getIndex()->getRange()});
    return VT->getElementType();
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


/// One more index for the array property of Pend: while indexes are
/// missing, E itself is pending (if more follow); with all of them, they
/// are checked like arguments and E is the property access.
Type *Checker::checkPropertyIndex(IndexExpr *E, PendingIndex Pend){
  PropertyDecl *P = Pend.Prop;
  Pend.Args.push_back(E->getIndex());
  size_t N = P->getParams().size();
  if (Pend.Args.size() < N) {
    if (E == IndexBase) {
      PendingIndexes[E] = std::move(Pend);
      return P->getType();
    }
    E->setProperty(P, Pend.Root, Ctx.copyArray<Expr *>(Pend.Args));
    Identifier Id = propertyName(E);
    Diags.error(Id.Loc,
                "array property '" + Id.Spelling + "' needs " +
                    std::to_string(N) + " indexes, got " +
                    std::to_string(Pend.Args.size()),
                {E->getRange()});
    noteDeclaredHere(P);
    for (Expr *A : Pend.Args)
      check(A);
    return errorType();
  }
  E->setProperty(P, Pend.Root, Ctx.copyArray<Expr *>(Pend.Args));
  Identifier Id = propertyName(E);
  checkCallArgs(P->getParams(), P, Id, E->getPropertyArgs(), E->getRange());
  if (E == CurAssignTarget)
    return P->getType(); // checked by the assignment
  return checkPropertyRead(P, E, Id);
}


/// Checks a value assigned to a target of type TargetTy. A procedural
/// variable named as the value of a procedural target is copied, not
/// called.
/// [a, b, c] given to a dynamic array of type T.
Type *Checker::checkDynArrayCtor(SetCtorExpr *S, DynArrayType *T){
  Type *Elem = T->getElementType();
  for (const SetElement &El : S->getElements()) {
    if (El.Hi) {
      Diags.error(El.Hi->getBeginLoc(),
                  "a range cannot be an element of a dynamic array",
                  {El.Hi->getRange()});
      continue;
    }
    Type *AT = checkValueFor(Elem, El.Lo);
    if (AT->isError())
      continue;
    if (!isAssignable(Elem, AT) && !isPCharLiteral(Elem, El.Lo) &&
        !convertImplicitly(El.Lo, Elem))
      Diags.error(El.Lo->getBeginLoc(),
                  "a value of type " + quoted(AT) +
                      " cannot be an element of a dynamic array of " + quoted(Elem),
                  {El.Lo->getRange()});
  }
  S->setType(T);
  return T;
}


Type *Checker::checkValueFor(Type *TargetTy, Expr *Value){
  // A call: the type wanted of its result can give a generic routine a
  // type argument (inferSpecialization).
  if (llvm::isa<CallExpr>(Value)) {
    WantedCall = Value;
    WantedType = TargetTy;
  }
  // if c then a else b: both branches are values for the type wanted.
  if (auto *CE = llvm::dyn_cast<CondExpr>(Value); CE && !CE->getType()) {
    Type *T = checkCond(CE, TargetTy);
    CE->setType(T);
    if (isManaged(T))
      markUsesManaged();
    return T;
  }
  // A tuple written out takes the element types of the tuple wanted.
  if (auto *TE = llvm::dyn_cast<TupleExpr>(Value)) {
    auto *Want = llvm::dyn_cast<RecordType>(TargetTy);
    if (Want && Want->isTuple() &&
        Want->getAllFields().size() == TE->getElements().size()) {
      Type *T = checkTuple(TE, Want);
      TE->setType(T);
      if (isManaged(T))
        markUsesManaged();
      return T;
    }
  }
  if (auto *DA = llvm::dyn_cast<DynArrayType>(TargetTy))
    if (auto *S = llvm::dyn_cast<SetCtorExpr>(Value))
      return checkDynArrayCtor(S, DA);
  if (auto *TP = llvm::dyn_cast<PointerType>(TargetTy);
      TP && TP->isUntyped() && llvm::isa<AddrOfExpr>(Value)) {
    AddrOfWantsCode = true;
    Type *T = check(Value);
    AddrOfWantsCode = false;
    return T;
  }
  if (auto *RT = llvm::dyn_cast<RoutineType>(TargetTy);
      RT && llvm::isa<AddrOfExpr>(Value)) { // @p picks an overload by RT
    RoutineType *Saved = AddrOfTarget;
    AddrOfTarget = RT;
    Type *T = check(Value);
    AddrOfTarget = Saved;
    return T;
  }
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
RoutineType *Checker::checkIndirectCallee(IndirectCallExpr *E){
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
  checkCallArgs(RT->getParams(), nullptr, Name,
                orderedArgs(E, RT->getParams(), Name), E->getRange());
  return RT;
}


Type *Checker::visitIndirectCallExpr(IndirectCallExpr *E){
  if (isMethodCallee(E->getCallee())) {
    Type *T = checkMethodCall(E);
    if (T)
      return T;
    Diags.error(E->getBeginLoc(), "procedure does not return a value",
                {E->getRange()});
    return errorType();
  }
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


/// if c then a else b, a value. Want: the type it is a value for, which
/// both branches then take; where they cannot, and without Want, its
/// type is the common type of the branches. It becomes an if statement
/// that assigns the branch chosen to a hidden variable.
Type *Checker::checkCond(CondExpr *E, Type *Want){
  checkCondition(E->getCond(), "if");
  OwnedState Before = ownedGet();
  nullNarrowBy(E->getCond(), true);
  Type *A = Want ? checkValueFor(Want, E->getThen()) : check(E->getThen());
  OwnedState AfterThen = ownedGet();
  ownedSet(Before);
  nullNarrowBy(E->getCond(), false);
  Type *B = Want ? checkValueFor(Want, E->getElse()) : check(E->getElse());
  ownedJoin(AfterThen);
  if (A->isError() || B->isError())
    return errorType();
  Type *R = nullptr;
  if (Want && !Want->isError() && isAssignable(Want, A) &&
      isAssignable(Want, B))
    R = Want;
  if (!R) {
    Type *Str = E->hasLongStrings()
                    ? static_cast<Type *>(Ctx.getAnsiStringType())
                    : Ctx.getShortStringType(255);
    Type *TA = A->isStringLiteral() ? Str : A;
    Type *TB = B->isStringLiteral() ? Str : B;
    if (isInferable(TA) && isInferable(TB))
      R = commonType(TA, TB, Str);
    else if (isInferable(TA) && isAssignable(TA, TB))
      R = TA; // (nil, [] in the other branch)
    else if (isInferable(TB) && isAssignable(TB, TA))
      R = TB;
    if (!R) {
      Diags.error(E->getBeginLoc(),
                  "the branches of this conditional expression have no "
                  "type in common: " + quoted(A) + " and " + quoted(B),
                  {E->getThen()->getRange(), E->getElse()->getRange()});
      return errorType();
    }
  }
  VarDecl *Tmp = makeHiddenVar(R, E->getBeginLoc());
  if (!Tmp) {
    Diags.error(E->getBeginLoc(),
                "a conditional expression can only be written in the "
                "statements of a routine or program",
                {E->getRange()});
    return errorType();
  }
  auto Ref = [&](SourceLocation Loc) {
    Identifier Id = Tmp->getName();
    Id.Loc = Loc;
    auto *N = Ctx.create<NameRefExpr>(Id);
    N->setDecl(Tmp);
    N->setType(R);
    return N;
  };
  auto Assign = [&](Expr *V, Type *VT) {
    auto *S = Ctx.create<AssignStmt>(V->getRange(), Ref(V->getBeginLoc()),
                                     V->getBeginLoc(), V);
    finishAssign(S, R, VT);
    return S;
  };
  AssignStmt *Then = Assign(E->getThen(), A);
  AssignStmt *Else = Assign(E->getElse(), B);
  E->setLowered(Ctx.create<IfStmt>(E->getRange(), E->getCond(), Then, Else),
                Ref(E->getBeginLoc()));
  return R;
}


/// (a, b): a tuple value. Want: the tuple type it is a value for (with
/// as many elements), whose element types its elements then take; where
/// they cannot, and without Want, each element has the type a variable
/// declared with its value would have.
Type *Checker::checkTuple(TupleExpr *E, RecordType *Want){
  llvm::SmallVector<Type *, 4> Types;
  bool OK = true;
  for (size_t I = 0; I < E->getElements().size(); ++I) {
    Expr *X = E->getElements()[I];
    Type *T = Want ? checkValueFor(Want->getAllFields()[I]->getType(), X)
                   : check(X);
    OK &= !T->isError();
    Types.push_back(T);
  }
  if (!OK)
    return errorType();
  Type *Result = Want;
  for (size_t I = 0; Result && I < Types.size(); ++I) {
    Type *FT = Want->getAllFields()[I]->getType();
    Expr *X = E->getElements()[I];
    if (isAssignable(FT, Types[I]) || convertImplicitly(X, FT))
      checkWideToChar(FT, X);
    else
      Result = nullptr;
  }
  if (!Result) {
    for (size_t I = 0; I < Types.size(); ++I) {
      Type *T = Types[I];
      Expr *X = E->getElements()[I];
      auto *ST = llvm::dyn_cast<SetType>(T);
      if (T->isStringLiteral() || (isCharLiteral(X) && !KeepCharLiterals)) {
        T = E->hasLongStrings() ? Ctx.getAnsiStringType()
                                : Ctx.getShortStringType(255);
      } else if (T->isNil() || T->isUntyped() || T->isOpenArray() ||
                 T->isArrayOfConst() || containsFile(T) ||
                 (ST && ST->isEmptySet())) {
        Diags.error(X->getBeginLoc(),
                    "cannot tell the type of element " +
                        std::to_string(I + 1) + " of the tuple from " +
                        (T->isNil() ? std::string("nil")
                         : ST       ? std::string("an empty set")
                                    : "a value of type " + quoted(T)) +
                        "; assign the tuple to a variable of a tuple type",
                    {X->getRange()});
        return errorType();
      }
      Types[I] = T;
    }
    Result = getTupleType(Types, {});
  }
  if (VarDecl *V = E->getTemp()) {
    V->setType(Result);
  } else if (VarDecl *New = makeHiddenVar(Result, E->getBeginLoc())) {
    E->setTemp(New);
  } else {
    Diags.error(E->getBeginLoc(),
                "a tuple can only be written in the statements of a "
                "routine or program",
                {E->getRange()});
    return errorType();
  }
  return Result;
}


Type *Checker::visitAggregateExpr(AggregateExpr *E){
  Diags.error(E->getBeginLoc(),
              "a parenthesized list is only allowed as the value of a typed "
              "constant",
              {E->getRange()});
  return errorType();
}


/// Is the code being checked in an async routine (or a lambda in one)?
bool Checker::inAsyncRoutine() const{
  RoutineDecl *R = CurRoutine;
  while (R && R->isLambda() && !R->isAsync())
    R = R->getParent();
  return R && R->isAsync();
}


/// Is E (checked) the call of an async routine?
bool Checker::isAsyncCall(Expr *E){
  Decl *D = nullptr;
  if (auto *C = llvm::dyn_cast<CallExpr>(E))
    D = C->getDecl();
  else if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
    D = N->getDecl();
  else if (auto *F = llvm::dyn_cast<FieldExpr>(E))
    D = F->getMember();
  else if (auto *I = llvm::dyn_cast<IndirectCallExpr>(E))
    if (auto *F = llvm::dyn_cast<FieldExpr>(I->getCallee()))
      D = F->getMember();
  auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D);
  return R && R->isAsync();
}


void Checker::warnUnusedIterator(const Identifier &Callee, SourceRange Range){
  Diags.warning(Callee.Loc,
                "the iterator of '" + Callee.Spelling +
                    "' is never used: its body runs only when its values "
                    "are asked for (for ... in " + Callee.Spelling + " do)",
                {Range});
}


void Checker::warnDanglingTask(const Identifier &Callee, SourceRange Range){
  Diags.warning(Callee.Loc,
                "dangling task: '" + Callee.Spelling +
                    "' is never awaited; write 'await " + Callee.Spelling +
                    "' to wait for it, keep it in a variable to await "
                    "later, or 'detached " + Callee.Spelling +
                    "' to let it run on its own",
                {Range});
}


/// await e: e a task; its result (NeedValue: one is needed).
Type *Checker::checkAwait(AwaitExpr *E, bool NeedValue){
  Type *T = check(E->getOperand());
  if (T->isError())
    return errorType();
  auto *TT = llvm::dyn_cast<TaskType>(T);
  if (!TT) {
    Diags.error(E->getBeginLoc(),
                "'await' waits for a task (the call of an async routine), "
                "not a value of type " + quoted(T),
                {E->getRange()});
    return errorType();
  }
  if (!inAsyncRoutine())
    Diags.error(E->getBeginLoc(),
                "'await' is allowed only in an async routine; elsewhere, "
                "Wait(t) waits for a task",
                {E->getRange()});
  if (!TT->getResult()) {
    if (NeedValue) {
      Diags.error(E->getBeginLoc(),
                  "this task has no value (its routine is 'async' without "
                  "a type)",
                  {E->getRange()});
      return errorType();
    }
    return T;
  }
  return TT->getResult();
}


/// The hidden async procedure of spawn begin ... end: declared and
/// checked where it is spawned (it uses the variables declared so far).
void Checker::completeSpawnBlock(RoutineDecl *R){
  R->setLambdaDone();
  resolveSignature(R);
  unsigned SavedDepth = ScopeDepth;
  ScopeDepth = 0; // (its task has scopes of its own)
  declareAndCheckLambda(R);
  ScopeDepth = SavedDepth;
}


Type *Checker::visitSpawnExpr(SpawnExpr *E){
  Expr *C = E->getCall();
  if (auto *CE = llvm::dyn_cast<CallExpr>(C))
    if (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(CE->getDecl());
        R && R->isSpawnBlock() && !R->isLambdaDone())
      completeSpawnBlock(R);
  Type *T = check(C);
  if (T->isError())
    return errorType();
  const char *Word = E->isPoolRun() ? "Pool.Run" : "spawn";
  if (!T->isTask() || !isAsyncCall(C)) {
    Diags.error(C->getBeginLoc(),
                llvm::Twine("'") + Word +
                    "' runs the call of an async routine, not a value of type " +
                    quoted(T),
                {C->getRange()});
    return errorType();
  }
  if (!E->isPoolRun() && ScopeDepth == 0)
    Diags.error(E->getBeginLoc(),
                "'spawn' is allowed only inside a 'scope ... end' block, "
                "which waits for the tasks it starts; Pool.Run starts one "
                "on its own",
                {E->getRange()});
  return T;
}


/// Wait(t) (also as a statement: NeedValue false), WhenAll(tasks),
/// WhenAny(tasks).
Type *Checker::checkWait(llvm::ArrayRef<Expr *> Args, const Identifier &Callee,
                SourceRange Range, bool NeedValue){
  if (Args.size() != 1) {
    Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 1 argument",
                {Range});
    for (Expr *A : Args)
      check(A);
    return errorType();
  }
  Type *T = check(Args[0]);
  if (T->isError())
    return errorType();
  auto *TT = llvm::dyn_cast<TaskType>(T);
  if (!TT) {
    Diags.error(Args[0]->getBeginLoc(),
                "'Wait' waits for a task (the call of an async routine), "
                "not a value of type " + quoted(T),
                {Args[0]->getRange()});
    return errorType();
  }
  if (!TT->getResult()) {
    if (NeedValue) {
      Diags.error(Args[0]->getBeginLoc(),
                  "this task has no value (its routine is 'async' without "
                  "a type)",
                  {Args[0]->getRange()});
      return errorType();
    }
    return T;
  }
  return TT->getResult();
}


/// The tasks of WhenAll / WhenAny: an array of tasks (static, dynamic or
/// open); their task type, or null after a diagnostic.
TaskType *Checker::checkTaskArray(Expr *A, const Identifier &Callee){
  Type *T = check(A);
  if (T->isError())
    return nullptr;
  Type *Elem = nullptr;
  if (auto *AT = llvm::dyn_cast<ArrayType>(T))
    Elem = AT->getElementType();
  else if (auto *DT = llvm::dyn_cast<DynArrayType>(T))
    Elem = DT->getElementType();
  else if (auto *OT = llvm::dyn_cast<OpenArrayType>(T))
    Elem = OT->getElementType();
  auto *TT = llvm::dyn_cast_or_null<TaskType>(Elem);
  if (!TT) {
    Diags.error(A->getBeginLoc(),
                "'" + Callee.Spelling + "' takes an array of tasks, not a "
                "value of type " + quoted(T),
                {A->getRange()});
    return nullptr;
  }
  return TT;
}


Type *Checker::visitAddrOfExpr(AddrOfExpr *E){
  completeLambda(E, AddrOfTarget);
  if (E->getLambda() && E->getLambda()->isLambdaBad())
    return errorType();
  if (Type *T = checkMethodAddr(E))
    return T;
  // @routine (Turbo/FPC): a procedural value.
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E->getOperand()))
    if (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookup(N->getName().Name))) {
      if (R->isOverloaded()) { // objfpc: the overload the target wants
        RoutineDecl *Pick = nullptr;
        for (RoutineDecl *X = R; AddrOfTarget && X; X = X->getNextOverload())
          if (!Pick && X->isCDecl() == AddrOfTarget->isCDecl() &&
              congruent(AddrOfTarget->getParams(),
                        AddrOfTarget->getResult(), X->getParams(),
                        X->isFunction() ? X->getResultType() : nullptr))
            Pick = X;
        if (!Pick) {
          Diags.error(N->getBeginLoc(),
                      "'@" + N->getName().Spelling +
                          "': '" + N->getName().Spelling +
                          "' is overloaded; assign it to a procedural "
                          "variable of the heading wanted",
                      {N->getRange()});
          return errorType();
        }
        R = Pick;
      }
      N->setDecl(R);
      if (R->isMethod()) {
        Diags.error(N->getBeginLoc(),
                    "'@" + N->getName().Spelling +
                        "': methods cannot be procedural values",
                    {N->getRange()});
        return errorType();
      }
      if (std::string Why = notProceduralValue(R, false); !Why.empty()) {
        Diags.error(N->getBeginLoc(), "'@" + N->getName().Spelling + "': " + Why,
                    {N->getRange()});
        return errorType();
      }
      // A closure wanted: a lambda (made one by completeLambda) or a
      // routine declared at program or unit level (no frame to keep).
      // A method pointer: a lambda made one by completeLambda.
      bool Reference = AddrOfTarget && AddrOfTarget->isReference();
      auto *T = Ctx.create<RoutineType>(
          R->getParams(), R->isFunction() ? R->getResultType() : nullptr,
          R->isCDecl(), R->isMethodLambda(), Reference);
      N->setType(T);
      return T;
    }
  Type *T = checkAllowingUntyped(E->getOperand()); // (@x of an untyped x too)
  if (T->isError())
    return errorType();
  if (isBitField(E->getOperand())) {
    diagBitFieldAddress(E->getOperand());
    return errorType();
  }
  if (!isVariableReference(E->getOperand())) {
    Diags.error(E->getOperand()->getBeginLoc(),
                "'@' requires a variable", {E->getOperand()->getRange()});
    return errorType();
  }
  if (VarDecl *OV = ownedVarOf(E->getOperand())) {
    Diags.error(E->getOperand()->getBeginLoc(),
                "cannot take the address of owned variable '" +
                    OV->getName().Spelling + "'",
                {E->getOperand()->getRange()});
    return errorType();
  }
  bool Typed = (LangOpts.Safe == SafeLevel::Verified &&
                !isUnsafe(E->getBeginLoc())) ||
               (Directives &&
                Directives->isEnabled(CheckDirectives::TypedAddress,
                                      E->getBeginLoc(), false));
  if (!T->isUntyped() && Typed)
    return Ctx.create<PointerType>(T, /*Untyped=*/false, /*TypedAddr=*/true);
  return Ctx.getUntypedPointerType();
}


Type *Checker::visitDerefExpr(DerefExpr *E){
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
    if (NilPointersHere)
      requireNotNil(E->getBase(), "but it is dereferenced with '^'");
    return P->getPointee() ? P->getPointee() : errorType();
  }
  if (auto *F = llvm::dyn_cast<FileType>(T); F && !F->isUntyped())
    return F->getElementType(); // the buffer variable f^
  if (T->isFile()) {
    Diags.error(E->getBase()->getBeginLoc(),
                "an untyped file has no buffer variable",
                {E->getBase()->getRange()});
    return errorType();
  }
  Diags.error(E->getBase()->getBeginLoc(),
              "'^' applied to a value of type " + quoted(T) +
                  ", which is not a pointer",
              {E->getBase()->getRange()});
  return errorType();
}


Type *Checker::visitSetCtorExpr(SetCtorExpr *E){
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

