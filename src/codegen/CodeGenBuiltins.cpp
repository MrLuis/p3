// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- CodeGenBuiltins.cpp - AST -> LLVM IR ---------------------------------------===//

#include "CodeGenInternal.h"

using namespace p3;
using namespace p3::detail;


//===--------------------------------------------------------------------===//
// Short strings (Turbo/FPC): values are pointers to length-prefixed bytes
//===--------------------------------------------------------------------===//

llvm::GlobalVariable *CodeGenModule::getPStrConstant(llvm::StringRef S){
  std::string Bytes(1, static_cast<char>(std::min<size_t>(S.size(), 255)));
  Bytes += S.take_front(255).str();
  return getStringConstant(Bytes);
}


/// A string constant as an ansistring: static characters after a header
/// {count -1, length}; null when empty.
llvm::Constant *CodeGenModule::getAnsiConstant(llvm::StringRef S){
  if (S.empty())
    return llvm::ConstantPointerNull::get(B.getPtrTy());
  llvm::GlobalVariable *&G = AnsiConstants[S];
  if (!G) {
    std::string Bytes(8, '\xff');
    uint64_t N = S.size();
    for (int I = 0; I < 8; ++I)
      Bytes.push_back(static_cast<char>(N >> (8 * I)));
    Bytes += S.str();
    Bytes.push_back('\0');
    auto *Init = llvm::ConstantDataArray::getString(M.getContext(), Bytes,
                                                    /*AddNull=*/false);
    G = new llvm::GlobalVariable(M, Init->getType(), true,
                                 llvm::GlobalValue::PrivateLinkage, Init,
                                 ".astr");
    G->setAlignment(llvm::Align(8));
  }
  return llvm::ConstantExpr::getInBoundsGetElementPtr(B.getInt8Ty(), G,
                                                      B.getInt64(16));
}


/// A call of an ansistring routine of the runtime on Args.
llvm::Value *CodeGenModule::astrCall(llvm::StringRef Name, llvm::Type *Ret,
                      llvm::ArrayRef<llvm::Value *> Args){
  llvm::SmallVector<llvm::Type *, 4> Params;
  for (llvm::Value *A : Args)
    Params.push_back(A->getType());
  return B.CreateCall(classRuntime(Name, Ret, Params), Args);
}


/// Any string-like expression, or a pchar, as an ansistring (borrowed: a
/// new one is held until the statement ends).
llvm::Value *CodeGenModule::emitAnsi(Expr *E){
  Type *T = E->getType();
  if (T->isAnsiString())
    return visitExpr(E);
  if (auto *SL = llvm::dyn_cast<StringLiteralExpr>(E))
    return getAnsiConstant(SL->getValue());
  if (auto *N = llvm::dyn_cast<NameRefExpr>(unwrapConst(E)))
    if (auto *C = llvm::dyn_cast<ConstDecl>(N->getDecl());
        C && C->getValue().K == ConstValue::String)
      return getAnsiConstant(C->getValue().Str);
  bool NoHold = NoHoldAnsiResult;
  NoHoldAnsiResult = false;
  llvm::Type *Ptr = B.getPtrTy();
  if (T->isCharLike())
    return takeOrHoldAnsi(
        astrCall("__pas_astr_from_char", Ptr, {emitCharValue(E)}), NoHold);
  if (T->isPointer())
    return takeOrHoldAnsi(
        astrCall("__pas_astr_from_cstr", Ptr, {visitExpr(E)}), NoHold);
  return takeOrHoldAnsi(astrCall("__pas_astr_from_pstr", Ptr, {emitPStr(E)}),
                        NoHold);
}


bool CodeGenModule::isMethodPointerType(Type *T){
  auto *RT = llvm::dyn_cast<RoutineType>(T);
  return RT && RT->isOfObject();
}


/// slice(a, n): the first n elements of a, as an open array.
llvm::Value *CodeGenModule::emitSlice(CallExpr *C){
  Expr *A = C->getArgs()[0], *NE = C->getArgs()[1];
  llvm::Value *Data = emitAddress(A);
  llvm::Value *Count;
  if (auto *AT = llvm::dyn_cast<ArrayType>(A->getType())) {
    OrdinalBounds Bd = getOrdinalBounds(AT->getIndexType());
    Count = B.getInt64(Bd.Hi - Bd.Lo + 1);
  } else {
    Count = openCount(openDescriptor(A));
  }
  llvm::Value *N = toI64(visitExpr(NE), NE->getType());
  if (rangeChecks(NE->getBeginLoc()))
    emitRuntimeCheck(B.CreateICmpUGT(N, Count), "slice length out of bounds",
                     NE->getBeginLoc());
  return newOpenDescriptor(Data, N);
}


/// The descriptor {data, count} of the open array parameter E.
llvm::Value *CodeGenModule::openDescriptor(Expr *E){
  auto *V = llvm::cast<VarDecl>(llvm::cast<NameRefExpr>(E)->getDecl());
  return B.CreateLoad(B.getPtrTy(), varAddress(V), "open");
}


llvm::Value *CodeGenModule::openCount(llvm::Value *Desc){
  return B.CreateLoad(B.getInt64Ty(),
                      B.CreateConstInBoundsGEP1_64(B.getInt64Ty(), Desc, 1),
                      "count");
}


llvm::Value *CodeGenModule::newOpenDescriptor(llvm::Value *Data, llvm::Value *Count){
  llvm::Value *Desc =
      createTemp(llvm::ArrayType::get(B.getInt64Ty(), 2), "open");
  B.CreateStore(Data, Desc);
  B.CreateStore(Count, B.CreateConstInBoundsGEP1_64(B.getInt64Ty(), Desc, 1));
  return Desc;
}


/// The open array passed for Arg: another one, a slice built in a
/// temporary, a call's own result, an array, or a string constant.
llvm::Value *CodeGenModule::emitOpenArrayArg(Expr *Arg, OpenArrayType *OA){
  Type *T = Arg->getType();
  if (auto *S = llvm::dyn_cast<SetCtorExpr>(Arg)) // typed as the parameter
    return emitOpenArrayCtor(S, OA);
  if (auto *C = llvm::dyn_cast_or_null<CallExpr>(Arg);
      C && C->getDecl() && llvm::isa<BuiltinDecl>(C->getDecl()) &&
      llvm::cast<BuiltinDecl>(C->getDecl())->getBuiltinKind() ==
          BuiltinKind::Slice)
    return emitSlice(C);
  if (auto *C = llvm::dyn_cast_or_null<CallExpr>(Arg)) {
    // a call's own result (a Copy, or a routine's array result; an
    // argument the sema typed as the parameter included): the count is
    // the result's length
    llvm::Value *Data = visitExpr(C);
    return newOpenDescriptor(Data, dynLength(Data));
  }
  if (T == OA)
    return openDescriptor(Arg);
  if (auto *A = llvm::dyn_cast<ArrayType>(T)) {
    OrdinalBounds Bd = getOrdinalBounds(A->getIndexType());
    llvm::Value *Data = emitAddress(Arg);
    return newOpenDescriptor(Data, B.getInt64(Bd.Hi - Bd.Lo + 1));
  }
  if (T->isDynArray()) {
    llvm::Value *Data = visitExpr(Arg);
    return newOpenDescriptor(Data, dynLength(Data));
  }
  if (T->isStringLiteral())
    return newOpenDescriptor(emitAddress(Arg), B.getInt64(T->getStringLength()));
  llvm::Value *C = createTemp(B.getInt8Ty(), "chr"); // a char constant
  B.CreateStore(visitExpr(Arg), C);
  return newOpenDescriptor(C, B.getInt64(1));
}


llvm::Value *CodeGenModule::emitOpenArrayCtor(SetCtorExpr *S, OpenArrayType *OA){
  Type *Elem = OA->getElementType();
  auto Els = S->getElements();
  if (Els.empty())
    return newOpenDescriptor(llvm::ConstantPointerNull::get(B.getPtrTy()),
                             B.getInt64(0));
  llvm::Type *ArrTy = llvm::ArrayType::get(memType(Elem), Els.size());
  llvm::Value *Arr = createTemp(ArrTy, "elems");
  for (size_t I = 0; I < Els.size(); ++I) {
    Expr *E = Els[I].Lo;
    llvm::Value *Slot =
        B.CreateInBoundsGEP(ArrTy, Arr, {B.getInt64(0), B.getInt64(I)});
    // Borrowed: the callee counts what it keeps.
    if (auto *SS = llvm::dyn_cast<ShortStringType>(Elem))
      emitStringAssign(Slot, SS, E);
    else if (Elem->isAnsiString())
      B.CreateStore(emitAnsi(E), Slot);
    else if (Elem->isStructured())
      copyAggregate(Slot, emitAddress(E), Elem);
    else if (llvm::Constant *C = cStringFor(Elem, E))
      B.CreateStore(C, Slot);
    else
      storeScalar(Slot, convertForAssign(visitExpr(E), E->getType(), Elem,
                                         E->getBeginLoc()),
                  Elem);
  }
  return newOpenDescriptor(Arr, B.getInt64(Els.size()));
}


/// A value parameter's own copy of the open array in frame slot Slot.
void CodeGenModule::copyOpenArray(llvm::Value *Slot, OpenArrayType *OA){
  llvm::Type *ET = memType(OA->getElementType());
  llvm::Value *Desc = B.CreateLoad(B.getPtrTy(), Slot, "open");
  llvm::Value *Data = B.CreateLoad(B.getPtrTy(), Desc, "data");
  llvm::Value *N = openCount(Desc);
  llvm::Value *Copy = B.CreateAlloca(ET, N, "copy");
  B.CreateMemCpy(Copy, llvm::MaybeAlign(1), Data, llvm::MaybeAlign(1),
                 B.CreateMul(N, B.getInt64(sizeOf(OA->getElementType()))));
  B.CreateStore(newOpenDescriptor(Copy, N), Slot);
}


