// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- SemaTypes.cpp - Semantic analysis -------------------------------------===//

#include "Checker.h"

using namespace p3;
using namespace p3::detail;


//===--------------------------------------------------------------------===//
// Integer types (Turbo/FPC rules; ISO has only `integer`)
//===--------------------------------------------------------------------===//

/// Integer operands narrower than 32 bits widen to integer.
Type *Checker::promoteInteger(Type *T){
  auto *I = llvm::cast<IntegerType>(T->getBase());
  return I->getBits() < 32 ? Ctx.getIntegerType() : I;
}


/// Is E a constant that a cardinal can hold?
bool Checker::isCardinalConstant(Expr *E){
  auto C = fold(E);
  return C && C->Value.K == ConstValue::Ordinal && C->Value.Int >= 0 &&
         C->Value.Int <= UINT32_MAX;
}


/// Is E a constant of zero or more (that a qword can hold)?
bool Checker::isNonNegativeConstant(Expr *E){
  auto C = fold(E);
  return C && C->Value.K == ConstValue::Ordinal && C->Value.Int >= 0;
}


/// The type integer operands LE and RE are converted to for an operator:
/// qword if one is qword and the other an int64 constant of zero or more;
/// int64 if either is int64; qword if either is qword (the other, of 32
/// bits or less, is converted to qword, as in FPC); cardinal if both are
/// unsigned (or the other is a constant that fits); int64 when cardinal
/// meets a signed value (so no value is lost, as in FPC); integer
/// otherwise.
Type *Checker::integerOperandType(Expr *LE, Type *L, Expr *RE, Type *R){
  Type *PL = promoteInteger(L), *PR = promoteInteger(R);
  Type *I64 = Ctx.getInt64Type(), *Card = Ctx.getCardinalType();
  if ((isQWord(PL) && PR == I64 && isNonNegativeConstant(RE)) ||
      (isQWord(PR) && PL == I64 && isNonNegativeConstant(LE)))
    return Ctx.getQWordType();
  if (PL == I64 || PR == I64)
    return I64;
  if (isQWord(PL) || isQWord(PR))
    return Ctx.getQWordType();
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


//===--------------------------------------------------------------------===//
// Type relations (ISO 7185 6.4.5 / 6.4.6)
//===--------------------------------------------------------------------===//

/// Compatible types: identical, ordinal types with the same host, or
/// string types of the same length.
bool Checker::areCompatible(Type *A, Type *B){
  if (A == B)
    return true;
  if ((A->isPointer() && B->isNil()) || (A->isNil() && B->isPointer()))
    return true;
  if ((A->isClass() && B->isNil()) || (A->isNil() && B->isClass()))
    return true;
  if ((A->isDynArray() && B->isNil()) || (A->isNil() && B->isDynArray()))
    return true;
  if ((A->isClassRef() && B->isNil()) || (A->isNil() && B->isClassRef()))
    return true;
  if (A->isClassRef() && B->isClassRef()) { // related classes (objfpc)
    ClassType *CA = llvm::cast<ClassRefType>(A)->getClass();
    ClassType *CB = llvm::cast<ClassRefType>(B)->getClass();
    return CA->inheritsFrom(CB) || CB->inheritsFrom(CA);
  }
  if ((A->isInterface() && B->isNil()) || (A->isNil() && B->isInterface()))
    return true;
  if (A->isInterface() && B->isInterface()) { // related interfaces
    auto *IA = llvm::cast<InterfaceType>(A), *IB = llvm::cast<InterfaceType>(B);
    return IA->inheritsFrom(IB) || IB->inheritsFrom(IA);
  }
  if (A->isClass() && B->isClass()) // related classes (objfpc)
    return llvm::cast<ClassType>(A)->inheritsFrom(llvm::cast<ClassType>(B)) ||
           llvm::cast<ClassType>(B)->inheritsFrom(llvm::cast<ClassType>(A));
  if (A->isPointer() && B->isPointer()) {
    auto *PA = llvm::cast<PointerType>(A);
    auto *PB = llvm::cast<PointerType>(B);
    if (PA->isUntyped() || PB->isUntyped())
      return true; // Turbo/FPC untyped `pointer`
    if ((PA->isTypedAddr() || PB->isTypedAddr()) &&
        PA->getPointee() == PB->getPointee())
      return true; // {$T+} @x matches any pointer to the same pointee type
  }
  if (A->isSet() && B->isSet()) {
    auto *SA = llvm::cast<SetType>(A), *SB = llvm::cast<SetType>(B);
    return SA->isEmptySet() || SB->isEmptySet() ||
           haveSameOrdinalHost(SA->getElementType(), SB->getElementType());
  }
  if (A->isVector() && B->isVector()) {
    auto *VA = llvm::cast<VectorType>(A), *VB = llvm::cast<VectorType>(B);
    return VA->getCount() == VB->getCount() &&
           haveSameOrdinalHost(VA->getElementType(),
                               VB->getElementType());
  }
  if (A->isOrdinal() && B->isOrdinal())
    return haveSameOrdinalHost(A, B);
  if (A->isStringType() && B->isStringType())
    return A->getStringLength() == B->getStringLength();
  return false;
}


bool Checker::isPChar(Type *T){
  auto *P = llvm::dyn_cast<PointerType>(T);
  return P && !P->isUntyped() && P->getPointee() && P->getPointee()->isChar();
}


/// Are A and B tuple types with the same element types, in turn (the
/// names may differ)? Such tuples have one layout.
bool Checker::sameTupleShape(Type *A, Type *B){
  auto *RA = llvm::dyn_cast<RecordType>(A);
  auto *RB = llvm::dyn_cast<RecordType>(B);
  if (!RA || !RB || !RA->isTuple() || !RB->isTuple() ||
      RA->getAllFields().size() != RB->getAllFields().size())
    return false;
  for (size_t I = 0; I < RA->getAllFields().size(); ++I) {
    Type *TA = RA->getAllFields()[I]->getType();
    Type *TB = RB->getAllFields()[I]->getType();
    if (TA != TB && !sameTupleShape(TA, TB))
      return false;
  }
  return true;
}


/// Can a value of type Value be assigned to a variable of type Target?
bool Checker::isAssignable(Type *Target, Type *Value){
  if (Target->isOpenArray() || Value->isOpenArray())
    return false; // only passed along and indexed
  if (containsFile(Target) || containsFile(Value))
    return false; // files cannot be copied
  if (Target == Value)
    return true;
  if (sameTupleShape(Target, Value))
    return true;
  // vector[N] of T: by shape (the same length and lane type), so the
  // vector of booleans a comparison gives fits any declared one
  if (auto *TV = llvm::dyn_cast<VectorType>(Target))
    if (auto *VV = llvm::dyn_cast<VectorType>(Value))
      return TV->getCount() == VV->getCount() &&
             TV->getElementType()->getBase() ==
                 VV->getElementType()->getBase();
  if (Target->isReal() && Value->isIntegerLike())
    return true;
  if (Target->isPointer() && (Value->isNil() || Value->isPointer()))
    return areCompatible(Target, Value);
  // Classes (objfpc): an object of the class or a descendant, or nil; any
  // object fits an untyped pointer.
  if (auto *TC = llvm::dyn_cast<ClassType>(Target))
    return Value->isNil() || (Value->isClass() &&
                              llvm::cast<ClassType>(Value)->inheritsFrom(TC));
  // Interfaces: nil, a descendant interface, an object of a class that
  // implements it (listed by the class or an ancestor).
  if (auto *TI = llvm::dyn_cast<InterfaceType>(Target)) {
    if (Value->isNil())
      return true;
    if (auto *VI = llvm::dyn_cast<InterfaceType>(Value))
      return VI->inheritsFrom(TI);
    if (auto *VC = llvm::dyn_cast<ClassType>(Value))
      return VC->findInterface(TI) != nullptr;
    return false;
  }
  // Class references: the class or a descendant, or nil.
  if (auto *TR = llvm::dyn_cast<ClassRefType>(Target))
    return Value->isNil() ||
           (Value->isClassRef() &&
            llvm::cast<ClassRefType>(Value)->getClass()->inheritsFrom(
                TR->getClass()));
  if (Target->isPointer() && (Value->isClass() || Value->isClassRef()))
    return llvm::cast<PointerType>(Target)->isUntyped();
  if (Target->isDynArray() || Target->isTask() || Target->isToken() ||
      Target->isIterator())
    return Value->isNil();
  if (Target->isShortString() && Value->isStringish())
    return true; // truncated to the target's length at runtime
  if (Target->isAnsiString())
    return Value->isStringish() || isPChar(Value);
  if (Target->isSet() && Value->isSet())
    return areCompatible(Target, Value); // range-checked at runtime
  // Procedural values (Turbo/FPC): congruent headings, or nil.
  if (auto *TR = llvm::dyn_cast<RoutineType>(Target)) {
    if (Value->isNil())
      return true;
    auto *VR = llvm::dyn_cast<RoutineType>(Value);
    return VR && VR->isCDecl() == TR->isCDecl() &&
           VR->isOfObject() == TR->isOfObject() &&
           VR->isReference() == TR->isReference() &&
           congruent(TR->getParams(), TR->getResult(), VR->getParams(),
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
bool Checker::isVariableReference(Expr *E){
  if (isBitField(E)) // assignable, but without an address
    return false;
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
    Decl *D = N->getDecl();
    return D && (llvm::isa<VarDecl>(D) || llvm::isa<FieldDecl>(D));
  }
  if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
    if (I->getBase()->getType() && (I->getBase()->getType()->isDynArray() ||
                                    I->getBase()->getType()->isPointer()))
      return !I->getProperty(); // on the heap, like p^; p[i]
    return !I->getProperty() && isVariableReference(I->getBase());
  }
  if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
    if (llvm::isa_and_nonnull<VarDecl>(F->getMember()))
      return true; // a class variable
    // obj.f is a variable like p^.f, whatever obj is (objfpc).
    if (isObject(F->getBase()))
      return llvm::isa_and_nonnull<FieldDecl>(F->getMember());
    return isVariableReference(F->getBase());
  }
  // T(x) of an untyped parameter x (Turbo/FPC)
  if (auto *C = llvm::dyn_cast<CallExpr>(E))
    return C->isVarCast() || C->isMethodCastOfVariable();
  // p^ is a variable even when p is a function result.
  return llvm::isa<DerefExpr>(E);
}


/// E (a bit field) where an address is needed.
void Checker::diagBitFieldAddress(Expr *E){
  Diags.error(E->getBeginLoc(),
              "the elements and fields of bitpacked arrays and records have "
              "no address (they cannot be passed as 'var' parameters or used "
              "with '@')",
              {E->getRange()});
}


/// Does T have a record laid out with a field alignment (packed, or
/// {$packrecords N}): itself, an element, a field?
bool Checker::containsAlignedRecord(Type *T){
  if (auto *A = llvm::dyn_cast<ArrayType>(T))
    return containsAlignedRecord(A->getElementType());
  if (auto *R = llvm::dyn_cast<RecordType>(T)) {
    if (R->getFieldAlignment())
      return true;
    for (FieldDecl *F : R->getAllFields())
      if (F->getType() && containsAlignedRecord(F->getType()))
        return true;
  }
  return false;
}


/// A type in T whose values a constant's byte image cannot hold (one that
/// is not an ordinal, real or short string, or an array or record of
/// those), or null. (The constants of records laid out with a field
/// alignment are their bytes.)
Type *Checker::notByteImage(Type *T){
  if (auto *A = llvm::dyn_cast<ArrayType>(T))
    return notByteImage(A->getElementType());
  if (auto *R = llvm::dyn_cast<RecordType>(T)) {
    for (FieldDecl *F : R->getAllFields())
      if (Type *Bad = F->getType() ? notByteImage(F->getType()) : nullptr)
        return Bad;
    return nullptr;
  }
  if (T->isOrdinal() || T->isReal() || T->isShortString() || T->isError())
    return nullptr;
  return T;
}


/// Does T have a bitpacked part (itself, an element, a field)?
bool Checker::containsBitPacked(Type *T){
  if (auto *A = llvm::dyn_cast<ArrayType>(T))
    return A->isBitPacked() || containsBitPacked(A->getElementType());
  if (auto *R = llvm::dyn_cast<RecordType>(T)) {
    if (R->isBitPacked())
      return true;
    for (FieldDecl *F : R->getAllFields())
      if (F->getType() && containsBitPacked(F->getType()))
        return true;
  }
  return false;
}


/// Does a value of the non-ordinal type T need no alignment (it can sit
/// at any byte of a bitpacked record)?
bool Checker::isByteAligned(Type *T){
  if (T->isOrdinal()) // as an array's element: stored in a byte?
    return T->getBase()->isBoolean() || T->getBase()->isChar() ||
           (T->isInteger() && llvm::cast<IntegerType>(T)->getBits() == 8);
  if (T->isShortString())
    return true;
  if (auto *R = llvm::dyn_cast<RecordType>(T))
    return R->isBitPacked();
  if (auto *A = llvm::dyn_cast<ArrayType>(T))
    return A->hasBitElements() || isByteAligned(A->getElementType());
  return false;
}


/// Is E a variable a type helper's method can change (not a method's
/// result: r.M, r.M.f)?
bool Checker::isHelperVariable(Expr *E){
  if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
    if (llvm::isa_and_nonnull<VarDecl>(F->getMember()))
      return true;
    if (!llvm::isa_and_nonnull<FieldDecl>(F->getMember()))
      return false;
    return isObject(F->getBase()) || isHelperVariable(F->getBase());
  }
  if (auto *I = llvm::dyn_cast<IndexExpr>(E);
      I && !I->getProperty() && I->getBase()->getType() &&
      !I->getBase()->getType()->isDynArray() &&
      !I->getBase()->getType()->isPointer())
    return isHelperVariable(I->getBase());
  return isVariableReference(E);
}