/// Runs Fn on the address of each element of the open array in slot Slot.
void CodeGenModule::forEachOpenElement(llvm::Value *Slot, OpenArrayType *OA,
                        llvm::function_ref<void(llvm::Value *)> Fn,
                        llvm::StringRef What){
  llvm::Value *Desc = B.CreateLoad(B.getPtrTy(), Slot, "open");
  llvm::Value *Data = B.CreateLoad(B.getPtrTy(), Desc, "data");
  llvm::Value *N = openCount(Desc);
  llvm::BasicBlock *Pre = B.GetInsertBlock();
  auto *Loop = newBlock(What + ".loop");
  auto *Done = newBlock(What + ".done");
  B.CreateCondBr(B.CreateICmpEQ(N, B.getInt64(0)), Done, Loop);
  B.SetInsertPoint(Loop);
  llvm::PHINode *I = B.CreatePHI(B.getInt64Ty(), 2, "i");
  I->addIncoming(B.getInt64(0), Pre);
  Fn(B.CreateInBoundsGEP(memType(OA->getElementType()), Data, I));
  llvm::Value *Next = B.CreateAdd(I, B.getInt64(1));
  I->addIncoming(Next, B.GetInsertBlock());
  B.CreateCondBr(B.CreateICmpEQ(Next, N), Done, Loop);
  B.SetInsertPoint(Done);
}


/// The array of const passed for Arg: another array of const, or [...]
/// built in a temporary: a header slot {count}, then {VType, value}.
llvm::Value *CodeGenModule::emitConstArray(Expr *Arg){
  auto *S = llvm::dyn_cast<SetCtorExpr>(Arg);
  if (!S)
    return visitExpr(Arg);
  auto Els = S->getElements();
  llvm::Value *Tmp = createTemp(
      llvm::ArrayType::get(B.getInt64Ty(), 2 * (Els.size() + 1)), "args");
  B.CreateStore(B.getInt64(Els.size()), Tmp);
  uint64_t Slot = 2;
  for (const SetElement &El : Els) {
    int64_t Kind;
    llvm::Value *V = emitConstArrayValue(El.Lo, Kind);
    B.CreateStore(B.getInt64(Kind),
                  B.CreateConstInBoundsGEP1_64(B.getInt64Ty(), Tmp, Slot));
    B.CreateStore(V, B.CreateConstInBoundsGEP1_64(B.getInt64Ty(), Tmp, Slot + 1));
    Slot += 2;
  }
  return Tmp;
}


/// An element's value as stored in its TVarRec (Kind: its VType).
llvm::Value *CodeGenModule::emitConstArrayValue(Expr *E, int64_t &Kind){
  Type *T = E->getType();
  if (T->isStringLiteral() || T->isAnsiString()) {
    Kind = 11; // vtAnsiString
    return emitAnsi(E);
  }
  if (T->isShortString() || T->isStringType()) {
    Kind = 4; // vtString
    return emitPStr(E);
  }
  if (T->isReal() || T->isIntegerLike()) {
    llvm::Value *V = visitExpr(E);
    if (T->isIntegerLike() && intBits(T) != 64) {
      Kind = 0; // vtInteger
      return toI64(V, T);
    }
    Kind = T->isReal() ? 3 : 16; // vtExtended, vtInt64: by reference
    llvm::Value *Tmp = createTemp(V->getType(), "arg");
    B.CreateStore(V, Tmp);
    return Tmp;
  }
  if (T->getBase()->isChar() || T->getBase()->isBoolean()) {
    Kind = T->getBase()->isChar() ? 2 : 1; // vtChar, vtBoolean
    return B.CreateZExt(visitExpr(E), B.getInt64Ty());
  }
  Kind = T->isClass() ? 7 : T->isClassRef() ? 8 : 5; // vtObject, vtClass, vtPointer
  if (auto *P = llvm::dyn_cast<PointerType>(T);
      P && !P->isUntyped() && P->getPointee() && P->getPointee()->isChar())
    Kind = 6; // vtPChar
  return visitExpr(E);
}


llvm::Value *CodeGenModule::newStringTemp(unsigned MaxLen){
  llvm::Value *Tmp =
      createTemp(llvm::ArrayType::get(B.getInt8Ty(), MaxLen + 1), "str");
  B.CreateLifetimeStart(Tmp);
  StringTemps.push_back({Tmp, MaxLen + 1});
  return Tmp;
}


/// Ends the lifetime of the short-string temporaries made since Mark.
void CodeGenModule::releaseStringTemps(size_t Mark){
  for (size_t I = Mark; I < StringTemps.size(); ++I)
    B.CreateLifetimeEnd(StringTemps[I].first);
  StringTemps.resize(Mark);
}


/// Any string-like expression as a pointer to a length-prefixed string.
llvm::Value *CodeGenModule::emitPStr(Expr *E){
  Type *T = E->getType();
  if (T->isShortString())
    return visitExpr(E); // designators yield their address, results a temp
  if (T->isAnsiString()) {
    llvm::Value *S = visitExpr(E);
    llvm::Value *Tmp = newStringTemp();
    astrCall("__pas_astr_to_pstr", B.getVoidTy(), {Tmp, B.getInt32(255), S});
    return Tmp;
  }
  if (auto *SL = llvm::dyn_cast<StringLiteralExpr>(E))
    return getPStrConstant(SL->getValue());
  if (auto *N = llvm::dyn_cast<NameRefExpr>(unwrapConst(E)))
    if (auto *C = llvm::dyn_cast<ConstDecl>(N->getDecl());
        C && C->getValue().K == ConstValue::String)
      return getPStrConstant(C->getValue().Str);
  if (T->isCharLike()) {
    llvm::Value *Tmp = newStringTemp(1);
    B.CreateStore(B.getInt8(1), Tmp);
    llvm::Value *C = emitCharValue(E);
    B.CreateStore(C, B.CreateConstGEP1_64(B.getInt8Ty(), Tmp, 1));
    return Tmp;
  }
  // packed array [1..n] of char
  uint64_t N = T->getStringLength();
  llvm::Value *Tmp = newStringTemp(static_cast<unsigned>(std::min<uint64_t>(N, 255)));
  B.CreateStore(B.getInt8(static_cast<uint8_t>(std::min<uint64_t>(N, 255))), Tmp);
  llvm::Value *Dst = B.CreateConstGEP1_64(B.getInt8Ty(), Tmp, 1);
  llvm::Value *Src = emitAddress(E);
  B.CreateMemCpy(Dst, llvm::MaybeAlign(1), Src, llvm::MaybeAlign(1),
                 std::min<uint64_t>(N, 255));
  return Tmp;
}


void CodeGenModule::emitStringAssign(llvm::Value *Dst, ShortStringType *T, Expr *Value){
  B.CreateCall(RT.StrAssign,
               {Dst, B.getInt32(T->getMaxLength()), emitPStr(Value)});
}


llvm::Value *CodeGenModule::emitStringFunction(CallExpr *E, BuiltinKind K){
  auto Args = E->getArgs();
  switch (K) {
  case BuiltinKind::Length: {
    Type *T = Args[0]->getType();
    if (T->isOpenArray())
      return B.CreateTrunc(openCount(openDescriptor(Args[0])), B.getInt32Ty());
    if (T->isDynArray())
      return B.CreateTrunc(dynLength(visitExpr(Args[0])), B.getInt32Ty());
    if (T->isArrayOfConst())
      return B.CreateTrunc(B.CreateLoad(B.getInt64Ty(), visitExpr(Args[0])),
                           B.getInt32Ty());
    if (T->isAnsiString()) {
      llvm::Value *Len =
          isSliceExpr(Args[0])
              ? emitSliceExpr(Args[0]).Len
              : astrCall("__pas_astr_length", B.getInt32Ty(),
                         {visitExpr(Args[0])});
      noteRange(Len, {true, 0, INT32_MAX});
      return Len;
    }
    if (T->isShortString())
      return B.CreateZExt(B.CreateLoad(B.getInt8Ty(), emitPStr(Args[0])),
                          B.getInt32Ty(), "length");
    if (T->isCharLike()) {
      visitExpr(Args[0]);
      return B.getInt32(1);
    }
    return B.getInt32(static_cast<uint32_t>(T->getStringLength()));
  }
  case BuiltinKind::UpCase: {
    llvm::Value *C = visitExpr(Args[0]); // a char or a widechar
    auto Ch = [&](uint64_t X) { return llvm::ConstantInt::get(C->getType(), X); };
    llvm::Value *GeA = B.CreateICmpUGE(C, Ch('a'));
    llvm::Value *LeZ = B.CreateICmpULE(C, Ch('z'));
    llvm::Value *IsLower = B.CreateAnd(GeA, LeZ);
    return B.CreateSelect(IsLower, B.CreateSub(C, Ch(32)), C, "upcase");
  }
  case BuiltinKind::Pos:
    if (Args[0]->getType()->isAnsiString() || Args[1]->getType()->isAnsiString()) {
      llvm::Value *Sub = emitAnsi(Args[0]);
      llvm::Value *S = emitAnsi(Args[1]);
      return astrCall("__pas_astr_pos", B.getInt32Ty(), {Sub, S});
    }
    return B.CreateCall(RT.StrPos, {emitPStr(Args[0]), emitPStr(Args[1])});
  case BuiltinKind::Copy: {
    if (Args[0]->getType()->isDynArray()) {
      llvm::Value *A = visitExpr(Args[0]);
      llvm::Value *Start = Args.size() > 1
                               ? toI64(visitExpr(Args[1]), Args[1]->getType())
                               : B.getInt64(0);
      llvm::Value *Count = Args.size() > 2
                               ? toI64(visitExpr(Args[2]), Args[2]->getType())
                               : B.getInt64(INT64_MAX);
      return holdResult(astrCall("__pas_dyn_copy", B.getPtrTy(), {A, Start, Count}),
                        Args[0]->getType());
    }
    if (Args[0]->getType()->isAnsiString()) {
      bool NoHold = NoHoldAnsiResult;
      NoHoldAnsiResult = false;
      llvm::Value *S = visitExpr(Args[0]);
      llvm::Value *Index = emitI32(Args[1]);
      llvm::Value *Count = emitI32(Args[2]);
      return takeOrHoldAnsi(
          astrCall("__pas_astr_copy", B.getPtrTy(), {S, Index, Count}), NoHold);
    }
    llvm::Value *Tmp = newStringTemp();
    B.CreateCall(RT.StrCopy, {Tmp, emitPStr(Args[0]), emitI32(Args[1]),
                              emitI32(Args[2])});
    return Tmp;
  }
  default: { // Concat
    if (E->getType()->isAnsiString()) {
      // Concat(a, b, c): three joined in one allocation, then pairwise.
      bool NoHold = NoHoldAnsiResult;
      NoHoldAnsiResult = false;
      llvm::Value *R = nullptr;
      size_t From = 1;
      if (Args.size() >= 3 && Args[0]->getType()->isAnsiString() &&
          (Args[1]->getType()->isAnsiString() ||
           Args[1]->getType()->isCharLike()) &&
          Args[2]->getType()->isAnsiString()) {
        llvm::Value *A = emitAnsi(Args[0]);
        llvm::Value *M = nullptr, *N = nullptr;
        if (Args[1]->getType()->isAnsiString()) {
          M = emitAnsi(Args[1]);
          N = B.CreateZExt(
              astrCall("__pas_astr_length", B.getInt32Ty(), {M}),
              B.getInt64Ty());
        } else {
          M = emitCharPiece(Args[1]);
          N = B.getInt64(1);
        }
        llvm::Value *C = emitAnsi(Args[2]);
        R = takeOrHoldAnsi(
            astrCall("__pas_astr_join", B.getPtrTy(), {A, M, N, C}),
            NoHold && Args.size() == 3);
        From = 3;
      } else
        R = emitAnsi(Args[0]);
      for (size_t I = From; I < Args.size(); ++I) {
        llvm::Value *S = emitAnsi(Args[I]);
        R = takeOrHoldAnsi(astrCall("__pas_astr_concat", B.getPtrTy(), {R, S}),
                           NoHold && I + 1 == Args.size());
      }
      return R;
    }
    llvm::Value *Tmp = newStringTemp();
    B.CreateCall(RT.StrAssign, {Tmp, B.getInt32(255), emitPStr(Args[0])});
    for (Expr *A : Args.drop_front())
      B.CreateCall(RT.StrConcat, {Tmp, Tmp, emitPStr(A)});
    return Tmp;
  }
  }
}


//===--------------------------------------------------------------------===//
// Sets
//===--------------------------------------------------------------------===//

/// The members Lo..Hi (within 0..255) as a set value of Bits bits.
llvm::Constant *CodeGenModule::constRangeMask(int64_t Lo, int64_t Hi, unsigned Bits){
  llvm::APInt M = llvm::APInt::getBitsSet(256, static_cast<unsigned>(Lo),
                                          static_cast<unsigned>(Hi) + 1);
  return llvm::ConstantInt::get(B.getIntNTy(Bits), M.trunc(Bits));
}


/// Ordinal value -> bit index of a set of Bits bits, checking 0..255
/// under range checks. (A set constructor's representation holds every
/// value of its elements' type that is in 0..255.)
llvm::Value *CodeGenModule::setIndex(llvm::Value *V, Type *T, SourceLocation L,
                      unsigned Bits){
  llvm::Value *X = toI64(V, T);
  OrdinalBounds Bd = getOrdinalBounds(T);
  if (rangeChecks(L) && (Bd.Lo < 0 || Bd.Hi > 255))
    emitRuntimeCheck(B.CreateICmpUGT(X, B.getInt64(255)),
                     "set element out of range 0..255", L);
  llvm::Value *Masked = B.CreateAnd(X, B.getInt64(Bits - 1));
  return B.CreateZExtOrTrunc(Masked, B.getIntNTy(Bits));
}


llvm::Value *CodeGenModule::emitSetCtor(SetCtorExpr *E){
  unsigned Bits = setBits(E->getType());
  llvm::IntegerType *Ty = B.getIntNTy(Bits);
  llvm::Value *Result = llvm::ConstantInt::get(Ty, 0);
  llvm::Value *One = llvm::ConstantInt::get(Ty, 1);
  llvm::Value *AllOnes = llvm::ConstantInt::getAllOnesValue(Ty);
  for (const SetElement &El : E->getElements()) {
    Type *LT = El.Lo->getType();
    llvm::Value *Lo = visitExpr(El.Lo);
    if (!El.Hi) {
      Result = B.CreateOr(
          Result, B.CreateShl(One, setIndex(Lo, LT, El.Lo->getBeginLoc(), Bits)));
      continue;
    }
    llvm::Value *Hi = visitExpr(El.Hi);
    Type *HT = El.Hi->getType();
    // Bits Lo..Hi, or nothing if Lo > Hi.
    llvm::Value *Empty = isSignedOrdinal(LT) ? B.CreateICmpSGT(Lo, Hi)
                                             : B.CreateICmpUGT(Lo, Hi);
    llvm::Value *LoI = setIndex(Lo, LT, El.Lo->getBeginLoc(), Bits);
    llvm::Value *HiI = setIndex(Hi, HT, El.Hi->getBeginLoc(), Bits);
    llvm::Value *IsTop =
        B.CreateICmpEQ(HiI, llvm::ConstantInt::get(Ty, Bits - 1));
    llvm::Value *Below = B.CreateSub(B.CreateShl(One, B.CreateAdd(HiI, One)), One);
    llvm::Value *HiMask = B.CreateSelect(IsTop, AllOnes, Below);
    llvm::Value *LoMask = B.CreateNot(B.CreateSub(B.CreateShl(One, LoI), One));
    llvm::Value *Range = B.CreateSelect(
        Empty, llvm::ConstantInt::get(Ty, 0), B.CreateAnd(HiMask, LoMask));
    Result = B.CreateOr(Result, Range);
  }
  return Result;
}


/// x in [a, b..c, ...] without building the set (optimization.md, item
/// 6). The members given by constants are tested together: one value or
/// one range by comparisons; values at most 63 apart by a bit of a
/// 64-bit mask; up to four ranges by comparisons; else the bit of the
/// 256-bit set. Each other element is a comparison (a range: two). All
/// elements are evaluated, in order, after x.
llvm::Value *CodeGenModule::emitInCtor(llvm::Value *X, SetCtorExpr *S){
  llvm::APInt Members(256, 0);
  llvm::Value *Result = nullptr;
  auto Or = [&](llvm::Value *V) {
    Result = Result ? B.CreateOr(Result, V, "in") : V;
  };
  // An element as an i64, checked like a set member (0..255).
  auto Element = [&](Expr *El) {
    Type *T = El->getType();
    llvm::Value *V = toI64(visitExpr(El), T);
    OrdinalBounds Bd = getOrdinalBounds(T);
    if (!llvm::isa<llvm::ConstantInt>(V) && rangeChecks(El->getBeginLoc()) &&
        (Bd.Lo < 0 || Bd.Hi > 255))
      emitRuntimeCheck(B.CreateICmpUGT(V, B.getInt64(255)),
                       "set element out of range 0..255", El->getBeginLoc());
    return V;
  };
  for (const SetElement &El : S->getElements()) {
    llvm::Value *Lo = Element(El.Lo);
    llvm::Value *Hi = El.Hi ? Element(El.Hi) : Lo;
    auto *CLo = llvm::dyn_cast<llvm::ConstantInt>(Lo);
    auto *CHi = llvm::dyn_cast<llvm::ConstantInt>(Hi);
    if (CLo && CHi) {
      int64_t L = std::max<int64_t>(CLo->getSExtValue(), 0);
      int64_t H = std::min<int64_t>(CHi->getSExtValue(), 255);
      if (L <= H)
        Members.setBits(static_cast<unsigned>(L), static_cast<unsigned>(H) + 1);
      continue;
    }
    if (!El.Hi) {
      Or(B.CreateICmpEQ(X, Lo, "in"));
      continue;
    }
    llvm::Value *AtLeast = B.CreateICmpSGE(X, Lo);
    llvm::Value *AtMost = B.CreateICmpSLE(X, Hi);
    Or(B.CreateAnd(AtLeast, AtMost, "in"));
  }
  if (!Members.isZero()) {
    // The constant members: their ranges, lowest first.
    llvm::SmallVector<std::pair<int64_t, int64_t>, 8> Ranges;
    for (unsigned I = 0; I < 256; ++I) {
      if (!Members[I])
        continue;
      unsigned J = I;
      while (J + 1 < 256 && Members[J + 1])
        ++J;
      Ranges.push_back({I, J});
      I = J;
    }
    int64_t Min = Ranges.front().first, Max = Ranges.back().second;
    auto InRange = [&](int64_t L, int64_t H) -> llvm::Value * {
      if (L == H)
        return B.CreateICmpEQ(X, B.getInt64(L), "in");
      llvm::Value *AtLeast = B.CreateICmpSGE(X, B.getInt64(L));
      llvm::Value *AtMost = B.CreateICmpSLE(X, B.getInt64(H));
      return B.CreateAnd(AtLeast, AtMost, "in");
    };
    if (Ranges.size() == 1) {
      Or(InRange(Min, Max));
    } else if (Max - Min < 64) {
      llvm::Value *Idx = B.CreateSub(X, B.getInt64(Min));
      llvm::Value *Inside = B.CreateICmpULE(Idx, B.getInt64(Max - Min));
      uint64_t Mask = Members.lshr(static_cast<unsigned>(Min)).getLoBits(64)
                          .getZExtValue();
      llvm::Value *Bit = B.CreateTrunc(
          B.CreateLShr(B.getInt64(Mask), B.CreateAnd(Idx, B.getInt64(63))),
          B.getInt1Ty());
      Or(B.CreateAnd(Inside, Bit, "in"));
    } else if (Ranges.size() <= 4) {
      for (auto [L, H] : Ranges)
        Or(InRange(L, H));
    } else {
      llvm::Value *Inside = B.CreateICmpULE(X, B.getInt64(255));
      llvm::Value *Idx =
          B.CreateZExt(B.CreateAnd(X, B.getInt64(255)), B.getIntNTy(256));
      llvm::Value *Bit = B.CreateTrunc(
          B.CreateLShr(llvm::ConstantInt::get(B.getIntNTy(256), Members), Idx),
          B.getInt1Ty());
      Or(B.CreateAnd(Inside, Bit, "in"));
    }
  }
  if (!Result)
    return B.getFalse(); // x in []
  return Result;
}