/// Is E (already checked) a value of a class type?
bool Checker::isObject(Expr *E){
  return E->getType() && E->getType()->isClass();
}


/// The `const` parameter a designator is part of (x, x[i], x.f; not x^ or
/// obj.f, whose target is a different variable), if any.
VarDecl *Checker::constParamRoot(Expr *E){
  while (true) {
    if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
      if (I->getProperty() || I->getBase()->getType()->isDynArray() ||
          I->getBase()->getType()->isPointer())
        return nullptr; // (a dynamic array's elements are not the parameter)
      E = I->getBase();
    } else if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
      if (isObject(F->getBase()) ||
          llvm::isa_and_nonnull<VarDecl>(F->getMember()))
        return nullptr;
      E = F->getBase();
    } else if (auto *P = llvm::dyn_cast<ParenExpr>(E)) {
      E = P->getInner();
    } else if (auto *C = llvm::dyn_cast<CallExpr>(E);
               C && (C->isVarCast() || C->isMethodCast())) {
      E = C->getArgs()[0]; // T(x): x
    } else {
      auto *N = llvm::dyn_cast<NameRefExpr>(E);
      auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(N->getDecl()) : nullptr;
      return V && (V->isConstParam() || V->isReadOnly()) ? V : nullptr;
    }
  }
}


/// Reports a change to a `const` parameter. Returns false if reported.
bool Checker::checkModifiable(Expr *E){
  if (VarDecl *OV = ownedVarOf(E); OV && E != OwnedTarget) {
    // (the callee could replace the object without freeing it)
    Diags.error(E->getBeginLoc(),
                "owned variable '" + OV->getName().Spelling +
                    "' cannot be passed as a variable parameter",
                {E->getRange()});
    return false;
  }
  VarDecl *V = constParamRoot(E);
  if (!V)
    return true;
  if (V->getOwner() && V->getOwner()->isCopyFactory())
    Diags.error(E->getBeginLoc(),
                "cannot modify '" + V->getName().Spelling +
                    "', a copy made by this lambda (copies are read-only)",
                {E->getRange()});
  else
    Diags.error(E->getBeginLoc(),
                llvm::Twine(V->isReadOnly() ? "cannot modify constant '"
                                            : "cannot modify constant parameter '") +
                    V->getName().Spelling + "'",
                {E->getRange()});
  noteDeclaredHere(V);
  return false;
}


//===--------------------------------------------------------------------===//
// Constant folding
//===--------------------------------------------------------------------===//