/// x in s  ==  x is within s's representation and bit x of s is set
llvm::Value *CodeGenModule::emitIn(BinaryExpr *E){
  Type *XT = E->getLHS()->getType();
  llvm::Value *X = toI64(visitExpr(E->getLHS()), XT);
  if (auto *S = llvm::dyn_cast<SetCtorExpr>(E->getRHS()))
    return emitInCtor(X, S);
  llvm::Value *S = visitExpr(E->getRHS());
  unsigned Bits = setBits(E->getRHS()->getType());
  llvm::Value *InRange = B.CreateICmpULE(X, B.getInt64(Bits - 1));
  llvm::Value *Masked = B.CreateAnd(X, B.getInt64(Bits - 1));
  llvm::Value *Idx = B.CreateZExtOrTrunc(Masked, S->getType());
  llvm::Value *Bit = B.CreateTrunc(B.CreateLShr(S, Idx), B.getInt1Ty());
  return B.CreateAnd(InRange, Bit, "in");
}


/// (Name: R's function's; External: R is exported or imported, and so
/// are its task's functions.)
void CodeGenModule::declareAsync(RoutineDecl *R, const std::string &Name, bool External){
  auto Linkage = External ? llvm::Function::ExternalLinkage
                          : llvm::Function::InternalLinkage;
  llvm::SmallVector<llvm::Type *, 8> Slots{B.getPtrTy()}; // the link
  if (R->hasSelf())
    Slots.push_back(B.getPtrTy());
  bool Counted = false;
  for (VarDecl *P : R->getParams()) {
    Slots.push_back(memType(P->getType()));
    Counted |= isManaged(P->getType());
  }
  AsyncParts A;
  A.ArgsTy = llvm::StructType::create(M.getContext(), Slots,
                                      "args." + Name.substr(4));
  A.Entry = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false), Linkage,
      Name + (R->isIterator() ? ".gen" : ".task"), M);
  A.Entry->getArg(0)->setName(R->isIterator() ? "gen" : "task");
  A.Fini = nullptr;
  if (Counted) {
    A.Fini = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false), Linkage,
        Name + (R->isIterator() ? ".genfini" : ".taskfini"), M);
    A.Fini->getArg(0)->setName("args");
  }
  Asyncs[R] = A;
}


void CodeGenModule::emitAsyncParts(RoutineDecl *R){
  AsyncParts &A = Asyncs[R];
  llvm::PointerType *Ptr = B.getPtrTy();
  // R.task(task): R(args...), its result into the task's
  CurFn = A.Entry;
  AddrAligns.clear();
  CurRoutine = nullptr;
  CurFrame = nullptr;
  CurPin = nullptr;
  PinTemps = true;
  CurSP = nullptr;
  B.SetCurrentDebugLocation(llvm::DebugLoc());
  B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", A.Entry));
  llvm::Value *Task = A.Entry->getArg(0);
  llvm::Value *Args = B.CreateCall(
      classRuntime(R->isIterator() ? "__pas_gen_args" : "__pas_task_args", Ptr, {Ptr}),
      {Task}, "args");
  llvm::SmallVector<llvm::Value *, 8> Values;
  Values.push_back(B.CreateLoad(Ptr, B.CreateStructGEP(A.ArgsTy, Args, 0), "link"));
  llvm::Value *ResultSlot = nullptr;
  if (R->isFunction())
    ResultSlot = B.CreateCall(classRuntime("__pas_task_result", Ptr, {Ptr}),
                              {Task}, "result");
  bool ByPointer = R->isFunction() && returnsByPointer(R->getResultType());
  unsigned ResultArgNo = Values.size();
  if (ByPointer)
    Values.push_back(ResultSlot);
  if (R->hasSelf())
    Values.push_back(B.CreateLoad(Ptr, B.CreateStructGEP(A.ArgsTy, Args, 1), "self"));
  unsigned Slot = firstParamSlot(R);
  for (VarDecl *P : R->getParams()) {
    llvm::Value *Addr = B.CreateStructGEP(A.ArgsTy, Args, Slot++);
    Type *T = P->getType();
    if (T->isStructured()) // (the callee copies a value parameter)
      Values.push_back(Addr);
    else
      Values.push_back(loadScalar(Addr, T, P->getName().Name));
  }
  auto *Call = B.CreateCall(Functions[R], Values);
  if (ByPointer)
    addResultCallAttrs(Call, ResultArgNo, R->getResultType());
  llvm::Value *Result = Call;
  if (R->isFunction() && !ByPointer) {
    Type *T = R->getResultType();
    if (T->isBoolean() || T->isOrdinal() || T->isReal())
      storeScalar(ResultSlot, Result, T);
    else // (a counted value moves into the task)
      B.CreateStore(Result, ResultSlot);
  }
  B.CreateRetVoid();
  if (!A.Fini)
    return;
  // R.taskfini(args): the counted arguments released
  CurFn = A.Fini;
  AddrAligns.clear();
  B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", A.Fini));
  Slot = firstParamSlot(R);
  for (VarDecl *P : R->getParams()) {
    llvm::Value *Addr = B.CreateStructGEP(A.ArgsTy, A.Fini->getArg(0), Slot++);
    if (isManaged(P->getType()))
      finalizeValue(Addr, P->getType());
  }
  B.CreateRetVoid();
}


/// A call of async routine R (link, Self: Self's value or null): its
/// task, made with copies of the arguments and started (here: it runs
/// until it waits or ends).
llvm::Value *CodeGenModule::emitAsyncCall(RoutineDecl *R, llvm::Value *Link, llvm::Value *Self,
                           llvm::ArrayRef<Expr *> Args){
  StartMode Mode = NextStart;
  NextStart = StartMode::Here;
  AsyncParts &A = Asyncs[R];
  llvm::PointerType *Ptr = B.getPtrTy();
  llvm::Constant *Info = R->isFunction()
                             ? typeInfoOf(R->getResultType())
                             : llvm::ConstantPointerNull::get(Ptr);
  llvm::Constant *Fini = A.Fini ? static_cast<llvm::Constant *>(A.Fini)
                                : llvm::ConstantPointerNull::get(Ptr);
  llvm::Value *Task = B.CreateCall(
      classRuntime("__pas_task_new", Ptr, {Ptr, B.getInt64Ty(), Ptr, Ptr}),
      {Info, B.getInt64(DL.getTypeAllocSize(A.ArgsTy)), A.Entry, Fini}, "task");
  llvm::Value *ArgsAddr =
      B.CreateCall(classRuntime("__pas_task_args", Ptr, {Ptr}), {Task}, "args");
  B.CreateStore(Link ? Link : llvm::ConstantPointerNull::get(Ptr),
                B.CreateStructGEP(A.ArgsTy, ArgsAddr, 0));
  if (R->hasSelf())
    B.CreateStore(Self, B.CreateStructGEP(A.ArgsTy, ArgsAddr, 1));
  unsigned Slot = firstParamSlot(R);
  auto Params = R->getParams();
  for (size_t I = 0; I < Params.size(); ++I) {
    Expr *Arg = I < Args.size() ? Args[I] : Params[I]->getDefault();
    storeExpr(B.CreateStructGEP(A.ArgsTy, ArgsAddr, Slot++), Params[I]->getType(),
              Arg);
  }
  const char *Start = Mode == StartMode::Spawn  ? "__pas_task_spawn"
                      : Mode == StartMode::Pool ? "__pas_task_run_pool"
                                                : "__pas_task_start";
  B.CreateCall(classRuntime(Start, B.getVoidTy(), {Ptr}), {Task});
  return holdResult(Task, R->getAsyncType());
}


/// A call of iterator routine R (link, Self: Self's value or null): its
/// iterator, made with copies of the arguments; its body runs when its
/// values are asked for.
llvm::Value *CodeGenModule::emitIteratorCall(RoutineDecl *R, llvm::Value *Link, llvm::Value *Self,
                              llvm::ArrayRef<Expr *> Args){
  AsyncParts &A = Asyncs[R];
  llvm::PointerType *Ptr = B.getPtrTy();
  llvm::Constant *Fini = A.Fini ? static_cast<llvm::Constant *>(A.Fini)
                                : llvm::ConstantPointerNull::get(Ptr);
  llvm::Constant *Info = typeInfoOf(R->getIteratorType()->getElementType());
  llvm::Value *Gen = B.CreateCall(
      classRuntime("__pas_gen_new", Ptr, {Ptr, B.getInt64Ty(), Ptr, Ptr}),
      {Info, B.getInt64(DL.getTypeAllocSize(A.ArgsTy)), A.Entry, Fini}, "iter");
  llvm::Value *ArgsAddr =
      B.CreateCall(classRuntime("__pas_gen_args", Ptr, {Ptr}), {Gen}, "args");
  B.CreateStore(Link ? Link : llvm::ConstantPointerNull::get(Ptr),
                B.CreateStructGEP(A.ArgsTy, ArgsAddr, 0));
  if (R->hasSelf())
    B.CreateStore(Self, B.CreateStructGEP(A.ArgsTy, ArgsAddr, 1));
  unsigned Slot = firstParamSlot(R);
  auto Params = R->getParams();
  for (size_t I = 0; I < Params.size(); ++I) {
    Expr *Arg = I < Args.size() ? Args[I] : Params[I]->getDefault();
    storeExpr(B.CreateStructGEP(A.ArgsTy, ArgsAddr, Slot++), Params[I]->getType(),
              Arg);
  }
  return holdResult(Gen, R->getIteratorType());
}


/// IteratorNext(it): runs it to its next value; an i1.
llvm::Value *CodeGenModule::emitIteratorNext(llvm::Value *It){
  llvm::Value *R = B.CreateCall(
      classRuntime("__pas_gen_next", B.getInt32Ty(), {B.getPtrTy()}), {It}, "more");
  return B.CreateICmpNE(R, B.getInt32(0), "more");
}