/// ISO 7185 6.3 constant: [sign] (number | constant-identifier) | string.
bool Checker::isISOConstantForm(Expr *E){
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


double Checker::asReal(const Constant &C){
  if (C.Value.K == ConstValue::Real)
    return C.Value.RealVal;
  if (C.Ty && isQWord(C.Ty->getBase()))
    return static_cast<double>(static_cast<uint64_t>(C.Value.Int));
  return static_cast<double>(C.Value.Int);
}


/// Does constant V of type VTy lie within T's values? (qword values are
/// unsigned.)
bool Checker::constantInRange(int64_t V, Type *VTy, Type *T){
  if (VTy->getBase()->isWideChar() && T->getBase()->isChar())
    V = wideToChar(V);
  bool Unsigned = isQWordValue(VTy);
  if (isQWord(T))
    return Unsigned || V >= 0;
  if (Unsigned && V < 0) // 2^63 or more
    return false;
  OrdinalBounds Bd = getOrdinalBounds(T);
  return V >= Bd.Lo && V <= Bd.Hi;
}


/// Folds an already type-checked expression. Returns nullopt if it is not
/// constant (without diagnosing).
std::optional<Constant> Checker::fold(Expr *E){
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
    auto *SL = llvm::cast<StringLiteralExpr>(E);
    if (SL->isWideCharLiteral())
      return Constant{T, ConstValue::ordinal(SL->getWideCode())};
    llvm::StringRef S = SL->getValue();
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
  case Expr::EK_Field: // TFoo.C
    if (NameRefExpr *Ref = llvm::cast<FieldExpr>(E)->getConstRef())
      return fold(Ref);
    return std::nullopt;
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
      return Constant{T, ConstValue::ordinal(castOrdinalValue(~V->Value.Int, T, T))};
    }
    return std::nullopt;
  }
  case Expr::EK_Binary: {
    auto *B = llvm::cast<BinaryExpr>(E);
    if (NameRefExpr *Ref = B->getConstRef()) // 'a' + 'b'
      return fold(Ref);
    auto L = fold(B->getLHS()), R = fold(B->getRHS());
    if (!L || !R || L->Value.K == ConstValue::String ||
        R->Value.K == ConstValue::String)
      return std::nullopt;
    if (!T->isOrdinal() && !T->isReal())
      return std::nullopt; // 'a' + 'b': a string (not folded)
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
    // qword operands: div, mod and comparisons are unsigned.
    bool U64 = !IsReal && B->getOperandType() && isQWord(B->getOperandType());
    switch (B->getOp()) {
    case BinaryOp::Add: return IsReal ? Real(X + Y) : Ord(static_cast<int64_t>(UA + UC));
    case BinaryOp::Sub: return IsReal ? Real(X - Y) : Ord(static_cast<int64_t>(UA - UC));
    case BinaryOp::Mul: return IsReal ? Real(X * Y) : Ord(static_cast<int64_t>(UA * UC));
    case BinaryOp::RealDiv:
      if (Y == 0)
        return std::nullopt;
      return Real(X / Y);
    case BinaryOp::IntDiv:
      if (U64)
        return UC == 0 ? std::nullopt
                       : std::optional(Ord(static_cast<int64_t>(UA / UC)));
      if (C == 0 || (A == INT64_MIN && C == -1))
        return std::nullopt;
      return Ord(A / C);
    case BinaryOp::Mod:
      if (U64)
        return UC == 0 ? std::nullopt
                       : std::optional(Ord(static_cast<int64_t>(UA % UC)));
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
    case BinaryOp::Lt:  return Ord(IsReal ? X < Y : U64 ? UA < UC : A < C);
    case BinaryOp::Le:  return Ord(IsReal ? X <= Y : U64 ? UA <= UC : A <= C);
    case BinaryOp::Gt:  return Ord(IsReal ? X > Y : U64 ? UA > UC : A > C);
    case BinaryOp::Ge:  return Ord(IsReal ? X >= Y : U64 ? UA >= UC : A >= C);
    case BinaryOp::In: case BinaryOp::Is: case BinaryOp::As:
      return std::nullopt;
    }
    return std::nullopt;
  }
  case Expr::EK_Call: {
    auto *Call = llvm::cast<CallExpr>(E);
    if (llvm::isa_and_nonnull<TypeDecl>(Call->getDecl())) { // T(x)
      auto V = fold(Call->getArgs()[0]);
      if (!V || V->Value.K != ConstValue::Ordinal || !T->isOrdinal())
        return std::nullopt;
      return Constant{T, ConstValue::ordinal(
                             castOrdinalValue(V->Value.Int, V->Ty, T))};
    }
    auto *BD = llvm::dyn_cast_or_null<BuiltinDecl>(Call->getDecl());
    if (!BD || Call->getArgs().size() != 1)
      return std::nullopt;
    if (BD->getBuiltinKind() == BuiltinKind::Low ||
        BD->getBuiltinKind() == BuiltinKind::High) {
      bool IsHigh = BD->getBuiltinKind() == BuiltinKind::High;
      Type *AT = Call->getArgs()[0]->getType();
      if (AT->isArrayOfConst() || AT->isOpenArray() || AT->isDynArray())
        return std::nullopt; // known only when the program runs
      if (auto *SS = llvm::dyn_cast<ShortStringType>(AT))
        return Constant{T, ConstValue::ordinal(IsHigh ? SS->getMaxLength() : 0)};
      if (auto *A = llvm::dyn_cast<ArrayType>(AT))
        AT = A->getIndexType();
      OrdinalBounds Bd = getOrdinalBounds(AT);
      if (IsHigh && isQWord(AT)) // 2^64-1: its 64 bits
        return Constant{T, ConstValue::ordinal(-1)};
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
      if (isQWordValue(V->Ty)) // already non-negative
        return Ord(V->Value.Int);
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
std::optional<Constant> Checker::evaluateConstant(Expr *E, llvm::StringRef What){
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
      !isQWordValue(C->Ty)) { // (any 64 bits are a qword)
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


/// The value of a typecast T(V) of a value of type From: integer types
/// wrap to their width (byte(300) = 44), and widechar to 16 bits; a
/// widechar above 255 is '?' as a char (as Free Pascal converts it);
/// other ordinal types keep the value.
int64_t Checker::castOrdinalValue(int64_t V, Type *From, Type *T){
  if (T->getBase()->isWideChar())
    return V & 0xFFFF;
  if (T->getBase()->isChar() && From->getBase()->isWideChar())
    return wideToChar(V);
  auto *I = llvm::dyn_cast<IntegerType>(T->getBase());
  if (!I || I->getBits() == 64)
    return V;
  uint64_t Mask = (uint64_t(1) << I->getBits()) - 1;
  uint64_t U = static_cast<uint64_t>(V) & Mask;
  if (I->isSigned() && (U >> (I->getBits() - 1)))
    U |= ~Mask; // sign-extend
  return static_cast<int64_t>(U);
}


/// A widechar's code as a char: '?' above 255 (Free Pascal's
/// conversion).
int64_t Checker::wideToChar(int64_t V){ return V > 255 ? '?' : V; }


/// A widechar constant above 255 that becomes a char or a string: a
/// warning, as Free Pascal gives ('?' in its place).
void Checker::checkWideToChar(Type *Target, Expr *E){
  if (!E->getType() || !E->getType()->getBase()->isWideChar() ||
      !(Target->getBase()->isChar() ||
        (Target->isStringish() && !Target->isCharLike())))
    return;
  auto C = fold(E);
  if (C && C->Value.K == ConstValue::Ordinal && C->Value.Int > 255)
    Diags.warning(E->getBeginLoc(),
                  "the widechar #" + std::to_string(C->Value.Int) +
                      " becomes '?' in " + quoted(Target),
                  {E->getRange()});
}


std::string Checker::formatValue(Type *T, int64_t V){
  Type *B = T->getBase();
  if (B->isWideChar() && (V > 127 || !llvm::isPrint(static_cast<char>(V))))
    return "#" + std::to_string(V);
  if (B->isChar() || B->isWideChar())
    return llvm::isPrint(static_cast<char>(V))
               ? "'" + std::string(1, static_cast<char>(V)) + "'"
               : "chr(" + std::to_string(V) + ")";
  if (B->isBoolean())
    return V ? "true" : "false";
  if (auto *E = llvm::dyn_cast<EnumType>(B))
    if (V >= 0 && static_cast<size_t>(V) < E->getValues().size())
      return E->getValues()[V]->getName().Spelling.str();
  if (isQWord(B))
    return std::to_string(static_cast<uint64_t>(V));
  return std::to_string(V);
}


//===--------------------------------------------------------------------===//
// Types
//===--------------------------------------------------------------------===//

Type *Checker::resolveType(TypeSpec *TS, llvm::StringRef DeclName){
  if (Type *T = TS->getResolved())
    return T;
  Type *T = buildType(TS);
  if (TS->isNullable() && !T->isError()) { // T?
    if (!isNullRef(T)) {
      Diags.error(TS->getBeginLoc(),
                  "'?' makes a class, interface or class-reference type "
                  "nullable; " + quoted(T) + " is not one",
                  {TS->getRange()});
      T = errorType();
    } else if (!DeclName.empty()) {
      Diags.error(TS->getBeginLoc(),
                  "a nullable type cannot be given a name; write " +
                      ("'" + T->getName() + "?'") + " where it is used",
                  {TS->getRange()});
      T = errorType();
    }
  }
  auto *Rec = llvm::dyn_cast<RecordType>(T);
  if (!DeclName.empty() && !T->isError() && !llvm::isa<BuiltinType>(T) &&
      !llvm::isa<SpecializeTypeSpec>(TS) && // (so is a specialization)
      !(Rec && Rec->isTuple()) && // (a tuple type is shared by its uses)
      !T->isTask() && !T->isIterator()) // (and so are tasks and iterators)
    T->setDeclName(DeclName);
  TS->setResolved(T);
  return T;
}


Type *Checker::buildType(TypeSpec *TS){
  switch (TS->getKind()) {
  case TypeSpec::TS_ArrayOfConst:
    return ArrayOfConstTy;
  case TypeSpec::TS_DynArray: {
    auto *DS = llvm::cast<DynArrayTypeSpec>(TS);
    Type *Elem = resolveType(DS->getElement());
    if (Elem->isError())
      return errorType();
    if (containsFile(Elem)) {
      Diags.error(DS->getElement()->getBeginLoc(),
                  "the elements of a dynamic array cannot be files",
                  {DS->getElement()->getRange()});
      return errorType();
    }
    return Ctx.getDynArrayType(Elem);
  }
  case TypeSpec::TS_OpenArray: {
    auto *OS = llvm::cast<OpenArrayTypeSpec>(TS);
    Type *Elem = resolveType(OS->getElement());
    if (Elem->isError())
      return errorType();
    if (containsFile(Elem)) {
      Diags.error(OS->getElement()->getBeginLoc(),
                  "the elements of an open array cannot be files",
                  {OS->getElement()->getRange()});
      return errorType();
    }
    return Ctx.getOpenArrayType(Elem);
  }
  case TypeSpec::TS_Untyped:
    return Ctx.getUntypedType();
  case TypeSpec::TS_Named: {
    const Identifier &Id = llvm::cast<NamedTypeSpec>(TS)->getName();
    if (auto Quals = llvm::cast<NamedTypeSpec>(TS)->getQualifiers();
        !Quals.empty()) { // TOuter.TInner
      Decl *D = lookupMemberPath(Quals, Id);
      if (!D)
        return errorType();
      if (auto *TD = llvm::dyn_cast<TypeDecl>(D)) {
        warnHints(TD, Id);
        return TD->getType() ? TD->getType() : errorType();
      }
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' is a " + D->getKindName() +
                      ", not a type",
                  {Id.getRange()});
      noteDeclaredHere(D);
      return errorType();
    }
    Decl *D = Cur->lookup(Id.Name);
    if (llvm::isa_and_nonnull<TypeDecl>(D))
      warnHints(D, Id);
    if (!D) {
      if (isFutureBuiltin(Id.Name))
        Diags.error(Id.Loc, "type '" + Id.Spelling + "' is not supported yet",
                    {Id.getRange()});
      else if (isGenericName(Id.Name))
        Diags.error(Id.Loc,
                    "'" + Id.Spelling + "' is a generic; give its type "
                    "arguments: specialize " + Id.Spelling + "<...>",
                    {Id.getRange()});
      else
        Diags.error(Id.Loc, "unknown type name '" + Id.Spelling + "'" +
                      didYouMean(Id.Name),
                    {Id.getRange()});
      return errorType();
    }
    if (auto *TD = llvm::dyn_cast<TypeDecl>(D)) {
      if (auto *C = llvm::dyn_cast_or_null<ClassType>(TD->getType());
          C && C->isHelper()) {
        Diags.error(Id.Loc,
                    C->helperKindName() + " '" + Id.Spelling +
                        "' cannot be used as a type",
                    {Id.getRange()});
        return errorType();
      }
      return TD->getType() ? TD->getType() : errorType();
    }
    Diags.error(Id.Loc,
                "'" + Id.Spelling + "' is a " + D->getKindName() +
                    ", not a type",
                {Id.getRange()});
    noteDeclaredHere(D);
    return errorType();
  }

  case TypeSpec::TS_Specialize: {
    auto *SS = llvm::cast<SpecializeTypeSpec>(TS);
    TypeDecl *I = specializeType(SS->getName(), SS->getArgs());
    if (!I)
      return errorType();
    if (I->getType())
      return I->getType();
    // The specialization being declared, inside itself: as its name.
    Identifier Id = I->getName();
    Id.Loc = SS->getName().Loc;
    return resolveType(Ctx.create<NamedTypeSpec>(Id));
  }

  case TypeSpec::TS_Tuple: {
    auto *TS2 = llvm::cast<TupleTypeSpec>(TS);
    llvm::SmallVector<Type *, 4> Types;
    llvm::SmallVector<Identifier, 4> Names;
    bool OK = true;
    for (const TupleElementSpec &El : TS2->getElements()) {
      Type *T = resolveType(El.Type);
      if (T->isError()) {
        OK = false;
      } else if (containsFile(T)) {
        Diags.error(El.Type->getBeginLoc(),
                    "the elements of a tuple cannot be files",
                    {El.Type->getRange()});
        OK = false;
      }
      Identifier Name = El.Name;
      if (!Name.isValid())
        Name = tupleItemName(Names.size() + 1, El.Type->getBeginLoc());
      for (const Identifier &Prev : Names)
        if (Prev.Name == Name.Name) {
          Diags.error(El.Name.isValid() ? El.Name.Loc
                                        : El.Type->getBeginLoc(),
                      "duplicate element name '" + Name.Spelling + "'",
                      {El.Type->getRange()});
          OK = false;
          break;
        }
      Types.push_back(T);
      Names.push_back(Name);
    }
    if (!OK)
      return errorType();
    return getTupleType(Types, Names);
  }

  case TypeSpec::TS_Enum: {
    auto *ES = llvm::cast<EnumTypeSpec>(TS);
    // (integer, string): every name is a type's, so it is a tuple type.
    if (TupleTypeSpec *TT = ES->getTuple()) {
      bool AllTypes = true;
      for (const Identifier &Id : ES->getNames())
        AllTypes &= llvm::isa_and_nonnull<TypeDecl>(Cur->lookup(Id.Name));
      if (AllTypes)
        return resolveType(TT);
    }
    auto *E = Ctx.create<EnumType>();
    llvm::SmallVector<ConstDecl *, 8> Values;
    for (const Identifier &Id : ES->getNames()) {
      auto *C = Ctx.create<ConstDecl>(
          Id.Loc, Id, E, ConstValue::ordinal(static_cast<int64_t>(Values.size())));
      if (!ES->isScoped()) {
        declare(C);
      } else { // reached only as TColor.Red: not in the scope
        for (ConstDecl *Prev : Values)
          if (Prev->getName().Name == Id.Name) {
            Diags.error(Id.Loc, "redefinition of '" + Id.Spelling + "'",
                        {Id.getRange()});
            Diags.note(Prev->getLocation(), "previous definition is here");
            break;
          }
      }
      Values.push_back(C);
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
    if (Hi->Ty->getBase()->isWideChar()) // 'a'..#$100: of widechar
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
    for (auto It = Indexes.rbegin(); It != Indexes.rend(); ++It) {
      auto *A = Ctx.create<ArrayType>(*It, T, AS->isPacked());
      if (AS->isBitPacked())
        A->setBitPacked();
      T = A;
    }
    return T;
  }

  case TypeSpec::TS_Pointer: {
    // Always resolved at the end of the type section: `^T` binds to a T
    // declared later in the same block even if an outer T is visible.
    auto *P = Ctx.create<PointerType>(nullptr);
    PendingPointers.push_back({llvm::cast<PointerTypeSpec>(TS), P});
    return P;
  }

  case TypeSpec::TS_Async: {
    auto *AS = llvm::cast<AsyncTypeSpec>(TS);
    Type *Inner = nullptr;
    if (AS->getInner()) {
      Inner = resolveType(AS->getInner());
      if (Inner->isError())
        return errorType();
    }
    if (AS->isIterator()) {
      if (Inner->isUntyped() || containsFile(Inner)) {
        Diags.error(AS->getInner()->getBeginLoc(),
                    "the values of an iterator cannot be " + quoted(Inner),
                    {AS->getInner()->getRange()});
        return errorType();
      }
      return Ctx.getIteratorType(Inner);
    }
    return Ctx.getTaskType(Inner);
  }

  case TypeSpec::TS_Routine: {
    auto *RS = llvm::cast<RoutineTypeSpec>(TS);
    for (VarDecl *P : RS->getParams()) {
      P->setType(resolveType(P->getTypeSpec()));
      checkParamDefault(P);
    }
    Type *Result = nullptr;
    if (RS->isFunction()) {
      Result = resolveType(RS->getResult());
      bool MethodResult =
          (RS->isOfObject() || RS->isReference()) && !Result->isError() &&
          (Result->isShortString() || isMethodPointer(Result) ||
           (Result->isRecord() && !containsFile(Result) &&
            !containsInterface(Result)));
      if (!Result->isError() && !Result->isValidResultType() && !MethodResult) {
        Diags.error(RS->getResult()->getBeginLoc(),
                    "function result type must be a simple or pointer "
                    "type, not " + quoted(Result),
                    {RS->getResult()->getRange()});
        Result = errorType();
      }
    }
    return Ctx.create<RoutineType>(RS->getParams(), Result,
                                   RS->getCallConv() == CallConv::CDecl,
                                   RS->isOfObject(), RS->isReference());
  }

  case TypeSpec::TS_String: {
    auto *SS = llvm::cast<StringTypeSpec>(TS);
    if (!SS->getMaxLength())
      return SS->isLong() ? Ctx.getAnsiStringType()
                          : Ctx.getShortStringType(255);
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
    if (!FS->getElement()) // file (Turbo/FPC): untyped
      return Ctx.create<FileType>(nullptr, false);
    Type *Elem = resolveType(FS->getElement());
    if (Elem->isError())
      return errorType();
    if (containsFile(Elem)) {
      Diags.error(FS->getElement()->getBeginLoc(),
                  "the components of a file cannot be files",
                  {FS->getElement()->getRange()});
      return errorType();
    }
    if (isManaged(Elem)) {
      Diags.error(FS->getElement()->getBeginLoc(),
                  Elem->isAnsiString() ? "the components of a file cannot be ansistrings"
                  : Elem->isDynArray()
                      ? "the components of a file cannot be dynamic arrays"
                      : "the components of a file cannot be COM interfaces",
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

  case TypeSpec::TS_Vector: {
    auto *VS = llvm::cast<VectorTypeSpec>(TS);
    Type *Elem = resolveType(VS->getElementType());
    if (Elem->isError())
      return errorType();
    if (!Elem->isOrdinal() && !Elem->isReal()) {
      Diags.error(VS->getElementType()->getBeginLoc(),
                  "a vector's element type must be ordinal or real, not " +
                      quoted(Elem),
                  {VS->getElementType()->getRange()});
      return errorType();
    }
    if (isQWord(Elem)) {
      Diags.error(VS->getElementType()->getBeginLoc(),
                  "a vector's element type cannot be qword");
      return errorType();
    }
    auto CN = evaluateConstant(VS->getCountExpr(), "vector length");
    if (!CN || CN->Value.K != ConstValue::Ordinal) {
      Diags.error(VS->getCountExpr()->getBeginLoc(),
                  "a vector's length must be a constant");
      return errorType();
    }
    int64_t N = CN->Value.Int;
    if (N < 1 || N > 64) {
      Diags.error(VS->getCountExpr()->getBeginLoc(),
                  "a vector's length must be 1..64");
      return errorType();
    }
    return Ctx.create<VectorType>(Elem, N);
  }

  case TypeSpec::TS_Class:
    Diags.error(TS->getBeginLoc(),
                "a class type must be declared in a 'type' section",
                {TS->getRange()});
    return errorType();

  case TypeSpec::TS_Interface:
    Diags.error(TS->getBeginLoc(),
                "an interface type must be declared in a 'type' section",
                {TS->getRange()});
    return errorType();

  case TypeSpec::TS_Helper:
    Diags.error(TS->getBeginLoc(),
                llvm::Twine("a ") +
                    (llvm::cast<HelperTypeSpec>(TS)->isRecordHelper()
                         ? "record helper"
                         : "type helper") +
                    " must be declared in a 'type' section",
                {TS->getRange()});
    return errorType();

  case TypeSpec::TS_ClassRef: { // class of TFoo (TFoo may be forward)
    const Identifier &Id = llvm::cast<ClassRefTypeSpec>(TS)->getClassName();
    Decl *D = Cur->lookup(Id.Name);
    auto *TD = llvm::dyn_cast_or_null<TypeDecl>(D);
    if (TD && TD->getType() && TD->getType()->isError())
      return errorType();
    auto *C = TD ? llvm::dyn_cast_or_null<ClassType>(TD->getType()) : nullptr;
    if (!C) {
      if (!D)
        Diags.error(Id.Loc,
                    "unknown class name '" + Id.Spelling + "'" +
                        didYouMean(Id.Name),
                    {Id.getRange()});
      else {
        Diags.error(Id.Loc, "'" + Id.Spelling + "' is not a class type",
                    {Id.getRange()});
        noteDeclaredHere(D);
      }
      return errorType();
    }
    return Ctx.create<ClassRefType>(C);
  }

  case TypeSpec::TS_Record: {
    auto *RS = llvm::cast<RecordTypeSpec>(TS);
    if (!RS->getMembers().empty() && RS != NamedAdvancedRecord)
      Diags.error(RS->getBeginLoc(),
                  "methods are not allowed in anonymous or local records; "
                  "declare the record type in a 'type' section at program "
                  "or unit level",
                  {RS->getRange()});
    llvm::SmallVector<FieldDecl *, 16> All;
    llvm::StringMap<FieldDecl *> Seen;
    if (!resolveFieldList(RS->getFields(), All, Seen))
      return errorType();
    auto *R = Ctx.create<RecordType>(RS->getFields(),
                                     Ctx.copyArray<FieldDecl *>(All),
                                     RS->isPacked());
    R->setPackRecords(RS->getPackRecords());
    if (RS->isBitPacked()) {
      R->setBitPacked();
      for (FieldDecl *F : All) // ordinals, and what needs no alignment
        if (Type *FT = F->getType();
            FT && !FT->isError() && !FT->isOrdinal() && !isByteAligned(FT))
          Diags.error(F->getLocation(),
                      "a field of type " + quoted(FT) +
                          " in a bitpacked record is not supported yet",
                      {F->getName().getRange()});
    }
    return R;
  }
  }
  return errorType();
}


/// The name of a tuple's N-th element that has none: ItemN.
Identifier Checker::tupleItemName(size_t N, SourceLocation Loc){
  std::string S = "Item" + std::to_string(N);
  return {Ctx.getStrings().intern(llvm::StringRef(S).lower()),
          Ctx.getStrings().intern(S), Loc};
}


/// The tuple type with these element types and names (Names empty:
/// Item1, Item2, ...). There is one for each such list, so that tuple
/// types written alike are the same type.
RecordType *Checker::getTupleType(llvm::ArrayRef<Type *> Types,
                         llvm::ArrayRef<Identifier> Names){
  llvm::SmallVector<Identifier, 4> Own;
  if (Names.empty()) {
    for (size_t I = 0; I < Types.size(); ++I)
      Own.push_back(tupleItemName(I + 1, SourceLocation()));
    Names = Own;
  }
  for (RecordType *R : TupleTypes) {
    auto Fields = R->getAllFields();
    bool Same = Fields.size() == Types.size();
    for (size_t I = 0; Same && I < Fields.size(); ++I)
      Same = Fields[I]->getType() == Types[I] &&
             Fields[I]->getName().Name == Names[I].Name;
    if (Same)
      return R;
  }
  llvm::SmallVector<FieldDecl *, 4> Fields;
  for (size_t I = 0; I < Types.size(); ++I) {
    auto *F = Ctx.create<FieldDecl>(Names[I], nullptr);
    F->setType(Types[I]);
    Fields.push_back(F);
  }
  auto *FL = Ctx.create<FieldList>();
  FL->Fixed = Ctx.copyArray<FieldDecl *>(Fields);
  auto *R = Ctx.create<RecordType>(FL, FL->Fixed, /*Packed=*/false);
  R->setTuple();
  TupleTypes.push_back(R);
  return R;
}


/// A hidden variable of type T of the block being checked (a tuple is
/// built in one); null outside the statements of a block.
VarDecl *Checker::makeHiddenVar(Type *T, SourceLocation Loc){
  if (!InlineVars)
    return nullptr;
  std::string S = "$tuple" + std::to_string(++HiddenVars);
  llvm::StringRef N = Ctx.getStrings().intern(S);
  auto *V = Ctx.create<VarDecl>(Identifier{N, N, Loc}, nullptr);
  V->setInline();
  V->setType(T);
  V->setOwner(CurRoutine);
  Used.insert(V);
  InlineVars->push_back(V);
  return V;
}


bool Checker::addField(FieldDecl *F, llvm::SmallVectorImpl<FieldDecl *> &All,
              llvm::StringMap<FieldDecl *> &Seen){
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


bool Checker::resolveFieldList(FieldList *FL, llvm::SmallVectorImpl<FieldDecl *> &All,
                      llvm::StringMap<FieldDecl *> &Seen){
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
    size_t First = All.size();
    OK &= resolveFieldList(V->Fields, All, Seen);
    // The variants overlap, so their fields cannot be reference counted.
    for (size_t I = First; I < All.size(); ++I)
      if (All[I]->getType() && isManaged(All[I]->getType())) {
        Diags.error(All[I]->getLocation(),
                    All[I]->getType()->isAnsiString()
                        ? "a variant part cannot contain ansistrings"
                    : All[I]->getType()->isDynArray()
                        ? "a variant part cannot contain dynamic arrays"
                        : "a variant part cannot contain COM interfaces",
                    {All[I]->getName().getRange()});
        OK = false;
      }
  }
  return OK;
}


//===--------------------------------------------------------------------===//
// Declarations
//===--------------------------------------------------------------------===//

void Checker::declareConst(ConstDecl *C){
  // const X = F(args), F compiletime: the driver replaces it with the
  // value at the level of a program or unit (Compiletime.cpp).
  if (RoutineDecl *F = compiletimeCallee(C->getInit())) {
    if (LangOpts.CompiletimeDeferred && !CurRoutine) {
      C->setValue(errorType(), ConstValue());
      declare(C);
      return;
    }
    Diags.error(C->getInit()->getBeginLoc(),
                "the value of compiletime function '" +
                    F->getName().Spelling +
                    "' is computed only for a constant declared at the "
                    "level of a program or unit",
                {C->getInit()->getRange()});
    C->setValue(errorType(), ConstValue());
    declare(C);
    return;
  }
  if (auto V = evaluateConstant(C->getInit(), "constant value"))
    C->setValue(V->Ty, V->Value);
  else
    C->setValue(errorType(), ConstValue());
  declare(C);
}


/// The arguments of D's attributes must be constants. (An attribute names
/// a source generator; nothing here gives one a meaning.)
void Checker::checkAttributes(Decl *D){
  if (D->getAttributes().empty() || !AttributesChecked.insert(D).second)
    return;
  for (const Attribute &A : D->getAttributes())
    for (Expr *Arg : A.Args)
      evaluateConstant(Arg, "an argument of attribute '" +
                                A.Name.Spelling.str() + "'");
}


/// The attributes of a type declaration and of its fields and members.
void Checker::checkTypeAttributes(TypeDecl *TD){
  checkAttributes(TD);
  TypeSpec *TS = TD->getTypeSpec();
  llvm::ArrayRef<Decl *> Members;
  if (auto *CS = llvm::dyn_cast_or_null<ClassTypeSpec>(TS)) {
    Members = CS->getMembers();
  } else if (auto *RS = llvm::dyn_cast_or_null<RecordTypeSpec>(TS)) {
    Members = RS->getMembers();
    if (FieldList *FL = RS->getFields())
      for (FieldDecl *F : FL->Fixed)
        checkAttributes(F);
  }
  for (Decl *M : Members)
    checkAttributes(M);
}


void Checker::declareType(TypeDecl *TD){
  checkTypeAttributes(TD);
  if (TD->isGeneric())
    return declareGenericType(TD);
  if (auto *CS = llvm::dyn_cast<ClassTypeSpec>(TD->getTypeSpec()))
    return declareClass(TD, CS);
  if (auto *HS = llvm::dyn_cast<HelperTypeSpec>(TD->getTypeSpec()))
    return declareHelper(TD, HS);
  if (auto *IS = llvm::dyn_cast<InterfaceTypeSpec>(TD->getTypeSpec()))
    return declareInterface(TD, IS);
  auto *RS = llvm::dyn_cast<RecordTypeSpec>(TD->getTypeSpec());
  if (RS && !RS->getMembers().empty() && !CurRoutine)
    return declareAdvancedRecord(TD, RS);
  TD->setType(resolveType(TD->getTypeSpec(), typeDeclName(TD->getName())));
  declare(TD);
}


/// The variable that E, the target of an `absolute`, is in: E is a
/// variable, a field of a record in one or an element at a constant
/// index of an array in one. Null for anything else.
VarDecl *Checker::absoluteRootOf(Expr *E){
  if (isBitField(E))
    return nullptr;
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
    auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl());
    return V && !N->getWith() && !V->getType()->isArrayOfConst() ? V : nullptr;
  }
  if (auto *F = llvm::dyn_cast<FieldExpr>(E))
    return F->getBase()->getType()->isRecord() &&
                   llvm::isa_and_nonnull<FieldDecl>(F->getMember())
               ? absoluteRootOf(F->getBase())
               : nullptr;
  if (auto *I = llvm::dyn_cast<IndexExpr>(E))
    return !I->getProperty() && llvm::isa<ArrayType>(I->getBase()->getType()) &&
                   fold(I->getIndex())
               ? absoluteRootOf(I->getBase())
               : nullptr;
  return nullptr;
}


/// `V: T absolute Target`, as in Free Pascal. Checked before V is
/// declared, so that it cannot name itself.
void Checker::checkAbsolute(VarDecl *V, Expr *Target){
  // The name of an enclosing function is its result.
  auto *N = llvm::dyn_cast<NameRefExpr>(Target);
  auto *R = N ? llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookup(N->getName().Name))
              : nullptr;
  if (R && R->isFunction())
    for (RoutineDecl *Enc = CurRoutine; Enc; Enc = Enc->getParent())
      for (RoutineDecl *X = R; X; X = X->getNextOverload())
        if (Enc == X) {
          if (X != CurRoutine)
            X->getResultVar()->setCaptured();
          N->setDecl(X->getResultVar());
          N->setType(X->getResultType());
          ResultAssigned.insert(X);
          V->setAbsoluteRoot(X->getResultVar());
          return;
        }
  if (checkAllowingUntyped(Target)->isError())
    return;
  VarDecl *Root = absoluteRootOf(Target);
  if (!Root) {
    bool IsConstant = llvm::isa<IntegerLiteralExpr>(Target) ||
                      (N && llvm::isa_and_nonnull<ConstDecl>(N->getDecl()));
    Diags.error(Target->getBeginLoc(),
                IsConstant ? "'absolute' with a memory address is not supported"
                           : "'absolute' needs a variable, a field of a record "
                             "variable or an element at a constant index",
                {Target->getRange()});
    return;
  }
  V->setAbsoluteRoot(Root);
  if (Root->getVarKind() == VarDecl::Result) // objfpc `Result`
    ResultAssigned.insert(Root->getOwner());
}


/// x: T; [cvar;] external ['lib'] [name 'sym'];  -- a C variable, at
/// program or unit level; the library and symbol: string constants.
void Checker::checkExternalVar(VarDecl *V){
  const Identifier &Id = V->getName();
  Used.insert(V);
  if (!V->isExternal()) {
    Diags.error(Id.Loc,
                "'cvar' without 'external' (a variable for C code to use) "
                "is not supported yet",
                {Id.getRange()});
    return;
  }
  if (CurRoutine)
    Diags.error(Id.Loc,
                "external variable '" + Id.Spelling +
                    "' must be declared at program or unit level",
                {Id.getRange()});
  else if (V->getAbsolute() || V->getInit())
    Diags.error(Id.Loc,
                "external variable '" + Id.Spelling + "' cannot be " +
                    (V->getAbsolute() ? "'absolute'" : "initialized"),
                {Id.getRange()});
  if (Expr *L = V->getLibraryExpr())
    if (auto S = externalString(L, "library name", "variable"))
      V->setLibraryName(*S);
  if (Expr *N = V->getNameExpr())
    if (auto S = externalString(N, "symbol name", "variable"))
      V->setExternalName(*S);
}


void Checker::declareVar(VarDecl *V){
  V->setType(resolveType(V->getTypeSpec()));
  V->setOwner(CurRoutine);
  checkOwnedDecl(V);
  if (V->isExternal() || V->isCVar())
    checkExternalVar(V);
  if (Expr *Target = V->getAbsolute())
    checkAbsolute(V, Target);
  declareVarInit(V);
}


/// `x: T owned`: a variable of a routine (or one declared among the
/// statements), of a class type.
void Checker::checkOwnedDecl(VarDecl *V){
  if (V->isOwned()) {
    const Identifier &Id = V->getName();
    bool OK = false;
    if (!CurRoutine && !V->isInline())
      Diags.error(Id.Loc,
                  "'owned' is only allowed on the local variables of a "
                  "routine",
                  {Id.getRange()});
    else if (!V->getType()->isError() && !V->getType()->isClass())
      Diags.error(Id.Loc,
                  "owned variable '" + Id.Spelling +
                      "' must be of a class type, not " +
                      quoted(V->getType()),
                  {Id.getRange()});
    else if (V->getAbsolute() || V->getInit())
      Diags.error(Id.Loc,
                  "owned variable '" + Id.Spelling + "' cannot be " +
                      (V->getAbsolute() ? "'absolute'" : "initialized"),
                  {Id.getRange()});
    else
      OK = !V->getType()->isError();
    V->setOwned(OK);
    if (OK)
      markHasTry(); // freed in a finally part around the routine's body
  }
  checkRaceDecl(V->getRace(), V->getType(), V->getName(), "variable");
}


/// Can values of type T be shared between tasks? A task, a token, or an
/// object of a class derived from TSharedObject (which synchronizes what
/// it shares).
bool Checker::isThreadSafe(Type *T) const{
  if (T->isTask() || T->isToken())
    return true;
  auto *C = llvm::dyn_cast<ClassType>(T);
  return C && TSharedObjectTy && C->inheritsFrom(TSharedObjectTy);
}


/// Does a value of type T give access to data that others may change (an
/// object that is not thread-safe, a pointer, a dynamic array, an
/// interface)?
bool Checker::sharesMutableData(Type *T) const{
  if (isThreadSafe(T))
    return false;
  return T->isClass() || T->isPointer() || T->isDynArray() || T->isInterface();
}


/// `shared T`: T thread-safe; `isolated T`: T a reference (a class, a
/// dynamic array or a pointer), which `move` hands over.
void Checker::checkRaceDecl(RaceQualifier Q, Type *T, const Identifier &Id,
                   const char *What){
  if (Q == RaceQualifier::None || T->isError())
    return;
  if (Q == RaceQualifier::Shared && !isThreadSafe(T))
    Diags.error(Id.Loc,
                llvm::Twine("shared ") + What + " '" + Id.Spelling +
                    "' must be of a thread-safe type (a class derived from "
                    "TSharedObject, such as TAtomic<T>, TLocked<T> or "
                    "TChannel<T>; a task; a TCancelToken), not " + quoted(T),
                {Id.getRange()});
  if (Q == RaceQualifier::Isolated && !T->isClass() && !T->isDynArray() &&
      !T->isPointer())
    Diags.error(Id.Loc,
                llvm::Twine("isolated ") + What + " '" + Id.Spelling +
                    "' must be of a class, dynamic array or pointer type, "
                    "not " + quoted(T),
                {Id.getRange()});
}


void Checker::declareVarInit(VarDecl *V){
  if (Expr *Init = V->getInit()) {
    // Typed constants are static: they keep their value across calls.
    // (An initialized variable is set again whenever its routine runs.)
    if (V->isTypedConst()) {
      V->setOwner(nullptr);
      Used.insert(V);
    }
    ConstValue Folded;
    ConstInitWhat = V->isTypedConst() ? "typed constant value" : "initial value";
    Type *Unimaged = containsAlignedRecord(V->getType())
                         ? notByteImage(V->getType())
                         : nullptr;
    if (containsBitPacked(V->getType()))
      Diags.error(Init->getBeginLoc(),
                  std::string(V->isTypedConst() ? "typed constants"
                                                : "initialized variables") +
                      " of bitpacked types are not supported yet",
                  {Init->getRange()});
    else if (Unimaged)
      Diags.error(Init->getBeginLoc(),
                  std::string(V->isTypedConst() ? "typed constants"
                                                : "initialized variables") +
                      " of packed records or records under {$packrecords} "
                      "with values of type " +
                      quoted(Unimaged) + " are not supported yet",
                  {Init->getRange()});
    else if (!V->getType()->isError() &&
             checkConstInit(Init, V->getType(), Folded))
      V->setInitValue(Folded);
    ConstInitWhat = "typed constant value";
  }
  declare(V);
}


/// Is T an array of char (any index type) that a string constant of
/// length N initializes (Turbo/FPC)?
bool Checker::isCharArrayOfLength(Type *T, uint64_t N){
  auto *A = llvm::dyn_cast<ArrayType>(T);
  if (!A || !A->getElementType()->isChar())
    return false;
  OrdinalBounds Bd = getOrdinalBounds(A->getIndexType());
  return static_cast<uint64_t>(Bd.Hi - Bd.Lo + 1) == N;
}


/// Checks the initial value of a typed constant of type T: a constant
/// expression (folded into Folded) or, for arrays and records, an
/// aggregate whose elements are checked recursively.
bool Checker::checkConstInit(Expr *Init, Type *T, ConstValue &Folded){
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
      // (1, 'a') for a tuple: its elements in turn, each then named.
      if (R->isTuple() && !Agg->isRecordForm()) {
        auto Fields = R->getAllFields();
        if (Elems.size() != Fields.size()) {
          Diags.error(Init->getBeginLoc(),
                      "initializer for " + quoted(T) + " must list " +
                          std::to_string(Fields.size()) +
                          " elements, separated by ','",
                      {Init->getRange()});
          return false;
        }
        for (size_t I = 0; I < Elems.size(); ++I) {
          Elems[I]->Field = Fields[I]->getName();
          Elems[I]->Field.Loc = Elems[I]->Value->getBeginLoc();
        }
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

  if (T == TGuidTy) // '{01234567-89AB-CDEF-0123-456789ABCDEF}'
    return checkGuidInit(Init, Folded);
  if (auto *RT = llvm::dyn_cast<RoutineType>(T))
    return checkRoutineConstInit(Init, RT);
  if (T->isDynArray()) { // only nil
    if (llvm::isa<NilExpr>(Init)) {
      check(Init);
      return true;
    }
    Diags.error(Init->getBeginLoc(),
                "typed constants of type " + quoted(T) +
                    " can only be initialized with nil",
                {Init->getRange()});
    return false;
  }
  auto C = evaluateConstant(Init, ConstInitWhat);
  if (!C)
    return false;
  bool CharArray = C->Value.K == ConstValue::String &&
                   isCharArrayOfLength(T, C->Value.Str.size());
  if (!CharArray && !(T->isSimple() || T->isShortString() || T->isAnsiString() ||
                      T->isStringType())) {
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
    if (!constantInRange(C->Value.Int, C->Ty, T)) {
      Diags.error(Init->getBeginLoc(),
                  "value " + formatValue(C->Ty, C->Value.Int) +
                      " is out of range for " + quoted(T),
                  {Init->getRange()});
      return false;
    }
  }
  checkWideToChar(T, Init);
  Folded = C->Value;
  if (C->Value.K == ConstValue::Ordinal && C->Ty->getBase()->isWideChar() &&
      T->getBase()->isChar())
    Folded.Int = wideToChar(Folded.Int);
  return true;
}


/// A typed constant (or initialized variable) of procedural type RT: nil,
/// a routine (@F) declared at program or unit level or a static method,
/// or a lambda that uses nothing of the routines around it and has no
/// object.
bool Checker::checkRoutineConstInit(Expr *Init, RoutineType *RT){
  auto Bad = [&](const llvm::Twine &Why) {
    Diags.error(Init->getBeginLoc(),
                llvm::Twine(ConstInitWhat) + " must be a constant: " + Why,
                {Init->getRange()});
    return false;
  };
  if (llvm::isa<NilExpr>(Init)) {
    check(Init);
    return true;
  }
  auto *A = llvm::dyn_cast<AddrOfExpr>(Init);
  if (!A) {
    Diags.error(Init->getBeginLoc(),
                "typed constants of type " + quoted(RT) +
                    " can only be initialized with nil, a routine or a "
                    "lambda",
                {Init->getRange()});
    return false;
  }
  Type *VT = checkValueFor(RT, Init);
  if (VT->isError())
    return false;
  if (!isAssignable(RT, VT)) {
    Diags.error(Init->getBeginLoc(),
                "cannot initialize " + quoted(RT) + " with a value of type " +
                    quoted(VT),
                {Init->getRange()});
    return false;
  }
  if (A->getMethod())
    return Bad("the object of a method pointer is not");
  if (RoutineDecl *L = A->getLambda()) {
    if (L->getCapture().isValid())
      return Bad("this lambda uses '" + L->getCapture().Spelling +
                 "' of the routine around it");
    if (L->isMethodLambda() && L->getLambdaSelf())
      return Bad("this lambda's object is Self");
  }
  return true;
}


/// A TGUID typed constant (or initialized variable) from a string
/// constant, as in Free Pascal: '{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}'
/// (hexadecimal digits) or '' (all zeros). Folded receives its 16 bytes
/// (Data1, Data2 and Data3 little-endian, then Data4).
bool Checker::checkGuidInit(Expr *Init, ConstValue &Folded){
  auto C = evaluateConstant(Init, ConstInitWhat);
  if (!C)
    return false;
  bool IsString = C->Value.K == ConstValue::String;
  bool IsChar = C->Value.K == ConstValue::Ordinal && C->Ty->getBase()->isChar();
  if (!IsString && !IsChar) {
    Diags.error(Init->getBeginLoc(),
                "cannot initialize " + quoted(TGuidTy) +
                    " with a value of type " + quoted(C->Ty),
                {Init->getRange()});
    return false;
  }
  std::string Bytes(16, '\0');
  if (IsString && !C->Value.Str.empty()) {
    llvm::StringRef S = C->Value.Str;
    auto Hex = [&](size_t Pos, size_t Digits, uint64_t &V) {
      V = 0;
      for (size_t I = Pos; I < Pos + Digits; ++I) {
        if (!llvm::isHexDigit(S[I]))
          return false;
        V = V << 4 | llvm::hexDigitValue(S[I]);
      }
      return true;
    };
    // Groups: position, digits, first byte, bytes, little-endian
    struct Group { size_t Pos, Digits, Byte; bool LE; };
    static const Group Groups[] = {{1, 8, 0, true},   {10, 4, 4, true},
                                   {15, 4, 6, true},  {20, 4, 8, false},
                                   {25, 12, 10, false}};
    bool OK = S.size() == 38 && S[0] == '{' && S[37] == '}' &&
              S[9] == '-' && S[14] == '-' && S[19] == '-' && S[24] == '-';
    for (const Group &G : Groups) {
      uint64_t V;
      if (!OK || !(OK = Hex(G.Pos, G.Digits, V)))
        break;
      size_t N = G.Digits / 2;
      for (size_t I = 0; I < N; ++I)
        Bytes[G.Byte + (G.LE ? I : N - 1 - I)] =
            static_cast<char>((V >> (8 * I)) & 0xff);
    }
    if (!OK) {
      Diags.error(Init->getBeginLoc(),
                  "improper GUID syntax: expected "
                  "'{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}' (hexadecimal "
                  "digits) or ''",
                  {Init->getRange()});
      return false;
    }
  } else if (IsChar) {
    Diags.error(Init->getBeginLoc(),
                "improper GUID syntax: expected "
                "'{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}' (hexadecimal "
                "digits) or ''",
                {Init->getRange()});
    return false;
  }
  Folded = ConstValue::string(Ctx.getStrings().intern(Bytes));
  return true;
}


/// Is E, a checked expression, a constant (nil, a set of constants, a
/// concatenation of string constants)?
bool Checker::isConstantDefault(Expr *E){
  if (llvm::isa<NilExpr>(E))
    return true;
  if (auto *Sum = llvm::dyn_cast<BinaryExpr>(E);
      Sum && Sum->getOp() == BinaryOp::Add && !Sum->getOperator() &&
      (E->getType()->isShortString() || E->getType()->isAnsiString()))
    return isConstantDefault(Sum->getLHS()) && isConstantDefault(Sum->getRHS());
  if (auto *S = llvm::dyn_cast<SetCtorExpr>(E)) {
    for (const SetElement &El : S->getElements())
      if (!fold(El.Lo) || (El.Hi && !fold(El.Hi)))
        return false;
    return true;
  }
  return fold(E).has_value();
}


/// Checks the default value of parameter P, of a resolved type: a
/// constant that can be passed to it.
void Checker::checkParamDefault(VarDecl *P){
  Expr *E = P->getDefault();
  if (!E || P->getType()->isError())
    return;
  Type *T = P->getType(), *AT = check(E);
  if (AT->isError())
    return;
  if (!isConstantDefault(E)) {
    Diags.error(E->getBeginLoc(), "default parameter value must be a constant",
                {E->getRange()});
    return;
  }
  if (!isAssignable(T, AT) && !isPCharLiteral(T, E)) {
    Diags.error(E->getBeginLoc(),
                "cannot pass a default value of type " + quoted(AT) +
                    " to parameter '" + P->getName().Spelling + "' of type " +
                    quoted(T),
                {E->getRange()});
    return;
  }
  if (auto C = fold(E); C && C->Value.K == ConstValue::Ordinal && T->isOrdinal()) {
    if (!constantInRange(C->Value.Int, C->Ty, T))
      Diags.error(E->getBeginLoc(),
                  "value " + formatValue(C->Ty, C->Value.Int) +
                      " is out of range for " + quoted(T),
                  {E->getRange()});
  }
}


/// The elements of the constant set S.
std::bitset<256> Checker::constantSetBits(SetCtorExpr *S){
  std::bitset<256> Bits;
  for (const SetElement &El : S->getElements()) {
    int64_t Lo = fold(El.Lo)->Value.Int;
    int64_t Hi = El.Hi ? fold(El.Hi)->Value.Int : Lo;
    for (int64_t V = std::max<int64_t>(Lo, 0); V <= std::min<int64_t>(Hi, 255); ++V)
      Bits.set(static_cast<size_t>(V));
  }
  return Bits;
}


/// Are the constant default values A and B of a parameter equal?
bool Checker::sameDefaultValue(Expr *A, Expr *B){
  if (llvm::isa<NilExpr>(A) || llvm::isa<NilExpr>(B))
    return llvm::isa<NilExpr>(A) && llvm::isa<NilExpr>(B);
  auto *SA = llvm::dyn_cast<SetCtorExpr>(A);
  auto *SB = llvm::dyn_cast<SetCtorExpr>(B);
  if (SA || SB)
    return SA && SB && constantSetBits(SA) == constantSetBits(SB);
  auto CA = fold(A), CB = fold(B);
  if (!CA || !CB)
    return false;
  const ConstValue &VA = CA->Value, &VB = CB->Value;
  if (VA.K != VB.K)
    return false;
  return VA.K == ConstValue::Ordinal  ? VA.Int == VB.Int
         : VA.K == ConstValue::String ? VA.Str == VB.Str
                                      : VA.RealVal == VB.RealVal;
}


/// Do the default values of Impl, the implementation of Heading (with
/// the same signature), fit? It may leave them out, or repeat them.
bool Checker::sameDefaults(RoutineDecl *Heading, RoutineDecl *Impl){
  for (size_t I = 0; I < Impl->getParams().size(); ++I) {
    VarDecl *P = Impl->getParams()[I];
    if (!P->getDefault())
      continue;
    Expr *HD = Heading->getParams()[I]->getDefault();
    if (!HD)
      return false;
    unsigned Errors = Diags.getNumErrors();
    checkParamDefault(P);
    if (Diags.getNumErrors() != Errors) // reported
      continue;
    if (!sameDefaultValue(HD, P->getDefault()))
      return false;
  }
  return true;
}


/// Resolves the pointer types since Mark whose targets are visible now
/// (declared in a class); the others stay pending.
void Checker::resolvePendingPointersFrom(size_t Mark){
  for (size_t I = Mark; I < PendingPointers.size();) {
    auto *TD = llvm::dyn_cast_or_null<TypeDecl>(
        Cur->lookup(PendingPointers[I].first->getTarget().Name));
    if (TD && TD->getType()) {
      PendingPointers[I].second->setPointee(TD->getType());
      PendingPointers.erase(PendingPointers.begin() + I);
    } else {
      ++I;
    }
  }
}


void Checker::resolvePendingPointers(){
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