/// The address of iterator It's current value.
llvm::Value *CodeGenModule::iteratorValueAddr(llvm::Value *It){
  return B.CreateCall(classRuntime("__pas_gen_value", B.getPtrTy(), {B.getPtrTy()}),
                      {It}, "value");
}


/// await / Wait: waits for task T (of type TT); its result (an address
/// for a structured type), or null without one.
llvm::Value *CodeGenModule::emitAwait(llvm::Value *T, TaskType *TT){
  llvm::PointerType *Ptr = B.getPtrTy();
  B.CreateCall(classRuntime("__pas_task_await", B.getVoidTy(), {Ptr}), {T});
  Type *RT = TT->getResult();
  if (!RT)
    return nullptr;
  llvm::Value *Res =
      B.CreateCall(classRuntime("__pas_task_result", Ptr, {Ptr}), {T}, "result");
  if (RT->isStructured())
    return Res;
  return loadScalar(Res, RT, "awaited");
}

llvm::Value *CodeGenModule::visitAwaitExpr(AwaitExpr *E){
  llvm::Value *T = visitExpr(E->getOperand());
  return emitAwait(T, llvm::cast<TaskType>(E->getOperand()->getType()));
}


llvm::Value *CodeGenModule::visitSpawnExpr(SpawnExpr *E){
  NextStart = E->isPoolRun() ? StartMode::Pool : StartMode::Spawn;
  llvm::Value *T = visitExpr(E->getCall());
  NextStart = StartMode::Here;
  return T;
}


void CodeGenModule::visitScopeStmt(ScopeStmt *S){
  llvm::PointerType *Ptr = B.getPtrTy();
  llvm::Value *Token = S->getToken() && !S->getToken()->getType()->isNil()
                           ? visitExpr(S->getToken())
                           : llvm::ConstantPointerNull::get(Ptr);
  llvm::Value *Scope = B.CreateCall(
      classRuntime("__pas_scope_enter", Ptr, {Ptr}), {Token}, "scope");
  llvm::Value *Slot = createTemp(Ptr, "scope.slot");
  B.CreateStore(Scope, Slot);
  pinInMemory(Slot);
  emitTryFinally([&] { emitSequence(S->getBody()); },
                 [&](llvm::Value *State) {
                   llvm::Value *Raised = B.CreateICmpEQ(
                       B.CreateLoad(B.getInt32Ty(), State, "state"),
                       B.getInt32(1), "raised");
                   B.CreateCall(classRuntime("__pas_scope_leave", B.getVoidTy(),
                                             {Ptr, B.getInt32Ty()}),
                                {B.CreateLoad(Ptr, Slot, "scope"),
                                 B.CreateZExt(Raised, B.getInt32Ty())});
                 });
}


/// yield e / yield break / yield from it.
void CodeGenModule::visitYieldStmt(YieldStmt *S){
  llvm::PointerType *Ptr = B.getPtrTy();
  IteratorType *IT = CurRoutine->getIteratorType();
  Type *ET = IT->getElementType();
  // (each value is assigned into the slot: what it held is released)
  auto YieldFrom = [&](llvm::Value *Src, Type *SrcET) {
    llvm::Value *Slot =
        B.CreateCall(classRuntime("__pas_gen_slot", Ptr, {}), {}, "slot");
    if (ET->isStructured()) // (the same type: Sema)
      copyValue(Slot, Src, ET);
    else
      storeValue(Slot,
                 convertForAssign(loadScalar(Src, SrcET), SrcET, ET,
                                  S->getBeginLoc()),
                 ET);
    B.CreateCall(classRuntime("__pas_gen_yield", B.getVoidTy(), {}), {});
  };
  switch (S->getYieldKind()) {
  case YieldStmt::Break:
    emitJump(ExitBlock, 0);
    B.SetInsertPoint(newBlock("after.jump")); // unreachable code follows
    return;
  case YieldStmt::Value: {
    llvm::Value *Slot =
        B.CreateCall(classRuntime("__pas_gen_slot", Ptr, {}), {}, "slot");
    storeExpr(Slot, ET, S->getValue());
    B.CreateCall(classRuntime("__pas_gen_yield", B.getVoidTy(), {}), {});
    return;
  }
  case YieldStmt::From: {
    Expr *E = S->getValue();
    Type *SrcET = llvm::cast<IteratorType>(E->getType())->getElementType();
    llvm::Value *Held = createTemp(Ptr, "from");
    B.CreateStore(llvm::ConstantPointerNull::get(Ptr), Held);
    storeValue(Held, visitExpr(E), E->getType()); // (counted)
    emitTryFinally(
        [&] {
          auto *CondBB = newBlock("from.cond");
          auto *BodyBB = newBlock("from.body");
          auto *EndBB = newBlock("from.end");
          B.CreateBr(CondBB);
          B.SetInsertPoint(CondBB);
          llvm::Value *I = B.CreateLoad(Ptr, Held, "from");
          B.CreateCondBr(emitIteratorNext(I), BodyBB, EndBB);
          B.SetInsertPoint(BodyBB);
          YieldFrom(iteratorValueAddr(B.CreateLoad(Ptr, Held, "from")), SrcET);
          B.CreateBr(CondBB);
          B.SetInsertPoint(EndBB);
        },
        [&](llvm::Value *) { finalizeValue(Held, E->getType()); });
    return;
  }
  }
}


void CodeGenModule::visitDetachedStmt(DetachedStmt *S){
  llvm::PointerType *Ptr = B.getPtrTy();
  llvm::Value *T = visitExpr(S->getCall());
  llvm::Value *Fn = llvm::ConstantPointerNull::get(Ptr), *Link = Fn;
  if (Expr *H = S->getHandler()) {
    llvm::Value *Pair = emitRoutineValue(H);
    Fn = B.CreateExtractValue(Pair, 0, "handler");
    Link = B.CreateExtractValue(Pair, 1, "link");
  }
  B.CreateCall(classRuntime("__pas_task_detach", B.getVoidTy(), {Ptr, Ptr, Ptr}),
               {T, Fn, Link});
}


/// WhenAll(tasks) / WhenAny(tasks): the tasks' data and count.
llvm::Value *CodeGenModule::emitWhen(CallExpr *E, bool All){
  llvm::PointerType *Ptr = B.getPtrTy();
  Expr *A = E->getArgs()[0];
  Type *AT = A->getType();
  llvm::Value *Data, *N;
  TaskType *TT;
  if (auto *ST = llvm::dyn_cast<ArrayType>(AT)) {
    Data = emitAddress(A);
    OrdinalBounds Bd = getOrdinalBounds(ST->getIndexType());
    N = B.getInt64(Bd.Hi - Bd.Lo + 1);
    TT = llvm::cast<TaskType>(ST->getElementType());
  } else if (auto *DT = llvm::dyn_cast<DynArrayType>(AT)) {
    Data = visitExpr(A);
    N = dynLength(Data);
    TT = llvm::cast<TaskType>(DT->getElementType());
  } else {
    llvm::Value *Desc = openDescriptor(A);
    Data = B.CreateLoad(Ptr, Desc, "data");
    N = openCount(Desc);
    TT = llvm::cast<TaskType>(llvm::cast<OpenArrayType>(AT)->getElementType());
  }
  auto *RT = llvm::cast<TaskType>(E->getType());
  llvm::Value *Task;
  if (All) {
    llvm::Constant *ResInfo = RT->getResult()
                                  ? typeInfoOf(RT->getResult())
                                  : llvm::ConstantPointerNull::get(Ptr);
    llvm::Constant *Elem = TT->getResult() ? typeInfoOf(TT->getResult())
                                           : llvm::ConstantPointerNull::get(Ptr);
    Task = B.CreateCall(
        classRuntime("__pas_when_all", Ptr, {Ptr, B.getInt64Ty(), Ptr, Ptr}),
        {Data, N, ResInfo, Elem}, "whenall");
  } else {
    Task = B.CreateCall(
        classRuntime("__pas_when_any", Ptr, {Ptr, B.getInt64Ty(), Ptr}),
        {Data, N, typeInfoOf(RT->getResult())}, "whenany");
  }
  return holdResult(Task, RT);
}


//===--------------------------------------------------------------------===//
// Built-in procedures and functions
//===--------------------------------------------------------------------===//

/// Splits off a leading file argument of read/write; returns the PasFile*.
llvm::Value *CodeGenModule::fileTarget(llvm::ArrayRef<Expr *> &Args, bool Input,
                        FileType *&FT){
  FT = nullptr;
  if (!Args.empty() && Args[0]->getType() && Args[0]->getType()->isFile()) {
    FT = llvm::cast<FileType>(Args[0]->getType());
    llvm::Value *F = loadFile(emitAddress(Args[0]));
    Args = Args.drop_front();
    return F;
  }
  return Input ? inputFile() : outputFile();
}


/// Address of a value convertible to type T: the designator itself if it
/// already has type T, else a temporary holding the converted value.
llvm::Value *CodeGenModule::addressForValue(Expr *E, Type *T){
  if (E->getType() == T && (T->isStructured() || T->isStringType()))
    return emitAddress(E);
  if (T->isStructured()) {
    llvm::Value *Tmp = createTemp(memType(T));
    copyAggregate(Tmp, emitAddress(E), T);
    return Tmp;
  }
  llvm::Value *Tmp = createTemp(memType(T));
  storeScalar(Tmp, convertForAssign(visitExpr(E), E->getType(), T,
                                    E->getBeginLoc()),
              T);
  return Tmp;
}


/// An expression that cannot call Pascal code: its evaluation cannot raise
/// an exception, so it is safe to format it into the write buffer. Runtime
/// calls (bounds checks, string concatenation) are fine: the end of the
/// statement flushes the buffer, or the next statement discards it.
bool CodeGenModule::simpleWriteExpr(Expr *E){
  if (auto *P = llvm::dyn_cast<ParenExpr>(E))
    return simpleWriteExpr(P->getInner());
  if (llvm::isa<IntegerLiteralExpr>(E) || llvm::isa<RealLiteralExpr>(E) ||
      llvm::isa<StringLiteralExpr>(E))
    return true;
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
    Decl *D = N->getDecl();
    return !N->getWith() && D &&
           (llvm::isa<VarDecl>(D) || llvm::isa<ConstDecl>(D));
  }
  if (auto *U = llvm::dyn_cast<UnaryExpr>(E))
    return !U->getOperator() && simpleWriteExpr(U->getOperand());
  if (auto *B = llvm::dyn_cast<BinaryExpr>(E))
    return !B->getOperator() && simpleWriteExpr(B->getLHS()) &&
           simpleWriteExpr(B->getRHS());
  if (auto *I = llvm::dyn_cast<IndexExpr>(E))
    return !I->getProperty() && simpleWriteExpr(I->getBase()) &&
           simpleWriteExpr(I->getIndex());
  if (auto *F = llvm::dyn_cast<FieldExpr>(E))
    return F->getMember() && llvm::isa<FieldDecl>(F->getMember()) &&
           simpleWriteExpr(F->getBase());
  return false;
}


bool CodeGenModule::simpleWriteArg(Expr *E){
  if (auto *F = llvm::dyn_cast<FormattedExpr>(E))
    return simpleWriteExpr(F->getValue()) &&
           (!F->getWidth() || simpleWriteExpr(F->getWidth())) &&
           (!F->getPrecision() || simpleWriteExpr(F->getPrecision()));
  return simpleWriteExpr(E);
}


/// Appends a compile-time constant write argument to a merged literal.
/// Only ordinals: a real would need the runtime's exact formatting.
bool CodeGenModule::appendConstText(Expr *E, std::string &Text){
  if (auto *SL = llvm::dyn_cast<StringLiteralExpr>(E)) {
    if (SL->isWideCharLiteral())
      return false;
    Text += SL->getValue().str();
    return true;
  }
  if (auto *IL = llvm::dyn_cast<IntegerLiteralExpr>(E)) {
    Type *T = IL->getType();
    if (!T || !T->isInteger())
      return false;
    if (isSignedOrdinal(T))
      Text += std::to_string((int64_t)IL->getValue());
    else
      Text += std::to_string(IL->getValue());
    return true;
  }
  return false;
}


void CodeGenModule::emitWrite(ProcCallStmt *S, bool IsWriteln){
  llvm::ArrayRef<Expr *> Args = S->getArgs();
  FileType *FT;
  llvm::Value *File = fileTarget(Args, /*Input=*/false, FT);
  if (FT && !FT->isText()) {
    for (Expr *Arg : Args)
      B.CreateCall(RT.WriteElem,
                   {File, addressForValue(Arg, FT->getElementType())});
    return;
  }
  bool Batch = true;
  for (Expr *Arg : Args)
    if (!simpleWriteArg(Arg)) {
      Batch = false;
      break;
    }
  B.CreateCall(Batch ? RT.WriteBegin : RT.WriteDiscard, {File});
  // Adjacent string, character and ordinal literals become one write; a
  // trailing literal of writeln gets the newline (items 19 and 2).
  bool NewlineWritten = false;
  for (size_t I = 0; I < Args.size();) {
    std::string Text;
    size_t J = I;
    while (J < Args.size()) {
      if (!appendConstText(Args[J], Text))
        break;
      ++J;
    }
    if (J > I) {
      bool Last = J == Args.size();
      if (Last && IsWriteln)
        Text.push_back('\n');
      llvm::Constant *S = B.CreateGlobalString(Text, ".str");
      B.CreateCall(RT.WriteStr,
                   {File, S, B.getInt64(Text.size()), B.getInt32(0)});
      if (Last && IsWriteln) {
        NewlineWritten = true;
        break;
      }
      I = J;
      continue;
    }
    emitWriteArg(File, Args[I]);
    ++I;
  }
  if (IsWriteln && !NewlineWritten)
    B.CreateCall(RT.Writeln, {File});
  if (Batch)
    B.CreateCall(RT.WriteEnd, {File});
}


void CodeGenModule::emitRead(ProcCallStmt *S, bool IsReadln){
  llvm::ArrayRef<Expr *> Args = S->getArgs();
  FileType *FT;
  llvm::Value *File = fileTarget(Args, /*Input=*/true, FT);
  if (FT && !FT->isText()) {
    Type *Elem = FT->getElementType();
    for (Expr *Arg : Args) {
      if (Arg->getType() == Elem) {
        B.CreateCall(RT.ReadElem, {File, emitAddress(Arg)});
        continue;
      }
      llvm::Value *Tmp = createTemp(memType(Elem));
      B.CreateCall(RT.ReadElem, {File, Tmp});
      llvm::Value *V = convertForAssign(loadScalar(Tmp, Elem), Elem,
                                        Arg->getType(), Arg->getBeginLoc());
      storeScalar(emitAddress(Arg), V, Arg->getType(), !isAliasedExpr(Arg));
    }
    return;
  }
  for (Expr *Arg : Args)
    emitReadArg(File, Arg);
  if (IsReadln)
    B.CreateCall(RT.Readln, {File});
}


void CodeGenModule::emitWriteArg(llvm::Value *File, Expr *Arg){
  Expr *Value = Arg;
  llvm::Value *Width = B.getInt32(0); // no field width
  llvm::Value *Precision = B.getInt32(-1);
  if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
    Value = F->getValue();
    Width = emitI32(F->getWidth());
    if (F->getPrecision())
      Precision = emitI32(F->getPrecision());
  }

  Type *T = Value->getType();
  if (T->isAnsiString()) {
    astrCall("__pas_write_astr", B.getVoidTy(), {File, visitExpr(Value), Width});
    return;
  }
  if (T->isPointer()) { // a pchar: the C string it points to
    astrCall("__pas_write_cstr", B.getVoidTy(), {File, visitExpr(Value), Width});
    return;
  }
  if (T->isShortString()) {
    B.CreateCall(RT.WritePStr, {File, emitPStr(Value), Width});
    return;
  }
  if (T->isStringType()) {
    B.CreateCall(RT.WriteStr, {File, emitAddress(Value),
                               B.getInt64(T->getStringLength()), Width});
    return;
  }
  llvm::Value *V = visitExpr(Value);
  if (T->isReal())
    B.CreateCall(RT.WriteReal, {File, V, Width, Precision});
  else if (T->getBase()->isChar())
    B.CreateCall(RT.WriteChar, {File, V, Width});
  else if (T->getBase()->isWideChar())
    B.CreateCall(RT.WriteChar, {File, wideToChar(V), Width});
  else if (T->getBase()->isBoolean())
    B.CreateCall(RT.WriteBool, {File, B.CreateZExt(V, B.getInt32Ty()), Width});
  else if (auto *ET = llvm::dyn_cast<EnumType>(T->getBase()))
    emitWriteEnum(File, V, T, ET, Width, Value->getBeginLoc());
  else if (isQWord(T->getBase()))
    B.CreateCall(classRuntime("__pas_write_uint", B.getVoidTy(),
                              {B.getPtrTy(), B.getInt64Ty(), B.getInt32Ty()}),
                 {File, V, Width});
  else
    B.CreateCall(RT.WriteInt, {File, toI64(V, T), Width});
}


/// The names of E's values as length-prefixed strings: a private
/// constant [N x ptr], one per enumeration type.
llvm::GlobalVariable *CodeGenModule::enumNameTable(EnumType *E){
  llvm::GlobalVariable *&G = EnumNameTables[E];
  if (G)
    return G;
  llvm::SmallVector<llvm::Constant *, 16> Names;
  for (ConstDecl *V : E->getValues())
    Names.push_back(getPStrConstant(V->getName().Spelling));
  auto *Ty = llvm::ArrayType::get(B.getPtrTy(), Names.size());
  G = new llvm::GlobalVariable(M, Ty, true, llvm::GlobalValue::PrivateLinkage,
                               llvm::ConstantArray::get(Ty, Names),
                               ".enumnames");
  G->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  return G;
}


/// write(e) of an enumeration's value (Turbo/FPC): its name, as declared,
/// padded on the right to the field width. A value outside the type is a
/// runtime error (always, as in Free Pascal).
void CodeGenModule::emitWriteEnum(llvm::Value *File, llvm::Value *V, Type *T, EnumType *E,
                   llvm::Value *Width, SourceLocation L){
  llvm::Value *X = toI64(V, T);
  uint64_t N = E->getValues().size();
  emitRuntimeCheck(B.CreateICmpUGE(X, B.getInt64(N)),
                   "invalid enumeration value", L);
  llvm::GlobalVariable *Table = enumNameTable(E);
  B.CreateCall(classRuntime("__pas_write_enum", B.getVoidTy(),
                            {B.getPtrTy(), B.getPtrTy(), B.getInt64Ty(),
                             B.getInt32Ty()}),
               {File, Table, X, Width});
}


void CodeGenModule::emitReadArg(llvm::Value *File, Expr *Arg){
  Type *T = Arg->getType();
  llvm::Value *Addr = emitAddress(Arg);
  if (T->isAnsiString()) {
    astrCall("__pas_read_astr", B.getVoidTy(), {File, Addr});
    return;
  }
  if (auto *SS = llvm::dyn_cast<ShortStringType>(T)) {
    B.CreateCall(RT.ReadStr, {File, Addr, B.getInt32(SS->getMaxLength())});
    return;
  }
  if (T->isReal()) {
    B.CreateStore(B.CreateCall(RT.ReadReal, {File}), Addr);
    return;
  }
  if (T->getBase()->isChar()) {
    llvm::Value *V = B.CreateCall(RT.ReadChar, {File});
    emitRangeCheck(V, T->getBase(), T, Arg->getBeginLoc());
    B.CreateStore(V, Addr);
    return;
  }
  if (isQWord(T)) { // all of 0 .. 2^64-1
    llvm::Value *V = B.CreateCall(
        classRuntime("__pas_read_uint64", B.getInt64Ty(), {B.getPtrTy()}),
        {File});
    B.CreateStore(V, Addr);
    return;
  }
  // Integers: read as i32 (or i64 for types that need it), then check the
  // value against the target type.
  bool Wide = intBits(T) == 64 || !isSignedOrdinal(T);
  llvm::Value *V = B.CreateCall(Wide ? RT.ReadInt64 : RT.ReadInt, {File});
  llvm::Value *X = B.CreateSExt(V, B.getInt64Ty());
  OrdinalBounds TB = getOrdinalBounds(T);
  if (rangeChecks(Arg->getBeginLoc()) &&
      (TB.Lo > (Wide ? INT64_MIN : INT32_MIN) ||
       TB.Hi < (Wide ? INT64_MAX : INT32_MAX)))
    emitRuntimeCheck(outsideRange(X, TB.Lo, TB.Hi), "value out of range",
                     Arg->getBeginLoc());
  B.CreateStore(B.CreateTrunc(X, regType(T)), Addr);
}


/// pack(a, i, z) / unpack(z, a, i): packed and unpacked arrays share a
/// representation, so both are a bounds-checked memcpy.
void CodeGenModule::emitPackUnpack(ProcCallStmt *S, bool IsPack){
  auto Args = S->getArgs();
  Expr *Unpacked = IsPack ? Args[0] : Args[1];
  Expr *Index = IsPack ? Args[1] : Args[2];
  Expr *Packed = IsPack ? Args[2] : Args[0];
  auto *UA = llvm::cast<ArrayType>(Unpacked->getType());
  auto *PA = llvm::cast<ArrayType>(Packed->getType());
  OrdinalBounds UB = getOrdinalBounds(UA->getIndexType());
  OrdinalBounds PB = getOrdinalBounds(PA->getIndexType());
  int64_t Count = PB.Hi - PB.Lo + 1;
  llvm::Value *I = toI64(visitExpr(Index), Index->getType());
  if (rangeChecks(Index->getBeginLoc()))
    emitRuntimeCheck(
        outsideRange(I, UB.Lo, UB.Hi - Count + 1),
        IsPack ? "'pack' index out of range" : "'unpack' index out of range",
        Index->getBeginLoc());
  llvm::Value *Base = emitAddress(Unpacked);
  llvm::Value *Offset = B.CreateSub(I, B.getInt64(UB.Lo));
  llvm::Value *Elem =
      B.CreateInBoundsGEP(memType(UA), Base, {B.getInt64(0), Offset});
  llvm::Value *PackedAddr = emitAddress(Packed);
  uint64_t Bytes = Count * sizeOf(UA->getElementType());
  if (IsPack)
    B.CreateMemCpy(PackedAddr, llvm::MaybeAlign(1), Elem, llvm::MaybeAlign(1), Bytes);
  else
    B.CreateMemCpy(Elem, llvm::MaybeAlign(1), PackedAddr, llvm::MaybeAlign(1), Bytes);
}


llvm::Value *CodeGenModule::emitFileQuery(BuiltinKind K, Expr *FileArg){
  llvm::Value *File = FileArg ? loadFile(emitAddress(FileArg)) : inputFile();
  llvm::Value *R = B.CreateCall(K == BuiltinKind::Eof ? RT.Eof : RT.Eoln, {File});
  return B.CreateICmpNE(R, B.getInt32(0), K == BuiltinKind::Eof ? "eof" : "eoln");
}


/// T(x): ordinal conversions keep the low bits (sign/zero-extending by
/// the source); pointer casts change only the type.
llvm::Value *CodeGenModule::emitTypecast(CallExpr *E){
  Expr *Arg = E->getArgs()[0];
  if (isMethodPointerType(E->getType()) && Arg->getType()->isRecord()) // TEv(m)
    return B.CreateLoad(memType(E->getType()), emitAddress(Arg));
  if (E->getType()->isAnsiString() && Arg->getType()->isPointer() &&
      llvm::cast<PointerType>(Arg->getType())->isUntyped())
    return visitExpr(Arg); // the pointer is an ansistring: borrowed
  if (E->getType()->isAnsiString())
    return emitAnsi(Arg);
  if (E->getType()->isShortString())
    return emitPStr(Arg);
  llvm::Value *V = visitExpr(Arg);
  if (auto *TV = llvm::dyn_cast<VectorType>(E->getType())) { // lane by lane
    Type *SE = llvm::cast<VectorType>(Arg->getType())->getElementType();
    Type *TE = TV->getElementType();
    llvm::Type *To = memType(TV);
    if (TE->isReal())
      return SE->isReal() ? V
             : isSignedOrdinal(SE) ? B.CreateSIToFP(V, To, "vconv")
                                   : B.CreateUIToFP(V, To, "vconv");
    if (TE->getBase()->isBoolean()) // nonzero lanes are true: 1
      return B.CreateZExt(
          B.CreateICmpNE(V, llvm::Constant::getNullValue(V->getType())), To,
          "vconv");
    if (V->getType() == To)
      return V;
    return isSignedOrdinal(SE) ? B.CreateSExtOrTrunc(V, To, "vconv")
                               : B.CreateZExtOrTrunc(V, To, "vconv");
  }
  if (Arg->getType()->isAnsiString()) { // pchar(s) is never nil
    auto *P = llvm::cast<PointerType>(E->getType());
    if (!P->isUntyped() && P->getPointee()->isChar())
      return astrCall("__pas_astr_pchar", B.getPtrTy(), {V});
    return V;
  }
  if (E->getType()->isOrdinal())
    return convertOrdinal(V, Arg->getType(), E->getType());
  if (E->getType()->isInterface())
    return convertForAssign(V, Arg->getType(), E->getType(), E->getBeginLoc());
  return V;
}


/// A runtime file routine (declared when first used).
llvm::FunctionCallee CodeGenModule::fileRuntime(llvm::StringRef Name, llvm::Type *Ret,
                                 llvm::ArrayRef<llvm::Type *> Params){
  return classRuntime(Name, Ret, Params);
}


llvm::Value *CodeGenModule::emitBuiltinCall(CallExpr *E, BuiltinDecl *BD){
  switch (BD->getBuiltinKind()) {
  case BuiltinKind::Wait:
    return emitAwait(visitExpr(E->getArgs()[0]),
                     llvm::cast<TaskType>(E->getArgs()[0]->getType()));
  case BuiltinKind::WhenAll:
  case BuiltinKind::WhenAny:
    return emitWhen(E, BD->getBuiltinKind() == BuiltinKind::WhenAll);
  case BuiltinKind::IteratorNext:
    return emitIteratorNext(visitExpr(E->getArgs()[0]));
  case BuiltinKind::IteratorValue: { // (read as a variable's value)
    Type *ET = E->getType();
    llvm::Value *Addr = iteratorValueAddr(visitExpr(E->getArgs()[0]));
    return ET->isStructured() ? Addr : loadScalar(Addr, ET, "value");
  }
  case BuiltinKind::VectorMask: { // a bit per lane: lane 1 is bit 0
    Expr *A = E->getArgs()[0];
    auto *VT = llvm::cast<VectorType>(A->getType());
    unsigned N = VT->getCount();
    llvm::Value *V = visitExpr(A);
    llvm::Value *Bits = B.CreateTrunc(
        V, llvm::FixedVectorType::get(B.getInt1Ty(), N), "lanes");
    Bits = B.CreateBitCast(Bits, B.getIntNTy(N), "mask");
    return B.CreateZExtOrTrunc(Bits, B.getInt64Ty(), "mask");
  }
  case BuiltinKind::VectorLoad: { // from any address (alignment 1)
    llvm::Value *P = visitExpr(E->getArgs()[1]);
    return B.CreateAlignedLoad(memType(E->getType()), P, llvm::MaybeAlign(1),
                               "vload");
  }
  case BuiltinKind::PopCnt:
  case BuiltinKind::CountTrailingZeros:
  case BuiltinKind::CountLeadingZeros: { // in the width of the argument's type
    Expr *A = E->getArgs()[0];
    llvm::Value *V = visitExpr(A);
    llvm::Value *R;
    BuiltinKind K = BD->getBuiltinKind();
    if (K == BuiltinKind::PopCnt)
      R = B.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, V);
    else
      R = B.CreateBinaryIntrinsic(K == BuiltinKind::CountTrailingZeros
                                      ? llvm::Intrinsic::cttz
                                      : llvm::Intrinsic::ctlz,
                                  V, B.getFalse());
    return B.CreateZExtOrTrunc(R, memType(E->getType()), "bits");
  }
  case BuiltinKind::AddChecked:
  case BuiltinKind::SubChecked:
  case BuiltinKind::MulChecked: { // the tuple (a op b, no overflow)
    auto *TT = llvm::cast<RecordType>(E->getType());
    Type *T = TT->getAllFields()[0]->getType();
    Expr *A = E->getArgs()[0], *C = E->getArgs()[1];
    llvm::Value *L = convertOrdinal(visitExpr(A), A->getType(), T);
    llvm::Value *R = convertOrdinal(visitExpr(C), C->getType(), T);
    bool Signed = isSignedOrdinal(T);
    BuiltinKind K = BD->getBuiltinKind();
    llvm::Intrinsic::ID ID =
        K == BuiltinKind::AddChecked
            ? (Signed ? llvm::Intrinsic::sadd_with_overflow
                      : llvm::Intrinsic::uadd_with_overflow)
        : K == BuiltinKind::SubChecked
            ? (Signed ? llvm::Intrinsic::ssub_with_overflow
                      : llvm::Intrinsic::usub_with_overflow)
            : (Signed ? llvm::Intrinsic::smul_with_overflow
                      : llvm::Intrinsic::umul_with_overflow);
    llvm::Value *Res = B.CreateBinaryIntrinsic(ID, L, R);
    llvm::Value *Tmp = createTemp(memType(TT), "checked");
    storeScalar(fieldAddress(Tmp, TT, TT->getAllFields()[0]),
                B.CreateExtractValue(Res, 0, "value"), T);
    storeScalar(fieldAddress(Tmp, TT, TT->getAllFields()[1]),
                B.CreateNot(B.CreateExtractValue(Res, 1), "ok"),
                TT->getAllFields()[1]->getType());
    return Tmp;
  }
  case BuiltinKind::FilePos:
  case BuiltinKind::FileSize:
    return B.CreateCall(
        fileRuntime(BD->getBuiltinKind() == BuiltinKind::FilePos
                        ? "__pas_filepos"
                        : "__pas_filesize",
                    B.getInt64Ty(), {B.getPtrTy()}),
        {loadFile(emitAddress(E->getArgs()[0]))},
        BD->getBuiltinKind() == BuiltinKind::FilePos ? "filepos" : "filesize");
  case BuiltinKind::GetFileHandle:
    return B.CreateCall(fileRuntime("__pas_file_handle", B.getInt32Ty(),
                                    {B.getPtrTy()}),
                        {loadFile(emitAddress(E->getArgs()[0]))}, "handle");
  case BuiltinKind::SizeOf: // the argument is not evaluated
    if (auto *OA = llvm::dyn_cast<OpenArrayType>(E->getArgs()[0]->getType()))
      return B.CreateMul(openCount(openDescriptor(E->getArgs()[0])),
                         B.getInt64(sizeOf(OA->getElementType())));
    return B.getInt64(sizeOf(E->getArgs()[0]->getType()));
  case BuiltinKind::Low:
  case BuiltinKind::High: {
    bool IsHigh = BD->getBuiltinKind() == BuiltinKind::High;
    Type *AT = E->getArgs()[0]->getType();
    if (AT->isOpenArray()) {
      if (!IsHigh)
        return B.getInt32(0);
      llvm::Value *N = openCount(openDescriptor(E->getArgs()[0]));
      return B.CreateSub(B.CreateTrunc(N, B.getInt32Ty()), B.getInt32(1));
    }
    if (AT->isDynArray()) {
      if (!IsHigh)
        return B.getInt32(0);
      llvm::Value *N = dynLength(visitExpr(E->getArgs()[0]));
      return B.CreateSub(B.CreateTrunc(N, B.getInt32Ty()), B.getInt32(1));
    }
    if (AT->isArrayOfConst()) {
      if (!IsHigh)
        return B.getInt32(0);
      llvm::Value *N = B.CreateLoad(B.getInt64Ty(), visitExpr(E->getArgs()[0]));
      return B.CreateSub(B.CreateTrunc(N, B.getInt32Ty()), B.getInt32(1));
    }
    int64_t V;
    if (auto *SS = llvm::dyn_cast<ShortStringType>(AT)) {
      V = IsHigh ? SS->getMaxLength() : 0;
    } else {
      if (auto *A = llvm::dyn_cast<ArrayType>(AT))
        AT = A->getIndexType();
      OrdinalBounds Bd = getOrdinalBounds(AT);
      V = IsHigh ? (isQWord(AT) ? -1 : Bd.Hi) : Bd.Lo; // (qword: 2^64-1)
    }
    return llvm::ConstantInt::get(regType(E->getType()),
                                  static_cast<uint64_t>(V), true);
  }
  case BuiltinKind::Assigned: {
    Expr *Arg = E->getArgs()[0];
    llvm::Value *P;
    if (isMethodPointerType(Arg->getType()) || Arg->getType()->isClosureType())
      P = B.CreateExtractValue(visitExpr(Arg), 0);
    else if (llvm::isa<RoutineType>(Arg->getType()))
      P = B.CreateExtractValue(
          B.CreateLoad(memType(Arg->getType()), emitAddress(Arg)), 0);
    else
      P = interfaceObject(visitExpr(Arg));
    return B.CreateIsNotNull(P, "assigned");
  }
  case BuiltinKind::ParamCount:
    return B.CreateCall(RT.ParamCount, {}, "paramcount");
  case BuiltinKind::ExceptAddr:
    return exceptAddr();
  case BuiltinKind::ParamStr: {
    llvm::Value *Tmp = newStringTemp();
    B.CreateCall(RT.ParamStr, {Tmp, emitI32(E->getArgs()[0])});
    return Tmp;
  }
  default:
    break;
  }
  switch (BD->getBuiltinKind()) {
  case BuiltinKind::Length: case BuiltinKind::Copy: case BuiltinKind::Pos:
  case BuiltinKind::Concat: case BuiltinKind::UpCase:
    return emitStringFunction(E, BD->getBuiltinKind());
  default:
    break;
  }
  if (BD->getBuiltinKind() == BuiltinKind::Eof ||
      BD->getBuiltinKind() == BuiltinKind::Eoln)
    return emitFileQuery(BD->getBuiltinKind(), E->getArgs()[0]);
  Expr *ArgE = E->getArgs()[0];
  Type *AT = ArgE->getType();
  llvm::Value *Arg = visitExpr(ArgE);
  SourceLocation Loc = E->getBeginLoc();

  switch (BD->getBuiltinKind()) {
  case BuiltinKind::Abs: {
    if (AT->isReal())
      return B.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, Arg, nullptr, "abs");
    Arg = convertOrdinal(Arg, AT, E->getType());
    if (!isSignedOrdinal(E->getType()))
      return Arg;
    llvm::Value *Min = llvm::ConstantInt::get(
        Arg->getType(),
        llvm::APInt::getSignedMinValue(Arg->getType()->getIntegerBitWidth()));
    if (overflowChecks(Loc))
      emitOverflowCheck(B.CreateICmpEQ(Arg, Min), "integer overflow in 'abs'", Loc);
    return B.CreateBinaryIntrinsic(llvm::Intrinsic::abs, Arg, B.getFalse(),
                                   nullptr, "abs");
  }
  case BuiltinKind::Sqr:
    if (AT->isReal())
      return B.CreateFMul(Arg, Arg, "sqr");
    Arg = convertOrdinal(Arg, AT, E->getType());
    return emitCheckedArith(BinaryOp::Mul, isSignedOrdinal(E->getType()), Arg,
                            Arg, Loc, "sqr");
  case BuiltinKind::Odd:
    return B.CreateTrunc(Arg, B.getInt1Ty(), "odd");
  case BuiltinKind::Ord:
    return convertOrdinal(Arg, AT, E->getType());
  case BuiltinKind::Chr:
    emitRangeCheck(Arg, AT, E->getType(), Loc, "'chr' argument out of range");
    return B.CreateTrunc(Arg, B.getInt8Ty(), "chr");
  case BuiltinKind::Succ:
  case BuiltinKind::Pred: {
    bool IsSucc = BD->getBuiltinKind() == BuiltinKind::Succ;
    Type *Base = AT->getBase();
    OrdinalBounds Bd = getOrdinalBounds(Base);
    if (isQWord(Base))
      Bd.Hi = -1; // 2^64-1
    if (rangeChecks(Loc))
      emitRuntimeCheck(
          B.CreateICmpEQ(toI64(Arg, Base), B.getInt64(IsSucc ? Bd.Hi : Bd.Lo)),
          IsSucc ? "'succ' of the last value of its type"
                 : "'pred' of the first value of its type",
          Loc);
    llvm::Value *One = llvm::ConstantInt::get(Arg->getType(), 1);
    return IsSucc ? B.CreateAdd(Arg, One, "succ") : B.CreateSub(Arg, One, "pred");
  }
  case BuiltinKind::Sin:
    return B.CreateUnaryIntrinsic(llvm::Intrinsic::sin, toDouble(Arg, AT));
  case BuiltinKind::Cos:
    return B.CreateUnaryIntrinsic(llvm::Intrinsic::cos, toDouble(Arg, AT));
  case BuiltinKind::Exp:
    return B.CreateUnaryIntrinsic(llvm::Intrinsic::exp, toDouble(Arg, AT));
  case BuiltinKind::Arctan:
    return B.CreateCall(RT.Atan, {toDouble(Arg, AT)});
  case BuiltinKind::Ln: {
    llvm::Value *X = toDouble(Arg, AT);
    if (rangeChecks(Loc))
      emitRuntimeCheck(
          B.CreateFCmpULE(X, llvm::ConstantFP::get(B.getDoubleTy(), 0.0)),
          "'ln' of zero or a negative number", Loc);
    return B.CreateUnaryIntrinsic(llvm::Intrinsic::log, X);
  }
  case BuiltinKind::Sqrt: {
    llvm::Value *X = toDouble(Arg, AT);
    if (rangeChecks(Loc))
      emitRuntimeCheck(
          B.CreateFCmpULT(X, llvm::ConstantFP::get(B.getDoubleTy(), 0.0)),
          "'sqrt' of a negative number", Loc);
    return B.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, X);
  }
  case BuiltinKind::Trunc:
  case BuiltinKind::Round: {
    if (!AT->isReal())
      return convertOrdinal(Arg, AT, E->getType());
    bool IsRound = BD->getBuiltinKind() == BuiltinKind::Round;
    // ISO: round() rounds halves away from zero, like llvm.round.
    // Turbo/FPC round halves to even (banker's rounding).
    llvm::Value *X =
        !IsRound ? Arg
        : B.CreateUnaryIntrinsic(LangOpts.Mode == LangMode::ISO
                                     ? llvm::Intrinsic::round
                                     : llvm::Intrinsic::roundeven,
                                 Arg);
    llvm::Type *ResTy = regType(E->getType());
    if (rangeChecks(Loc)) {
      // The result must lie in the integer (or int64) range; the bounds
      // are exact doubles. Also catches NaN (unordered comparisons are true).
      bool Is64 = ResTy->isIntegerTy(64);
      auto *Lo = llvm::ConstantFP::get(B.getDoubleTy(),
                                       Is64 ? -9223372036854775808.0 : -2147483649.0);
      auto *Hi = llvm::ConstantFP::get(B.getDoubleTy(),
                                       Is64 ? 9223372036854775808.0 : 2147483648.0);
      llvm::Value *Low = Is64 ? B.CreateFCmpULT(X, Lo) : B.CreateFCmpULE(X, Lo);
      emitRuntimeCheck(B.CreateOr(Low, B.CreateFCmpUGE(X, Hi)),
                       (llvm::Twine(IsRound ? "'round'" : "'trunc'") +
                        " result does not fit in " + E->getType()->getName())
                           .str(),
                       Loc);
    }
    return B.CreateFPToSI(X, ResTy, IsRound ? "round" : "trunc");
  }
  default:
    llvm_unreachable("not a builtin function");
  }
}

