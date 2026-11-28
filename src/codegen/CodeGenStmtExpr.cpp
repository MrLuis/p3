// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- CodeGenStmtExpr.cpp - AST -> LLVM IR ---------------------------------------===//

#include "CodeGenInternal.h"

using namespace p3;
using namespace p3::detail;


//===--------------------------------------------------------------------===//
// Calls
//===--------------------------------------------------------------------===//

llvm::Value *CodeGenModule::staticLinkFor(RoutineDecl *R){
  if (R->isClosure() && !R->getCapture().isValid()) // uses no frame
    return llvm::ConstantPointerNull::get(B.getPtrTy());
  return R->hasStaticLink() ? framePtrFor(R->getParent())
                            : llvm::ConstantPointerNull::get(B.getPtrTy());
}


/// The link argument of a direct call: null when the routine has no link
/// parameter (emitCallTo must not pass an extra null).
llvm::Value *CodeGenModule::callLink(RoutineDecl *R){
  return needsLink(R) ? staticLinkFor(R) : nullptr;
}


/// {function, static link} for a routine passed as a procedural argument.
llvm::Value *CodeGenModule::emitRoutineValue(Expr *Arg){
  if (auto *A = llvm::dyn_cast<AddrOfExpr>(Arg); A && A->getCode()) {
    // @TFoo.M of a static method
    llvm::Value *Pair = llvm::UndefValue::get(memType(A->getType()));
    Pair = B.CreateInsertValue(Pair, routineFunction(A->getCode()), 0);
    return B.CreateInsertValue(Pair, staticLinkFor(A->getCode()), 1);
  }
  if (auto *A = llvm::dyn_cast<AddrOfExpr>(Arg)) // @f (Turbo/FPC)
    Arg = A->getOperand();
  Decl *D = llvm::cast<NameRefExpr>(Arg)->getDecl();
  if (auto *V = llvm::dyn_cast<VarDecl>(D)) // forwarding a parameter
    return B.CreateLoad(memType(V->getType()), varAddress(V));
  auto *R = llvm::cast<RoutineDecl>(D);
  llvm::Value *Pair = llvm::UndefValue::get(memType(Arg->getType()));
  Pair = B.CreateInsertValue(Pair, routineFunction(R), 0);
  return B.CreateInsertValue(Pair, staticLinkFor(R), 1);
}


llvm::Value *CodeGenModule::emitCallTo(llvm::FunctionCallee Callee, llvm::Value *Link,
                        llvm::ArrayRef<VarDecl *> Params,
                        llvm::ArrayRef<Expr *> Args,
                        llvm::Value *Self,
                        Type *ResultTy, bool ToC,
                        llvm::Value *ResultDst,
                        uint32_t SliceMask){
  llvm::SmallVector<llvm::Value *, 8> Values;
  if (Link) // external (C) routines have none
    Values.push_back(Link);
  llvm::Value *ResultTmp = nullptr;
  unsigned ResultArgNo = Values.size();
  bool ByPointer = returnsByPointer(ResultTy) && !ToC;
  if (ByPointer) {
    // (ResultDst: the destination of the assignment, see emitResultInPlace)
    ResultTmp = ResultDst ? ResultDst : createTemp(memType(ResultTy), "result");
    Values.push_back(ResultTmp);
  }
  if (Self) // methods: after the result pointer
    Values.push_back(Self);
  // The arguments' results are held as usual; only a direct call's own
  // ansistring result may move into its assignment target.
  bool NoHold = NoHoldAnsiResult;
  NoHoldAnsiResult = false;
  emitArgValues(Params, Args, Values, ToC, SliceMask);
  NoHoldAnsiResult = NoHold;
  auto *Call = B.CreateCall(Callee, Values);
  if (ByPointer)
    addResultCallAttrs(Call, ResultArgNo, ResultTy);
  llvm::Value *Result = Call;
  if (ResultTmp) // expressions use a string's address
    return ResultTmp;
  // A string or record result comes back by value; expressions use its
  // address. (An interface, {ptr, ptr}, is a value.)
  if (Result->getType()->isArrayTy() ||
      (Result->getType()->isStructTy() &&
       Result->getType() != llvm::StructType::get(B.getPtrTy(), B.getPtrTy()))) {
    if (ResultTy && ResultTy->isRecord() && isManaged(ResultTy))
      return holdRecordResult(Result, ResultTy);
    llvm::Value *Tmp = createTemp(Result->getType(), "result");
    B.CreateStore(Result, Tmp);
    return Tmp;
  }
  if (ResultTy && isManaged(ResultTy)) {
    if (ResultTy->isAnsiString())
      return takeOrHoldAnsi(Result, NoHold);
    return holdResult(Result, ResultTy);
  }
  return Result;
}


Type *CodeGenModule::resultOf(RoutineDecl *R){
  return R->isFunction() ? R->getResultType() : nullptr;
}


/// Appends the values passed for Args (parameters Params) to Values.
/// ToC: to an external (C) routine, which takes callbacks as function
/// pointers, and maybe variadic arguments after Params.
void CodeGenModule::emitArgValues(llvm::ArrayRef<VarDecl *> Params,
                   llvm::ArrayRef<Expr *> Args,
                   llvm::SmallVectorImpl<llvm::Value *> &Values,
                   bool ToC, uint32_t SliceMask){
  // `out` arguments holding managed values are released first, before
  // any other argument is evaluated (another may be the same variable).
  llvm::SmallDenseMap<size_t, llvm::Value *, 2> Released;
  for (size_t I = 0; I < std::min(Args.size(), Params.size()); ++I) {
    VarDecl *P = Params[I];
    if (!P->isOutParam() || !isManaged(P->getType()))
      continue;
    if (auto *OA = llvm::dyn_cast<OpenArrayType>(P->getType())) {
      llvm::Value *Desc = emitOpenArrayArg(Args[I], OA);
      llvm::Value *Slot = createTemp(B.getPtrTy(), "open.slot");
      B.CreateStore(Desc, Slot); // what finalizeValue takes
      finalizeValue(Slot, OA);
      Released[I] = Desc;
      continue;
    }
    llvm::Value *Addr = emitAddress(Args[I]);
    finalizeValue(Addr, P->getType());
    Released[I] = Addr;
  }
  for (size_t I = 0; I < std::max(Args.size(), Params.size()); ++I) {
    // Arguments left out: the parameters' default values.
    Expr *Arg = I < Args.size() ? Args[I] : Params[I]->getDefault();
    if (!Arg)
      break;
    if (I >= Params.size()) {
      Values.push_back(emitVarArg(Arg));
      continue;
    }
    if (auto It = Released.find(I); It != Released.end()) {
      Values.push_back(It->second);
      continue;
    }
    VarDecl *P = Params[I];
    if (I < 32 && (SliceMask & (1u << I))) {
      AnsiSliceVal Sl = emitSliceExpr(Arg);
      Values.push_back(Sl.Ptr);
      Values.push_back(Sl.Len);
      continue;
    }
    if (isConstRefPtr(P) && Arg->isConstRefAddress()) { // the variable
      Values.push_back(emitAddress(Arg));
      continue;
    }
    if (P->getType()->isUntyped()) // Turbo/FPC: a variable, by reference
      Values.push_back(emitAddress(Arg));
    else if (auto *OA = llvm::dyn_cast<OpenArrayType>(P->getType()))
      Values.push_back(emitOpenArrayArg(Arg, OA));
    else if (P->getType()->isArrayOfConst())
      Values.push_back(emitConstArray(Arg));
    else if (llvm::Constant *CStr = cStringFor(P->getType(), Arg))
      Values.push_back(CStr);
    else if (llvm::isa<RoutineType>(P->getType()) && llvm::isa<NilExpr>(Arg))
      Values.push_back(ToC ? llvm::ConstantPointerNull::get(B.getPtrTy()) // a default
                           : emitRoutinePair(Arg, P->getType()));
    else if (llvm::isa<RoutineType>(P->getType()) && ToC) {
      llvm::Value *Pair = emitRoutineValue(Arg);
      Values.push_back(B.CreateExtractValue(Pair, 0));
    } else if (auto *RT = llvm::dyn_cast<RoutineType>(P->getType());
               RT && (RT->isOfObject() || RT->isReference()))
      Values.push_back(P->isVarParam() ? emitAddress(Arg) : emitRoutinePair(Arg, RT));
    else if (llvm::isa<RoutineType>(P->getType()))
      Values.push_back(emitRoutineValue(Arg));
    else if (P->getType()->isShortString() && !P->isVarParam())
      // Any string: a const parameter reads it there, a value parameter
      // copies what fits into its own variable.
      Values.push_back(emitPStr(Arg));
    else if (P->isVarParam() || P->getType()->isStructured())
      Values.push_back(emitAddress(Arg));
    else if (P->getType()->isAnsiString())
      Values.push_back(emitAnsi(Arg));
    else
      Values.push_back(convertForAssign(visitExpr(Arg), Arg->getType(),
                                        P->getType(), Arg->getBeginLoc()));
    if (isConstRefPtr(P)) { // a value: passed in a temporary
      llvm::Value *V = Values.pop_back_val();
      llvm::Value *Tmp = createTemp(memType(P->getType()));
      storeScalar(Tmp, V, P->getType());
      Values.push_back(Tmp);
    }
  }
}


/// A variadic argument of an external routine, with C's promotions:
/// ordinals to at least int, reals to double, string constants as C
/// strings.
llvm::Value *CodeGenModule::emitVarArg(Expr *Arg){
  Type *T = Arg->getType();
  if (T->isStringLiteral()) {
    if (auto *SL = llvm::dyn_cast<StringLiteralExpr>(Arg))
      return B.CreateGlobalString(SL->getValue(), ".cstr");
    auto *C = llvm::cast<ConstDecl>(
        llvm::cast<NameRefExpr>(unwrapConst(Arg))->getDecl());
    return B.CreateGlobalString(C->getValue().Str, ".cstr");
  }
  llvm::Value *V = visitExpr(Arg);
  if (T->isOrdinal() && V->getType()->getIntegerBitWidth() < 32)
    return isSignedOrdinal(T) ? B.CreateSExt(V, B.getInt32Ty())
                              : B.CreateZExt(V, B.getInt32Ty());
  return V; // (reals are doubles)
}


/// A string constant passed for a pchar parameter: a NUL-terminated copy.
llvm::Constant *CodeGenModule::cStringFor(Type *ParamTy, Expr *Arg){
  auto *P = llvm::dyn_cast<PointerType>(ParamTy);
  if (!P || P->isUntyped() || !P->getPointee() || !P->getPointee()->isChar())
    return nullptr;
  if (auto *SL = llvm::dyn_cast<StringLiteralExpr>(Arg))
    return B.CreateGlobalString(SL->getValue(), ".cstr");
  auto *N = llvm::dyn_cast<NameRefExpr>(unwrapConst(Arg));
  auto *C = N ? llvm::dyn_cast_or_null<ConstDecl>(N->getDecl()) : nullptr;
  if (!C)
    return nullptr;
  if (C->getValue().K == ConstValue::String)
    return B.CreateGlobalString(C->getValue().Str, ".cstr");
  if (C->getValue().K == ConstValue::Ordinal && C->getType()->getBase()->isChar())
    return B.CreateGlobalString(
        std::string(1, static_cast<char>(C->getValue().Int)), ".cstr");
  return nullptr;
}


llvm::Value *CodeGenModule::emitCall(RoutineDecl *R, llvm::ArrayRef<Expr *> Args,
                      llvm::Value *ResultDst){
  if (R->isAsync())
    return emitAsyncCall(R, staticLinkFor(R), nullptr, Args);
  if (R->isIterator())
    return emitIteratorCall(R, staticLinkFor(R), nullptr, Args);
  if (R->isExternal())
    return emitCallTo(ExternalFns[R], nullptr, R->getParams(), Args, nullptr,
                      nullptr, /*ToC=*/true);
  if (!ResultDst && !R->isMethod() && !R->hasStaticLink() &&
      !R->hasHeapFrame() && Args.size() <= R->getParams().size() &&
      Args.size() <= 32) {
    uint32_t Mask = 0;
    for (size_t I = 0; I < Args.size(); ++I)
      if (isSliceExpr(Args[I]) && isSliceSafeParam(R, R->getParams()[I]))
        Mask |= (1u << I);
    if (Mask != 0 && routineWritesOnlyLocals(R))
      return emitCallTo(getOrCreateSliceSpec(R, Mask), callLink(R),
                        R->getParams(), Args, nullptr, resultOf(R),
                        /*ToC=*/false, nullptr, Mask);
  }
  return emitCallTo(Functions[R], callLink(R), R->getParams(), Args, nullptr,
                    resultOf(R), /*ToC=*/false, ResultDst);
}


/// Call through a procedural / functional parameter or variable.
llvm::Value *CodeGenModule::emitIndirectCall(VarDecl *V, llvm::ArrayRef<Expr *> Args){
  auto *RT = llvm::cast<RoutineType>(V->getType());
  llvm::Value *Pair = B.CreateLoad(memType(RT), varAddress(V));
  // Parameters always hold a routine; variables may be nil.
  return emitCallThrough(Pair, RT, Args, V->isParam() ? SourceLocation()
                                                        : V->getLocation());
}


/// Calls the {function, static link} pair Pair of type RT. NilCheckLoc:
/// where to report a nil procedural value (invalid: no check).
llvm::Value *CodeGenModule::emitCallThrough(llvm::Value *Pair, RoutineType *RT,
                             llvm::ArrayRef<Expr *> Args,
                             SourceLocation NilCheckLoc){
  llvm::Value *Fn = B.CreateExtractValue(Pair, 0);
  if (NilCheckLoc.isValid() && rangeChecks(NilCheckLoc))
    emitRuntimeCheck(B.CreateIsNull(Fn), "call through a nil procedural value",
                     NilCheckLoc);
  if (RT->isOfObject()) { // {code, data}: Self comes from the pair
    llvm::Value *Self = B.CreateExtractValue(Pair, 1);
    return emitCallTo({routineFnType(RT->getParams(), RT->getResult(), true), Fn},
                      llvm::ConstantPointerNull::get(B.getPtrTy()), RT->getParams(),
                      Args, Self, RT->getResult());
  }
  if (RT->isCDecl()) { // a C entry (cthunk, external): no static link
    llvm::FunctionType *FTy =
        routineFnType(RT->getParams(), RT->getResult(), false, false);
    return emitCallTo({FTy, Fn}, nullptr, RT->getParams(), Args, nullptr,
                      RT->getResult());
  }
  llvm::FunctionType *FTy = routineFnType(RT->getParams(), RT->getResult());
  return emitCallTo({FTy, Fn}, B.CreateExtractValue(Pair, 1), RT->getParams(),
                    Args, nullptr, RT->getResult());
}


/// @obj.M, @TFoo.M, @M: {method's code, object or class}.
llvm::Value *CodeGenModule::emitMethodPointer(AddrOfExpr *A){
  RoutineDecl *R = A->getMethod();
  SourceLocation L = A->getBeginLoc();
  llvm::Value *Obj;
  bool SelfIsClass = false;
  if (auto *F = llvm::dyn_cast<FieldExpr>(A->getOperand())) {
    Expr *Base = F->getBase();
    auto *N = llvm::dyn_cast<NameRefExpr>(Base);
    if (Base->getType()->isRecord()) {
      Obj = emitAddress(Base);
    } else if (namesType(Base)) {
      Obj = vmtOf(llvm::cast<ClassType>(Base->getType()));
      SelfIsClass = true;
    } else {
      Obj = visitExpr(Base);
      SelfIsClass = Base->getType()->isClassRef();
    }
  } else {
    auto *N = llvm::cast<NameRefExpr>(A->getOperand());
    Obj = withBase(N->getWith());
    SelfIsClass = withIsClass(N->getWith());
  }
  llvm::Value *Code = Functions[R];
  if (R->isClassMethod()) {
    Obj = receiverClass(Obj, SelfIsClass, L);
    if (R->isDispatched())
      Code = loadVMTSlot(Obj, 6 + R->getVMTIndex(), "method");
  } else if (R->isDispatched()) {
    emitNilObjectCheck(Obj, L);
    Code = loadVirtual(Obj, static_cast<unsigned>(R->getVMTIndex()));
  }
  llvm::Value *Pair =
      llvm::UndefValue::get(llvm::StructType::get(B.getPtrTy(), B.getPtrTy()));
  Pair = B.CreateInsertValue(Pair, Code, 0);
  return B.CreateInsertValue(Pair, Obj, 1);
}


/// A procedural value of type T (Turbo/FPC): @routine, nil, or a
/// procedural variable / field / element.
llvm::Value *CodeGenModule::emitRoutinePair(Expr *E, Type *T){
  if (llvm::isa<NilExpr>(E))
    return llvm::Constant::getNullValue(memType(T));
  if (llvm::cast<RoutineType>(T)->isOfObject())
    return visitExpr(E);
  if (auto *A = llvm::dyn_cast<AddrOfExpr>(E)) {
    RoutineDecl *R = A->getCode(); // @TFoo.M of a static method
    if (!R)
      R = llvm::cast<RoutineDecl>(
          llvm::cast<NameRefExpr>(A->getOperand())->getDecl());
    llvm::Value *Pair = llvm::UndefValue::get(memType(T));
    Pair = B.CreateInsertValue(Pair, routineFunction(R), 0);
    return B.CreateInsertValue(Pair, staticLinkFor(R), 1);
  }
  if (llvm::cast<RoutineType>(T)->isReference()) // (also a call's result)
    return visitExpr(E);
  return B.CreateLoad(memType(T), emitAddress(E));
}


//===--------------------------------------------------------------------===//
// Objects and methods (objfpc)
//===--------------------------------------------------------------------===//

/// The object a with context stands for (Self for a method body), or the
/// address of a with statement's record.
//===--------------------------------------------------------------------===//
// Reference counting (COM interfaces, objfpc)
//===--------------------------------------------------------------------===//

/// The runtime's reference counting entry points (runtime/io.c).
llvm::FunctionCallee CodeGenModule::intfRuntime(llvm::StringRef Name){
  llvm::Type *Ptr = B.getPtrTy();
  if (Name == "__pas_intf_assign")
    return classRuntime(Name, B.getVoidTy(), {Ptr, Ptr, Ptr});
  // __pas_intf_addref(obj), __pas_intf_clear(addr), __pas_free_instance,
  // __pas_interfaced_construct(ed)(obj)
  return classRuntime(Name, B.getVoidTy(), {Ptr});
}


/// Releases the COM interfaces in the object of managed type T at Addr
/// and sets them to nil.
void CodeGenModule::finalizeValue(llvm::Value *Addr, Type *T){
  if (auto *OA = llvm::dyn_cast<OpenArrayType>(T)) {
    forEachOpenElement(
        Addr, OA,
        [&](llvm::Value *Elem) { finalizeValue(Elem, OA->getElementType()); },
        "fini");
    return;
  }
  if (T->isAnsiString()) {
    astrCall("__pas_astr_clear", B.getVoidTy(), {Addr});
    return;
  }
  if (T->isDynArray()) {
    astrCall("__pas_dyn_clear", B.getVoidTy(), {Addr});
    return;
  }
  if (T->isInterface()) {
    B.CreateCall(intfRuntime("__pas_intf_clear"), {Addr});
    return;
  }
  if (T->isClosureType()) {
    B.CreateCall(classRuntime("__pas_closure_clear", B.getVoidTy(),
                              {B.getPtrTy()}),
                 {Addr});
    return;
  }
  if (T->isTask() || T->isToken() || T->isIterator()) {
    B.CreateCall(classRuntime(T->isTask()       ? "__pas_task_clear"
                              : T->isIterator() ? "__pas_gen_clear"
                                                : "__pas_token_clear",
                              B.getVoidTy(), {B.getPtrTy()}),
                 {Addr});
    return;
  }
  if (auto *A = llvm::dyn_cast<ArrayType>(T)) {
    forEachElement(
        Addr, A,
        [&](llvm::Value *Elem) { finalizeValue(Elem, A->getElementType()); },
        "fini");
    return;
  }
  auto *R = llvm::cast<RecordType>(T);
  for (FieldDecl *F : R->getAllFields())
    if (isManaged(F->getType()))
      finalizeValue(fieldAddress(Addr, R, F), F->getType());
}


/// One more reference to each COM interface in the object of managed type
/// T at Addr (a copy was made).
void CodeGenModule::addRefValue(llvm::Value *Addr, Type *T){
  if (auto *OA = llvm::dyn_cast<OpenArrayType>(T)) {
    forEachOpenElement(
        Addr, OA,
        [&](llvm::Value *Elem) { addRefValue(Elem, OA->getElementType()); },
        "addref");
    return;
  }
  if (T->isAnsiString()) {
    astrCall("__pas_astr_addref", B.getVoidTy(),
             {B.CreateLoad(B.getPtrTy(), Addr, "s")});
    return;
  }
  if (T->isDynArray()) {
    astrCall("__pas_dyn_addref", B.getVoidTy(),
             {B.CreateLoad(B.getPtrTy(), Addr, "a")});
    return;
  }
  if (T->isInterface()) {
    llvm::Value *Obj = B.CreateLoad(B.getPtrTy(), Addr, "obj");
    B.CreateCall(intfRuntime("__pas_intf_addref"), {Obj});
    return;
  }
  if (T->isClosureType()) { // its frame
    llvm::Value *Frame = B.CreateLoad(
        B.getPtrTy(), B.CreateStructGEP(memType(T), Addr, 1), "frame");
    B.CreateCall(frameRuntime("__pas_frame_addref"), {Frame});
    return;
  }
  if (T->isTask() || T->isToken() || T->isIterator()) {
    llvm::Value *H = B.CreateLoad(B.getPtrTy(), Addr,
                                  T->isTask()       ? "task"
                                  : T->isIterator() ? "iter"
                                                    : "token");
    B.CreateCall(classRuntime(T->isTask()       ? "__pas_task_addref"
                              : T->isIterator() ? "__pas_gen_addref"
                                                : "__pas_token_addref",
                              B.getVoidTy(), {B.getPtrTy()}),
                 {H});
    return;
  }
  if (auto *A = llvm::dyn_cast<ArrayType>(T)) {
    forEachElement(
        Addr, A,
        [&](llvm::Value *Elem) { addRefValue(Elem, A->getElementType()); },
        "addref");
    return;
  }
  auto *R = llvm::cast<RecordType>(T);
  for (FieldDecl *F : R->getAllFields())
    if (isManaged(F->getType()))
      addRefValue(fieldAddress(Addr, R, F), F->getType());
}


/// *Dst := *Src for a record or array T with COM interfaces.
void CodeGenModule::copyManaged(llvm::Value *Dst, llvm::Value *Src, Type *T){
  addRefValue(Src, T);
  // The old value is released last: Src and Dst may be the same object.
  llvm::Value *Old = createTemp(memType(T), "old");
  copyAggregate(Old, Dst, T);
  copyAggregate(Dst, Src, T);
  finalizeValue(Old, T);
}


/// Addr^ := V for a COM interface.
void CodeGenModule::storeInterface(llvm::Value *Addr, llvm::Value *V){
  llvm::Value *Obj = B.CreateExtractValue(V, 0, "obj");
  llvm::Value *Table = B.CreateExtractValue(V, 1, "itable");
  B.CreateCall(intfRuntime("__pas_intf_assign"), {Addr, Obj, Table});
}


/// Stores V (scalar, of type T) at Addr: counted for COM interfaces.
void CodeGenModule::storeValue(llvm::Value *Addr, llvm::Value *V, Type *T,
                               bool AllowTBAA){
  if (T->isAnsiString())
    // A static string constant has refs < 0: storing it needs no count.
    astrCall(llvm::isa<llvm::Constant>(V) ? "__pas_astr_take"
                                          : "__pas_astr_assign",
             B.getVoidTy(), {Addr, V});
  else if (T->isDynArray())
    astrCall("__pas_dyn_assign", B.getVoidTy(), {Addr, V});
  else if (T->isClosureType())
    B.CreateCall(classRuntime("__pas_closure_assign", B.getVoidTy(),
                              {B.getPtrTy(), B.getPtrTy(), B.getPtrTy()}),
                 {Addr, B.CreateExtractValue(V, 0, "fn"),
                  B.CreateExtractValue(V, 1, "frame")});
  else if (T->isTask() || T->isToken() || T->isIterator())
    B.CreateCall(classRuntime(T->isTask()       ? "__pas_task_assign"
                              : T->isIterator() ? "__pas_gen_assign"
                                                : "__pas_token_assign",
                              B.getVoidTy(), {B.getPtrTy(), B.getPtrTy()}),
                 {Addr, V});
  else if (isManaged(T))
    storeInterface(Addr, V);
  else
    storeScalar(Addr, V, T, AllowTBAA);
}


/// Copies the record or array T at Src to Dst: counted if managed.
void CodeGenModule::copyValue(llvm::Value *Dst, llvm::Value *Src, Type *T){
  if (isManaged(T))
    copyManaged(Dst, Src, T);
  else
    copyAggregate(Dst, Src, T);
}


/// A function's result V, a COM interface (a reference the caller now
/// holds): kept in a temporary until the statement ends. T: its type, if
/// that may be a dynamic array.
llvm::Value *CodeGenModule::holdResult(llvm::Value *V, Type *T){
  heldTemp(V, T);
  return V;
}


llvm::Value *CodeGenModule::takeOrHoldAnsi(llvm::Value *V, bool NoHold){
  if (NoHold) {
    TookAnsiResult = true;
    return V;
  }
  return holdResult(V);
}


/// holdResult's temporary: its address.
llvm::Value *CodeGenModule::heldTemp(llvm::Value *V, Type *T){
  // (released by finalizeValue: also a closure's)
  bool Dyn = T && (T->isDynArray() || T->isClosureType() || T->isTask() ||
                   T->isToken() || T->isIterator());
  llvm::IRBuilder<> Entry(&CurFn->getEntryBlock(),
                          CurFn->getEntryBlock().begin());
  llvm::Value *Slot = Entry.CreateAlloca(
      V->getType(), nullptr,
      Dyn ? "dyn.tmp" : V->getType()->isPointerTy() ? "astr.tmp" : "intf.tmp");
  Entry.CreateStore(llvm::Constant::getNullValue(V->getType()), Slot);
  if (Dyn)
    RecordTemps[Slot] = T;
  clearTemp(Slot); // a loop's previous
  if (PinTemps)
    pinInMemory(Slot);
  B.CreateStore(V, Slot);
  LiveTemps.push_back(Slot);
  if (CurRoutine)
    RoutineTemps.push_back(Slot);
  return Slot;
}


/// A function's record result V, holding ansistrings the caller now owns:
/// kept in a temporary until the statement ends. Returns its address.
llvm::Value *CodeGenModule::holdRecordResult(llvm::Value *V, Type *T){
  llvm::IRBuilder<> Entry(&CurFn->getEntryBlock(),
                          CurFn->getEntryBlock().begin());
  llvm::Value *Slot = Entry.CreateAlloca(V->getType(), nullptr, "rec.tmp");
  Entry.CreateStore(llvm::Constant::getNullValue(V->getType()), Slot);
  finalizeValue(Slot, T); // a loop's previous
  if (PinTemps)
    pinInMemory(Slot);
  B.CreateStore(V, Slot);
  RecordTemps[Slot] = T;
  LiveTemps.push_back(Slot);
  if (CurRoutine)
    RoutineTemps.push_back(Slot);
  return Slot;
}


/// Releases the temporaries held since LiveTemps had Mark entries.
void CodeGenModule::releaseTemps(size_t Mark){
  for (size_t I = Mark; I < LiveTemps.size(); ++I)
    clearTemp(LiveTemps[I]);
  LiveTemps.resize(Mark);
}


/// Releases the ansistring or COM interface held in the temporary Slot.
void CodeGenModule::clearTemp(llvm::Value *Slot){
  if (Type *T = RecordTemps.lookup(Slot))
    finalizeValue(Slot, T);
  else if (llvm::cast<llvm::AllocaInst>(Slot)->getAllocatedType()->isPointerTy())
    astrCall("__pas_astr_clear", B.getVoidTy(), {Slot});
  else
    B.CreateCall(intfRuntime("__pas_intf_clear"), {Slot});
}


/// Is E a const parameter (whose value its caller owns)?
bool CodeGenModule::isConstParamRef(Expr *E){
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
    if (auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl()))
      return V->isConstParam();
  return false;
}


void CodeGenModule::emitAnsiIndexCheck(llvm::Value *S, llvm::Value *X, SourceLocation L){
  if (rangeChecks(L))
    emitRuntimeCheck(
        B.CreateICmpNE(astrCall("__pas_astr_bad_index", B.getInt32Ty(), {S, X}),
                       B.getInt32(0)),
        "string index out of bounds", L);
}


llvm::Value *CodeGenModule::visitExpr(Expr *E){
  if (E->getConversion())
    return emitConversion(E);
  return ExprVisitor<CodeGenModule, llvm::Value *>::visitExpr(E);
}


llvm::Value *CodeGenModule::emitConversion(Expr *E){
  RoutineDecl *Op = E->getConversion();
  E->suspendConversion();
  llvm::Value *V = emitCall(Op, {E});
  E->resumeConversion();
  return V;
}


//===--------------------------------------------------------------------===//
// goto
//===--------------------------------------------------------------------===//

llvm::BasicBlock *CodeGenModule::labelBlock(LabelDecl *L){
  llvm::BasicBlock *&BB = LabelBlocks[L];
  if (!BB)
    BB = newBlock("label." + L->getName().Name);
  return BB;
}


/// At entry of a block that is the target of non-local gotos:
/// switch (_setjmp(buf)) { case k: goto label_k; }
void CodeGenModule::emitSetJmpDispatch(Block &Blk, llvm::Value *Buf){
  llvm::Value *R = B.CreateCall(RT.SetJmp, {Buf}, "jmp");
  auto *Start = newBlock("body");
  llvm::SwitchInst *SW = B.CreateSwitch(R, Start);
  for (Decl *D : Blk.getDecls())
    if (auto *L = llvm::dyn_cast<LabelDecl>(D); L && L->isNonLocal())
      SW->addCase(B.getInt32(L->getIndex()), labelBlock(L));
  B.SetInsertPoint(Start);
}


//===--------------------------------------------------------------------===//
// Operators
//===--------------------------------------------------------------------===//

llvm::Value *CodeGenModule::emitShortCircuit(BinaryExpr *E){
  bool IsAnd = E->getOp() == BinaryOp::And;
  llvm::Value *L = visitExpr(E->getLHS());
  llvm::BasicBlock *LHSEnd = B.GetInsertBlock();
  auto *RHSBB = newBlock(IsAnd ? "and.rhs" : "or.rhs");
  auto *EndBB = newBlock(IsAnd ? "and.end" : "or.end");
  if (IsAnd)
    B.CreateCondBr(L, RHSBB, EndBB);
  else
    B.CreateCondBr(L, EndBB, RHSBB);

  llvm::SmallVector<LengthBound, 4> Bounds;
  llvm::SmallVector<ConstBound, 4> CBounds;
  bool HaveState = false;
  RangeState Saved;
  if (IsAnd && !RangeLocals.empty()) {
    collectLengthBounds(E->getLHS(), Bounds);
    collectConstBounds(E->getLHS(), CBounds);
    if (!Bounds.empty() || !CBounds.empty()) {
      Saved = saveRangeState();
      HaveState = true;
      applyLengthBounds(Bounds);
      applyConstBounds(CBounds);
    }
  }
  B.SetInsertPoint(RHSBB);
  llvm::Value *R = visitExpr(E->getRHS());
  if (HaveState)
    restoreRangeState(Saved);
  llvm::BasicBlock *RHSEnd = B.GetInsertBlock();
  B.CreateBr(EndBB);

  B.SetInsertPoint(EndBB);
  llvm::PHINode *Phi = B.CreatePHI(B.getInt1Ty(), 2, IsAnd ? "and" : "or");
  Phi->addIncoming(B.getInt1(!IsAnd), LHSEnd);
  Phi->addIncoming(R, RHSEnd);
  return Phi;
}


// Checks are skipped when a constant divisor makes them provably pass.
bool CodeGenModule::isConstNonZero(llvm::Value *V){
  auto *C = llvm::dyn_cast<llvm::ConstantInt>(V);
  return C && !C->isZero();
}

bool CodeGenModule::isConstNotMinusOne(llvm::Value *V){
  auto *C = llvm::dyn_cast<llvm::ConstantInt>(V);
  return C && !C->isMinusOne();
}


llvm::Constant *CodeGenModule::intConst(llvm::Value *Like, int64_t V){
  return llvm::ConstantInt::get(Like->getType(), static_cast<uint64_t>(V), true);
}


llvm::Value *CodeGenModule::emitDiv(llvm::Value *L, llvm::Value *R, bool Signed,
                     SourceLocation Loc){
  if (Signed) {
    ValueRange XL = rangeOfValue(L), XR = rangeOfValue(R);
    if (XL.Known && XL.Lo >= 0 && XR.Known && XR.Lo > 0)
      return B.CreateUDiv(L, R, "div");
  }
  if (!isConstNonZero(R))
    emitDivZeroCheck(B.CreateICmpEQ(R, intConst(R, 0)), Loc);
  if (!Signed)
    return B.CreateUDiv(L, R, "div");
  // INT_MIN div -1 overflows (and traps on x86).
  if (!isConstNotMinusOne(R)) {
    llvm::Value *Min = llvm::ConstantInt::get(
        L->getType(),
        llvm::APInt::getSignedMinValue(L->getType()->getIntegerBitWidth()));
    llvm::Value *IsMin = B.CreateICmpEQ(L, Min);
    llvm::Value *IsMinusOne = B.CreateICmpEQ(R, intConst(R, -1));
    llvm::Value *Overflow = B.CreateAnd(IsMin, IsMinusOne);
    emitOverflowCheck(Overflow, "integer overflow in 'div'", Loc);
  }
  return B.CreateSDiv(L, R, "div");
}


llvm::Value *CodeGenModule::emitMod(llvm::Value *L, llvm::Value *R, bool Signed,
                     SourceLocation Loc){
  if (Signed) {
    ValueRange XL = rangeOfValue(L), XR = rangeOfValue(R);
    if (XL.Known && XL.Lo >= 0 && XR.Known && XR.Lo > 0)
      return B.CreateURem(L, R, "mod");
  }
  if (LangOpts.Mode == LangMode::ISO) {
    // ISO 7185 6.7.2.2: error if j <= 0; result is i - k*j with
    // 0 <= result < j.
    auto *CR = llvm::dyn_cast<llvm::ConstantInt>(R);
    if (!CR || !CR->getValue().isStrictlyPositive())
      emitRuntimeCheck(B.CreateICmpSLE(R, intConst(R, 0)),
                       "'mod' by zero or a negative number", Loc);
    llvm::Value *Rem = B.CreateSRem(L, R);
    llvm::Value *Fixed = B.CreateAdd(Rem, R);
    return B.CreateSelect(B.CreateICmpSLT(Rem, intConst(Rem, 0)), Fixed, Rem,
                          "mod");
  }
  // Turbo/FPC: sign follows the dividend (like C %).
  if (!isConstNonZero(R))
    emitDivZeroCheck(B.CreateICmpEQ(R, intConst(R, 0)), Loc);
  if (!Signed)
    return B.CreateURem(L, R, "mod");
  // x mod -1 == 0, but INT_MIN srem -1 traps on x86: divide by 1 instead.
  llvm::Value *SafeR = B.CreateSelect(B.CreateICmpEQ(R, intConst(R, -1)),
                                      intConst(R, 1), R);
  return B.CreateSRem(L, SafeR, "mod");
}


llvm::Value *CodeGenModule::compareInts(BinaryOp Op, llvm::Value *L, llvm::Value *R,
                         bool Signed){
  llvm::CmpInst::Predicate P;
  switch (Op) {
  case BinaryOp::Eq: P = llvm::CmpInst::ICMP_EQ; break;
  case BinaryOp::Ne: P = llvm::CmpInst::ICMP_NE; break;
  case BinaryOp::Lt: P = Signed ? llvm::CmpInst::ICMP_SLT : llvm::CmpInst::ICMP_ULT; break;
  case BinaryOp::Le: P = Signed ? llvm::CmpInst::ICMP_SLE : llvm::CmpInst::ICMP_ULE; break;
  case BinaryOp::Gt: P = Signed ? llvm::CmpInst::ICMP_SGT : llvm::CmpInst::ICMP_UGT; break;
  case BinaryOp::Ge: P = Signed ? llvm::CmpInst::ICMP_SGE : llvm::CmpInst::ICMP_UGE; break;
  default: llvm_unreachable("not a comparison");
  }
  return B.CreateICmp(P, L, R, "cmp");
}


static bool tryAnsiConstLength(Expr *E, uint32_t &Len);

llvm::Value *CodeGenModule::emitCompare(BinaryExpr *E){
  BinaryOp Op = E->getOp();
  Type *LT = E->getLHS()->getType(), *RTy = E->getRHS()->getType();
  if (Op == BinaryOp::In)
    return emitIn(E);
  if (LT->isSet()) {
    // Compared in the larger of the two representations.
    Type *Wide = setBits(RTy) > setBits(LT) ? RTy : LT;
    llvm::Value *L = visitExpr(E->getLHS());
    L = convertSet(L, LT, Wide);
    llvm::Value *R = visitExpr(E->getRHS());
    R = convertSet(R, RTy, Wide);
    llvm::Value *Zero = llvm::ConstantInt::get(setType(Wide), 0);
    switch (Op) {
    case BinaryOp::Eq: return B.CreateICmpEQ(L, R, "cmp");
    case BinaryOp::Ne: return B.CreateICmpNE(L, R, "cmp");
    case BinaryOp::Le: // L is a subset of R
      return B.CreateICmpEQ(B.CreateAnd(L, B.CreateNot(R)), Zero, "subset");
    default:           // L is a superset of R
      return B.CreateICmpEQ(B.CreateAnd(R, B.CreateNot(L)), Zero, "superset");
    }
  }
  if (needsStringCompare(LT, RTy) &&
      (LT->isAnsiString() || RTy->isAnsiString())) {
    if (isSliceExpr(E->getLHS()) || isSliceExpr(E->getRHS()))
      return emitSliceCompare(Op, E->getLHS(), E->getRHS());
    uint32_t CL = 0, CR = 0;
    if ((Op == BinaryOp::Eq || Op == BinaryOp::Ne) &&
        (tryAnsiConstLength(E->getLHS(), CL) ||
         tryAnsiConstLength(E->getRHS(), CR)))
      return emitSliceCompare(Op, E->getLHS(), E->getRHS());
    llvm::Value *L = emitAnsi(E->getLHS());
    llvm::Value *R = emitAnsi(E->getRHS());
    if (Op == BinaryOp::Eq || Op == BinaryOp::Ne) {
      llvm::Value *Eq = astrCall("__pas_astr_eq", B.getInt32Ty(), {L, R});
      return B.CreateICmp(Op == BinaryOp::Eq ? llvm::CmpInst::ICMP_NE
                                             : llvm::CmpInst::ICMP_EQ,
                          Eq, B.getInt32(0), "cmp");
    }
    return compareInts(Op, astrCall("__pas_astr_compare", B.getInt32Ty(), {L, R}),
                       B.getInt32(0), /*Signed=*/true);
  }
  if (needsStringCompare(LT, RTy)) { // Turbo/FPC string comparison
    llvm::Value *Cmp = B.CreateCall(
        RT.StrCompare, {emitPStr(E->getLHS()), emitPStr(E->getRHS())});
    return compareInts(Op, Cmp, B.getInt32(0), /*Signed=*/true);
  }
  if (isMethodPointerType(LT) || isMethodPointerType(RTy)) {
    auto Part = [&](Expr *X) -> llvm::Value * {
      return llvm::isa<NilExpr>(X)
                 ? llvm::Constant::getNullValue(
                       llvm::StructType::get(B.getPtrTy(), B.getPtrTy()))
                 : visitExpr(X);
    };
    llvm::Value *L = Part(E->getLHS());
    llvm::Value *R = Part(E->getRHS());
    // The code only, as in Free Pascal.
    llvm::Value *LC = B.CreateExtractValue(L, 0);
    llvm::Value *RC = B.CreateExtractValue(R, 0);
    return Op == BinaryOp::Eq ? B.CreateICmpEQ(LC, RC, "cmp")
                              : B.CreateICmpNE(LC, RC, "cmp");
  }
  if (LT->isPointer() || LT->isNil() || LT->isClass() || LT->isClassRef() ||
      LT->isInterface() || LT->isDynArray()) { // interfaces: their objects
    llvm::Value *L = interfaceObject(visitExpr(E->getLHS()));
    llvm::Value *R = interfaceObject(visitExpr(E->getRHS()));
    switch (Op) {
    case BinaryOp::Eq: return B.CreateICmpEQ(L, R, "cmp");
    case BinaryOp::Ne: return B.CreateICmpNE(L, R, "cmp");
    // Pointers, by address (Turbo/FPC).
    case BinaryOp::Lt: return B.CreateICmpULT(L, R, "cmp");
    case BinaryOp::Le: return B.CreateICmpULE(L, R, "cmp");
    case BinaryOp::Gt: return B.CreateICmpUGT(L, R, "cmp");
    default:           return B.CreateICmpUGE(L, R, "cmp");
    }
  }

  if (LT->isStringType()) {
    // Equal-length strings compare lexicographically (unsigned chars).
    llvm::Value *Cmp = B.CreateCall(
        RT.Memcmp, {emitAddress(E->getLHS()), emitAddress(E->getRHS()),
                    B.getInt64(LT->getStringLength())});
    return compareInts(Op, Cmp, B.getInt32(0), /*Signed=*/true);
  }
  if (LT->isReal() || RTy->isReal()) {
    llvm::Value *L = toDouble(visitExpr(E->getLHS()), LT);
    llvm::Value *R = toDouble(visitExpr(E->getRHS()), RTy);
    llvm::CmpInst::Predicate P;
    switch (Op) {
    case BinaryOp::Eq: P = llvm::CmpInst::FCMP_OEQ; break;
    case BinaryOp::Ne: P = llvm::CmpInst::FCMP_UNE; break;
    case BinaryOp::Lt: P = llvm::CmpInst::FCMP_OLT; break;
    case BinaryOp::Le: P = llvm::CmpInst::FCMP_OLE; break;
    case BinaryOp::Gt: P = llvm::CmpInst::FCMP_OGT; break;
    default:           P = llvm::CmpInst::FCMP_OGE; break;
    }
    return B.CreateFCmp(P, L, R, "cmp");
  }
  llvm::Value *L = visitExpr(E->getLHS());
  llvm::Value *R = visitExpr(E->getRHS());
  if (Type *OpTy = E->getOperandType()) { // integers of possibly different types
    L = convertOrdinal(L, LT, OpTy);
    R = convertOrdinal(R, RTy, OpTy);
    return compareInts(Op, L, R, isSignedOrdinal(OpTy));
  }
  return compareInts(Op, L, R, isSignedOrdinal(LT));
}


/// Statements carry their source position into the debug info.
void CodeGenModule::visitStmt(Stmt *S){
  setDebugLoc(S->getBeginLoc());
  size_t Mark = LiveTemps.size(), StringMark = StringTemps.size();
  // The ranges of RangeLocals (optimization3 item 4) hold from store to
  // store in straight-line code. A statement with other control flow
  // forgets those of the variables it writes when it ends, since they
  // depend on the path taken; and when it starts, if code in it may run
  // again or be reached from the middle of it (a loop, a try statement).
  // A for loop does its own (visitForStmt).
  VarSet Written;
  bool Track = !RangeLocals.empty() && !llvm::isa<AssignStmt>(S) &&
               !llvm::isa<CompoundStmt>(S) && !llvm::isa<ProcCallStmt>(S) &&
               !llvm::isa<EmptyStmt>(S) && !llvm::isa<ForStmt>(S) &&
               !llvm::isa<WhileStmt>(S);
  if (Track) {
    Written = rangeLocalsWrittenBy(S);
    if (!llvm::isa<IfStmt>(S) && !llvm::isa<CaseStmt>(S) &&
        !llvm::isa<WithStmt>(S))
      forgetRanges(Written);
  }
  if (!llvm::isa<CompoundStmt>(S) && !llvm::isa<IfStmt>(S) &&
      !llvm::isa<AssignStmt>(S))
    AtRoutineTail = false;
  StmtVisitor<CodeGenModule>::visitStmt(S);
  AtRoutineTail = false;
  if (Track)
    forgetRanges(Written);
  releaseTemps(Mark); // the results held by its expressions
  releaseStringTemps(StringMark);
}


//===--------------------------------------------------------------------===//
// Statements
//===--------------------------------------------------------------------===//

void CodeGenModule::visitEmptyStmt(EmptyStmt *){}


/// The statements of a sequence. After one that declares owned
/// variables, the rest is the protected part of a try-finally that frees
/// them: their scope ends with the sequence, however it is left.
void CodeGenModule::emitSequence(llvm::ArrayRef<Stmt *> Body){
  bool Tail = AtRoutineTail;
  AtRoutineTail = false;
  for (size_t I = 0; I < Body.size(); ++I) {
    AtRoutineTail = Tail && (I + 1 == Body.size());
    visitStmt(Body[I]);
    AtRoutineTail = false;
    auto *D = llvm::dyn_cast<VarDeclStmt>(Body[I]);
    if (!D)
      continue;
    llvm::SmallVector<VarDecl *, 2> Owned;
    for (VarDecl *V : D->getVars())
      if (V->isOwned())
        Owned.push_back(V);
    if (Owned.empty())
      continue;
    emitTryFinally([&] { emitSequence(Body.drop_front(I + 1)); },
                   [&](llvm::Value *State) { emitFreeOwned(Owned, State); });
    return;
  }
}


/// var x: T [:= e]: x is set each time the statement runs, to e or to
/// zero (what it held, in an earlier iteration, is released).
void CodeGenModule::visitVarDeclStmt(VarDeclStmt *S){
  if (AssignStmt *A = S->getInitAssign()) {
    visitStmt(A);
    return;
  }
  for (VarDecl *V : S->getVars()) {
    Type *T = V->getType();
    llvm::Value *Addr = varAddress(V);
    if (V->isOwned()) {
      B.CreateCall(classRuntime("__pas_owned_set", B.getVoidTy(),
                                {B.getPtrTy(), B.getPtrTy()}),
                   {Addr, llvm::ConstantPointerNull::get(B.getPtrTy())});
      continue;
    }
    if (containsFile(T))
      continue; // (opened when its routine starts)
    if (isManaged(T))
      finalizeValue(Addr, T);
    emitZeroInit(Addr, T);
  }
}


/// If Value is Target + x, for a short string variable Target named in
/// both places: x.
Expr *CodeGenModule::appendedTo(Expr *Target, Expr *Value){
  auto *T = llvm::dyn_cast<NameRefExpr>(Target);
  auto *Sum = llvm::dyn_cast<BinaryExpr>(Value);
  if (!T || !Sum || Sum->getOp() != BinaryOp::Add || Sum->getOperator() ||
      !Sum->getType()->isShortString())
    return nullptr;
  auto *L = llvm::dyn_cast<NameRefExpr>(Sum->getLHS());
  if (!L || !llvm::isa_and_nonnull<VarDecl>(T->getDecl()) ||
      L->getDecl() != T->getDecl() || T->getWith() || L->getWith())
    return nullptr;
  return Sum->getRHS();
}


Expr *CodeGenModule::skipParens(Expr *E){
  while (auto *P = llvm::dyn_cast<ParenExpr>(E))
    E = P->getInner();
  return E;
}


bool CodeGenModule::isTargetName(Expr *E, VarDecl *V){
  auto *N = llvm::dyn_cast<NameRefExpr>(skipParens(E));
  return N && N->getDecl() == V && !N->getWith();
}


/// Is E, after parentheses and ansistring casts, Target itself?
bool CodeGenModule::isDirectTargetRef(Expr *E, VarDecl *V){
  E = skipParens(E);
  if (auto *C = llvm::dyn_cast<CallExpr>(E);
      C && E->getType()->isAnsiString() && C->getArgs().size() == 1 &&
      (C->isVarCast() || llvm::isa_and_nonnull<TypeDecl>(C->getDecl())))
    return isDirectTargetRef(C->getArgs()[0], V);
  return isTargetName(E, V);
}


/// The right operands of a left-nested ansistring addition whose leftmost
/// operand is V: s + a + b gives {a, b}.
bool CodeGenModule::ansiAppends(Expr *Value, VarDecl *V,
                        llvm::SmallVectorImpl<Expr *> &Parts){
  auto *Sum = llvm::dyn_cast<BinaryExpr>(Value);
  if (!Sum || Sum->getOp() != BinaryOp::Add || Sum->getOperator() ||
      !Value->getType()->isAnsiString())
    return false;
  if (!isTargetName(Sum->getLHS(), V) &&
      !ansiAppends(Sum->getLHS(), V, Parts))
    return false;
  Parts.push_back(Sum->getRHS());
  return true;
}


/// A fresh ansistring result (call, +, Copy/Concat, or char/pchar/shortstring
/// conversion): its reference can move directly into an assignment target via
/// __pas_astr_take instead of holding a temporary and recounting 1 -> 2 -> 1.
bool CodeGenModule::isFreshAnsiCall(Expr *Value){
  Value = skipParens(Value);
  if (!Value->getType()->isAnsiString()) {
    if (llvm::isa<StringLiteralExpr>(Value))
      return false;
    if (auto *N = llvm::dyn_cast<NameRefExpr>(unwrapConst(Value)))
      if (auto *C = llvm::dyn_cast<ConstDecl>(N->getDecl());
          C && C->getValue().K == ConstValue::String)
        return false;
    return true;
  }
  if (auto *Sum = llvm::dyn_cast<BinaryExpr>(Value))
    return Sum->getOp() == BinaryOp::Add && !Sum->getOperator();
  if (llvm::isa<InheritedExpr>(Value) ||
      llvm::isa<IndirectCallExpr>(Value))
    return true;
  auto *C = llvm::dyn_cast<CallExpr>(Value);
  if (!C || C->isVarCast() || C->isMethodCast())
    return false;
  if (C->getCalleeValue())
    return true;
  if (auto *BD = llvm::dyn_cast_or_null<BuiltinDecl>(C->getDecl()))
    return BD->getBuiltinKind() == BuiltinKind::Copy ||
           (BD->getBuiltinKind() == BuiltinKind::Concat &&
            C->getArgs().size() >= 2);
  return llvm::isa_and_nonnull<RoutineDecl>(C->getDecl()) ||
         llvm::isa_and_nonnull<VarDecl>(C->getDecl());
}


bool CodeGenModule::canMoveAnsiLocalToResult(VarDecl *TV, Expr *Value,
                                             bool TailMove){
  if (!TailMove || !TV || !CurRoutine || !CurRoutine->isFunction() ||
      CurRoutine->isAsync() || CurRoutine->isIterator() || !Cleanups.empty() ||
      TV != CurRoutine->getResultVar())
    return false;
  auto *N = llvm::dyn_cast<NameRefExpr>(skipParens(Value));
  if (!N || N->getWith())
    return false;
  auto *LV = llvm::dyn_cast_or_null<VarDecl>(N->getDecl());
  if (!LV || LV == TV || !LV->getType()->isAnsiString() ||
      LV->isCaptured() || BorrowedAnsiParams.count(LV) ||
      LV->isTypedConst() || LV->getAbsolute() || LV->isConstParam() ||
      LV->isVarParam() || LV->isOutParam() || isByRef(LV))
    return false;
  for (VarDecl *P : CurRoutine->getParams())
    if (P == LV)
      return true;
  for (Decl *D : CurRoutine->getBody()->getDecls())
    if (D == LV)
      return true;
  return false;
}


void CodeGenModule::emitAnsiAssign(llvm::Value *Dst, Expr *Target, VarDecl *TV,
                                   Expr *Value, bool TailMove){
  if (canMoveAnsiLocalToResult(TV, Value, TailMove)) {
    auto *LV = llvm::cast<VarDecl>(
        llvm::cast<NameRefExpr>(skipParens(Value))->getDecl());
    llvm::Value *SrcAddr = varAddress(LV);
    llvm::Value *V = B.CreateLoad(B.getPtrTy(), SrcAddr, "ansi.move");
    B.CreateStore(llvm::ConstantPointerNull::get(B.getPtrTy()), SrcAddr);
    astrCall("__pas_astr_take", B.getVoidTy(), {Dst, V});
    return;
  }
  bool Take = isFreshAnsiCall(Value) &&
              (!Target || llvm::isa<NameRefExpr>(Target));
  if (Take) {
    NoHoldAnsiResult = true;
    TookAnsiResult = false;
  }
  llvm::Value *V = emitAnsi(Value);
  if (Take) {
    NoHoldAnsiResult = false;
    Take = TookAnsiResult;
  }
  if (!Dst)
    Dst = emitAddress(Target);
  if (Take) {
    astrCall("__pas_astr_take", B.getVoidTy(), {Dst, V});
    return;
  }
  storeValue(Dst, V, TV ? TV->getType() : Target->getType());
}


/// optimization3 item 1: `Result := F(...)` where F returns a record by
/// pointer: this function's own result pointer is passed on to F, instead
/// of a temporary that is then copied. The pointer is `noalias` (the
/// caller's fresh temporary), so nothing else can see the record while F
/// writes it. Not done when
///  - the routine has a try or a non-local label (an exception caught
///    here could see a half-written result),
///  - the result variable is captured (a nested routine could read it),
///  - an argument mentions the result (F zeroes and fills it before it
///    copies its value parameters).
/// Returns whether the assignment was emitted.
bool CodeGenModule::emitResultInPlace(Expr *Target, Expr *Value, Type *TT){
  if (!returnsByPointer(TT) || !CurRoutine || !CurRoutine->isFunction() ||
      PinTemps)
    return false;
  VarDecl *RV = CurRoutine->getResultVar();
  auto *N = llvm::dyn_cast<NameRefExpr>(Target);
  if (!N || N->getWith() || N->getDecl() != RV || !isByRef(RV) ||
      FrameSlots.count(RV) || RV->isCaptured())
    return false;
  while (auto *P = llvm::dyn_cast<ParenExpr>(Value))
    Value = P->getInner();
  auto *C = llvm::dyn_cast<CallExpr>(Value);
  if (!C || C->isMethodCast() || C->isVarCast() || C->getCalleeValue() ||
      C->getWith())
    return false;
  auto *R = llvm::dyn_cast_or_null<RoutineDecl>(C->getDecl());
  if (!R || !R->isFunction() || R->isMethod() || R->isStatic() ||
      R->isExternal() || R->isAsync() || R->isIterator() ||
      R->getResultType() != TT)
    return false;
  VarSet Used;
  for (Expr *A : C->getArgs())
    PinAnalysis::usedBy(A, Used);
  for (VarDecl *P : R->getParams())
    PinAnalysis::usedBy(P->getDefault(), Used);
  if (Used.count(RV))
    return false;
  emitCall(R, C->getArgs(), emitAddress(Target));
  return true;
}


void CodeGenModule::visitAssignStmt(AssignStmt *S){
  bool TailMove = AtRoutineTail;
  AtRoutineTail = false;
  Expr *Target = S->getTarget(), *Value = S->getValue();
  Type *TT = Target->getType();
  // A variable of RangeLocals: the stored value's range from now on
  // (noted below), else none.
  VarDecl *RangeVar = PinAnalysis::plainAssignTarget(Target);
  if (RangeVar && !RangeLocals.count(RangeVar))
    RangeVar = nullptr;
  auto ForgetRange = llvm::make_scope_exit([&] {
    if (RangeVar)
      setVarRange(RangeVar, {});
  });
  if (PropertyDecl *P = assignedProperty(Target)) {
    emitPropertyWrite(P, Target, Value);
    return;
  }
  if (auto *SS = llvm::dyn_cast<ShortStringType>(TT)) {
    llvm::Value *Dst = emitAddress(Target);
    if (Expr *More = appendedTo(Target, Value)) {
      // s := s + x: x goes to the end of s, without a temporary.
      llvm::Value *Src = emitPStr(More);
      B.CreateCall(classRuntime("__pas_str_append", B.getVoidTy(),
                                {B.getPtrTy(), B.getInt32Ty(), B.getPtrTy()}),
                   {Dst, B.getInt32(SS->getMaxLength()), Src});
      return;
    }
    emitStringAssign(Dst, SS, Value);
    return;
  }
  if (TT->isAnsiString()) {
    VarDecl *TV = nullptr;
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Target); N && !N->getWith())
      TV = llvm::dyn_cast_or_null<VarDecl>(N->getDecl());
    if (TV && SliceLocals.count(TV)) {
      AnsiSliceVal Sl = emitSliceExpr(Value);
      SliceLocalSlots &Slots = SliceLocals[TV];
      B.CreateStore(Sl.Ptr, Slots.Ptr);
      B.CreateStore(Sl.Len, Slots.Len);
      return;
    }
    llvm::SmallVector<Expr *, 4> Parts;
    if (TV && ansiAppends(Value, TV, Parts)) {
      if (Parts.size() == 1) {
        // s := s + x: x goes to the end of s, in place when s is unique.
        llvm::Value *Dst = emitAddress(Target);
        if (Parts[0]->getType()->isCharLike())
          astrCall("__pas_astr_append_char", B.getVoidTy(),
                   {Dst, emitCharValue(Parts[0])});
        else
          astrCall("__pas_astr_append", B.getVoidTy(),
                   {Dst, emitAnsi(Parts[0])});
        return;
      }
      // s + a + b ...: evaluate every part before changing s, then append
      // them in order. (A bare s among the parts would read it after the
      // first append; keep the general concatenation for that.)
      bool Safe = true;
      for (Expr *P : Parts)
        if (isDirectTargetRef(P, TV)) {
          Safe = false;
          break;
        }
      if (Safe) {
        llvm::Value *Dst = emitAddress(Target);
        if (Parts.size() == 2 && (!Parts[0]->getType()->isCharLike() ||
                                  !Parts[1]->getType()->isCharLike())) {
          bool C0 = Parts[0]->getType()->isCharLike();
          bool C1 = Parts[1]->getType()->isCharLike();
          llvm::Value *V0 = C0 ? emitCharValue(Parts[0]) : emitAnsi(Parts[0]);
          llvm::Value *V1 = C1 ? emitCharValue(Parts[1]) : emitAnsi(Parts[1]);
          if (!C0 && C1)
            astrCall("__pas_astr_append_sc", B.getVoidTy(), {Dst, V0, V1});
          else if (C0 && !C1)
            astrCall("__pas_astr_append_cs", B.getVoidTy(), {Dst, V0, V1});
          else
            astrCall("__pas_astr_append2", B.getVoidTy(), {Dst, V0, V1});
          return;
        }
        llvm::SmallVector<llvm::Value *, 4> Values;
        for (Expr *P : Parts)
          Values.push_back(P->getType()->isCharLike() ? emitCharPiece(P)
                                                      : emitAnsi(P));
        llvm::Type *PtrArrTy = llvm::ArrayType::get(B.getPtrTy(), Parts.size());
        llvm::Type *LenArrTy =
            llvm::ArrayType::get(B.getInt64Ty(), Parts.size());
        llvm::Value *Ptrs = createTemp(PtrArrTy, "append.ptrs");
        llvm::Value *Lens = createTemp(LenArrTy, "append.lens");
        for (size_t I = 0; I < Parts.size(); ++I) {
          B.CreateStore(
              Values[I],
              B.CreateInBoundsGEP(PtrArrTy, Ptrs,
                                  {B.getInt64(0), B.getInt64(I)}));
          B.CreateStore(
              B.getInt64(Parts[I]->getType()->isCharLike() ? 1 : -1),
              B.CreateInBoundsGEP(LenArrTy, Lens,
                                  {B.getInt64(0), B.getInt64(I)}));
        }
        astrCall("__pas_astr_append_n", B.getVoidTy(),
                 {Dst, Ptrs, Lens, B.getInt64(Parts.size())});
        return;
      }
    }
    if (auto *Sum = llvm::dyn_cast<BinaryExpr>(Value);
        TV && Sum && Sum->getOp() == BinaryOp::Add && !Sum->getOperator() &&
        isTargetName(Sum->getRHS(), TV)) {
      // s := x + s: x goes to the front of s, in place when s is unique and
      // has spare capacity.
      llvm::Value *Dst = emitAddress(Target);
      Expr *Prefix = Sum->getLHS();
      if (Prefix->getType()->isCharLike())
        astrCall("__pas_astr_prepend_char", B.getVoidTy(),
                 {Dst, emitCharValue(Prefix)});
      else
        astrCall("__pas_astr_prepend", B.getVoidTy(), {Dst, emitAnsi(Prefix)});
      return;
    }
    emitAnsiAssign(nullptr, Target, TV, Value, TailMove);
    return;
  }
  if (TT->isStructured()) {
    if (emitResultInPlace(Target, Value, TT))
      return;
    llvm::Value *Dst = emitAddress(Target);
    copyValue(Dst, emitAddress(Value), TT);
    return;
  }
  if (llvm::isa<RoutineType>(TT)) { // procedural variable (Turbo/FPC)
    llvm::Value *Pair = emitRoutinePair(Value, TT);
    storeValue(emitAddress(Target), Pair, TT); // (a closure: counted)
    return;
  }
  llvm::Value *V = convertForAssign(visitExpr(Value), Value->getType(), TT,
                                    Value->getBeginLoc());
  if (isOwnedVar(Target)) { // frees the object it had
    llvm::Value *Slot = emitAddress(Target);
    B.CreateCall(classRuntime("__pas_owned_set", B.getVoidTy(),
                              {B.getPtrTy(), B.getPtrTy()}),
                 {Slot, V});
    return;
  }
  if (isBitPackedComponent(Target)) {
    bitStore(Target, V);
    return;
  }
  VarDecl *NewLenBase = RangeVar ? matchLengthExpr(Value) : nullptr;
  storeValue(emitAddress(Target), V, TT, !isAliasedExpr(Target));
  if (RangeVar) {
    setVarRange(RangeVar, rangeOfValue(V));
    if (NewLenBase)
      VarLengthOf[RangeVar] = NewLenBase;
    RangeVar = nullptr;
  }
}


/// Is E an owned variable (`x: TFoo owned`)?
bool CodeGenModule::isOwnedVar(Expr *E){
  auto *N = llvm::dyn_cast<NameRefExpr>(E);
  auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(N->getDecl()) : nullptr;
  return V && V->isOwned();
}


PropertyDecl *CodeGenModule::assignedProperty(Expr *Target){
  if (auto *N = llvm::dyn_cast<NameRefExpr>(Target))
    return llvm::dyn_cast_or_null<PropertyDecl>(N->getDecl());
  if (auto *F = llvm::dyn_cast<FieldExpr>(Target))
    return llvm::dyn_cast_or_null<PropertyDecl>(F->getMember());
  if (auto *I = llvm::dyn_cast<IndexExpr>(Target))
    return I->getProperty();
  return nullptr;
}


/// Target := Value for a property Target: stores into its field or
/// calls its write procedure.
void CodeGenModule::emitPropertyWrite(PropertyDecl *P, Expr *Target, Expr *Value){
  llvm::Value *Obj = nullptr;
  Type *C = nullptr;
  if (P->isClassProperty()) { // a class variable or a static method
    if (auto *R = llvm::dyn_cast<RoutineDecl>(P->getWrite())) {
      auto *I = llvm::dyn_cast<IndexExpr>(Target);
      emitCall(R, accessorArgs(P, I ? I->getPropertyArgs()
                                    : llvm::ArrayRef<Expr *>(),
                               Value));
      return;
    }
    storeProperty(P, Globals[llvm::cast<VarDecl>(P->getWrite())], Value);
    return;
  }
  if (auto *I = llvm::dyn_cast<IndexExpr>(Target)) {
    Obj = propertyObject(I, C);
  } else if (auto *N = llvm::dyn_cast<NameRefExpr>(Target)) {
    Obj = withBase(N->getWith());
    C = N->getWith()->Record->getType();
  } else {
    auto *F = llvm::cast<FieldExpr>(Target);
    if (F->getBase()->getType()->isInterface()) { // its setter
      auto *R = llvm::cast<RoutineDecl>(P->getWrite());
      emitInterfaceCall(R, F->getBase(), F->getField().Loc,
                        accessorArgs(P, {}, Value));
      return;
    }
    Obj = memberObject(F->getBase());
    C = F->getBase()->getType();
  }
  if (auto *R = llvm::dyn_cast<RoutineDecl>(P->getWrite())) {
    auto *I = llvm::dyn_cast<IndexExpr>(Target);
    emitMethodCall(R, Obj,
                   accessorArgs(P, I ? I->getPropertyArgs()
                                     : llvm::ArrayRef<Expr *>(),
                                Value),
                   /*Inherited=*/false, Target->getBeginLoc());
    return;
  }
  storeProperty(P, fieldAddress(Obj, C, llvm::cast<FieldDecl>(P->getWrite())),
                Value);
}


/// Value stored at Addr, property P's field or class variable.
void CodeGenModule::storeProperty(PropertyDecl *P, llvm::Value *Addr, Expr *Value){
  storeExpr(Addr, P->getType(), Value);
}


/// (a, b): its hidden variable, each element assigned its value.
llvm::Value *CodeGenModule::emitTuple(TupleExpr *E){
  auto *R = llvm::cast<RecordType>(E->getType());
  llvm::Value *Addr = varAddress(E->getTemp());
  for (size_t I = 0; I < E->getElements().size(); ++I) {
    FieldDecl *F = R->getAllFields()[I];
    storeExpr(fieldAddress(Addr, R, F), F->getType(), E->getElements()[I]);
  }
  return Addr;
}


void CodeGenModule::visitDestructureStmt(DestructureStmt *S){
  if (S->getInit())
    visitStmt(S->getInit());
  else if (!S->getAssigns().empty())
    emitAddress(S->getValue());
  for (AssignStmt *A : S->getAssigns())
    visitStmt(A);
}


/// Value, assignable to type T, stored at Addr (a variable of type T).
void CodeGenModule::storeExpr(llvm::Value *Addr, Type *T, Expr *Value){
  if (auto *SS = llvm::dyn_cast<ShortStringType>(T)) {
    emitStringAssign(Addr, SS, Value);
    return;
  }
  if (T->isAnsiString()) {
    storeValue(Addr, emitAnsi(Value), T);
    return;
  }
  if (T->isStructured()) {
    copyValue(Addr, emitAddress(Value), T);
    return;
  }
  if (llvm::isa<RoutineType>(T)) {
    storeValue(Addr, emitRoutinePair(Value, T), T); // (a closure: counted)
    return;
  }
  storeValue(Addr,
             convertForAssign(visitExpr(Value), Value->getType(), T,
                              Value->getBeginLoc()),
             T);
}


void CodeGenModule::visitProcCallStmt(ProcCallStmt *S){
  if (Expr *V = S->getCalleeValue()) { // a procedural field or property
    emitCallThrough(visitExpr(V), llvm::cast<RoutineType>(V->getType()),
                    S->getArgs(), S->getBeginLoc());
    return;
  }
  if (auto *R = llvm::dyn_cast<RoutineDecl>(S->getDecl())) {
    if (R->isStatic()) // a record's static method
      emitCall(R, S->getArgs());
    else if (R->isMethod()) // through Self or a with statement's object
      emitMethodCall(R, withBase(S->getWith()), S->getArgs(),
                     /*Inherited=*/false, S->getBeginLoc(),
                     withIsClass(S->getWith()));
    else
      emitCall(R, S->getArgs());
    return;
  }
  if (auto *V = llvm::dyn_cast<VarDecl>(S->getDecl())) {
    emitIndirectCall(V, S->getArgs());
    return;
  }
  auto *BD = llvm::cast<BuiltinDecl>(S->getDecl());
  if (BD->getBuiltinKind() == BuiltinKind::Wait) { // Wait(t);
    emitAwait(visitExpr(S->getArgs()[0]),
              llvm::cast<TaskType>(S->getArgs()[0]->getType()));
    return;
  }
  switch (BD->getBuiltinKind()) {
  case BuiltinKind::Write:
  case BuiltinKind::Writeln:
    emitWrite(S, BD->getBuiltinKind() == BuiltinKind::Writeln);
    return;
  case BuiltinKind::Read:
  case BuiltinKind::Readln:
    emitRead(S, BD->getBuiltinKind() == BuiltinKind::Readln);
    return;
  case BuiltinKind::Reset: case BuiltinKind::Rewrite: {
    // An untyped file's: with its record size (128 if not given).
    auto *FT = llvm::cast<FileType>(S->getArgs()[0]->getType());
    if (!FT->isUntyped())
      goto plain_file_proc;
    llvm::Value *File = loadFile(emitAddress(S->getArgs()[0]));
    llvm::Value *Size = S->getArgs().size() == 2
                            ? toI64(visitExpr(S->getArgs()[1]),
                                    S->getArgs()[1]->getType())
                            : B.getInt64(128);
    B.CreateCall(fileRuntime(BD->getBuiltinKind() == BuiltinKind::Reset
                                 ? "__pas_reset_size"
                                 : "__pas_rewrite_size",
                             B.getVoidTy(), {B.getPtrTy(), B.getInt64Ty()}),
                 {File, Size});
    return;
  }
  case BuiltinKind::Close: case BuiltinKind::CloseFile:
    B.CreateCall(fileRuntime("__pas_close", B.getVoidTy(), {B.getPtrTy()}),
                 {loadFile(emitAddress(S->getArgs()[0]))});
    return;
  case BuiltinKind::Flush:
    B.CreateCall(fileRuntime("__pas_flush", B.getVoidTy(), {B.getPtrTy()}),
                 {loadFile(emitAddress(S->getArgs()[0]))});
    return;
  case BuiltinKind::Assign: case BuiltinKind::AssignFile: {
    llvm::Value *File = loadFile(emitAddress(S->getArgs()[0]));
    llvm::Value *Name = emitAnsi(S->getArgs()[1]);
    B.CreateCall(fileRuntime("__pas_assign", B.getVoidTy(),
                             {B.getPtrTy(), B.getPtrTy()}),
                 {File, Name});
    return;
  }
  case BuiltinKind::BlockRead: case BuiltinKind::BlockWrite: {
    auto Args = S->getArgs();
    bool Read = BD->getBuiltinKind() == BuiltinKind::BlockRead;
    llvm::Value *File = loadFile(emitAddress(Args[0]));
    llvm::Value *Buf = emitAddress(Args[1]);
    llvm::Value *Count = toI64(visitExpr(Args[2]), Args[2]->getType());
    llvm::Value *Done = B.CreateCall(
        fileRuntime(Read ? "__pas_blockread" : "__pas_blockwrite",
                    B.getInt64Ty(),
                    {B.getPtrTy(), B.getPtrTy(), B.getInt64Ty(),
                     B.getInt32Ty()}),
        {File, Buf, Count, B.getInt32(Args.size() == 3)});
    if (Args.size() == 4) // the records done
      storeScalar(emitAddress(Args[3]),
                  B.CreateSExtOrTrunc(Done, regType(Args[3]->getType())),
                  Args[3]->getType(), !isAliasedExpr(Args[3]));
    return;
  }
  case BuiltinKind::Seek: {
    llvm::Value *File = loadFile(emitAddress(S->getArgs()[0]));
    llvm::Value *N = toI64(visitExpr(S->getArgs()[1]), S->getArgs()[1]->getType());
    B.CreateCall(fileRuntime("__pas_seek", B.getVoidTy(),
                             {B.getPtrTy(), B.getInt64Ty()}),
                 {File, N});
    return;
  }
  case BuiltinKind::Get: case BuiltinKind::Put: case BuiltinKind::Page:
  plain_file_proc: {
    llvm::FunctionCallee Fn;
    switch (BD->getBuiltinKind()) {
    case BuiltinKind::Reset:   Fn = RT.Reset; break;
    case BuiltinKind::Rewrite: Fn = RT.Rewrite; break;
    case BuiltinKind::Get:     Fn = RT.Get; break;
    case BuiltinKind::Put:     Fn = RT.Put; break;
    default:                   Fn = RT.Page; break;
    }
    llvm::Value *File = S->getArgs().empty()
                            ? outputFile()
                            : loadFile(emitAddress(S->getArgs()[0]));
    B.CreateCall(Fn, {File});
    return;
  }
  case BuiltinKind::Pack:
  case BuiltinKind::Unpack:
    emitPackUnpack(S, BD->getBuiltinKind() == BuiltinKind::Pack);
    return;
  case BuiltinKind::Break:
  case BuiltinKind::Continue:
  case BuiltinKind::Exit: {
    if (!S->getArgs().empty()) { // exit(value): set the result first
      VarDecl *Result = CurRoutine->getResultVar();
      Expr *Value = S->getArgs()[0];
      Type *RT = Result->getType();
      llvm::Value *Addr = varAddress(Result);
      if (auto *SS = llvm::dyn_cast<ShortStringType>(RT))
        emitStringAssign(Addr, SS, Value);
      else if (RT->isAnsiString()) {
        bool TailMove = CurRoutine && CurRoutine->isFunction() &&
                        !CurRoutine->isAsync() && !CurRoutine->isIterator() &&
                        Cleanups.empty() &&
                        !hasNonLocalLabels(*CurRoutine->getBody());
        emitAnsiAssign(Addr, nullptr, Result, Value, TailMove);
      } else if (isMethodPointerType(RT) || RT->isClosureType())
        storeValue(Addr, emitRoutinePair(Value, RT), RT);
      else if (RT->isStructured())
        copyValue(Addr, emitAddress(Value), RT);
      else
        storeValue(Addr,
                   convertForAssign(visitExpr(Value), Value->getType(), RT,
                                    Value->getBeginLoc()),
                   RT);
    }
    if (BD->getBuiltinKind() == BuiltinKind::Exit)
      emitJump(ExitBlock, 0);
    else if (BD->getBuiltinKind() == BuiltinKind::Continue)
      emitJump(Loops.back().Continue, Loops.back().Cleanups);
    else
      emitJump(Loops.back().Break, Loops.back().Cleanups);
    B.SetInsertPoint(newBlock("after.jump")); // unreachable code follows
    return;
  }
  case BuiltinKind::SetLength: {
    auto A = S->getArgs();
    llvm::Value *Addr = emitAddress(A[0]);
    if (auto *DA = llvm::dyn_cast<DynArrayType>(A[0]->getType())) {
      llvm::SmallVector<llvm::Value *, 2> Ns;
      for (Expr *N : A.drop_front())
        Ns.push_back(toI64(visitExpr(N), N->getType()));
      emitDynSetLength(Addr, DA, Ns);
      return;
    }
    llvm::Value *N = emitI32(A[1]);
    if (A[0]->getType()->isAnsiString())
      astrCall("__pas_astr_setlength", B.getVoidTy(), {Addr, N});
    else
      astrCall("__pas_str_setlength", B.getVoidTy(),
               {Addr,
                B.getInt32(llvm::cast<ShortStringType>(A[0]->getType())->getMaxLength()),
                N});
    return;
  }
  case BuiltinKind::Delete: {
    auto A = S->getArgs();
    if (A[0]->getType()->isAnsiString()) {
      llvm::Value *Addr = emitAddress(A[0]);
      llvm::Value *Index = emitI32(A[1]);
      llvm::Value *Count = emitI32(A[2]);
      astrCall("__pas_astr_delete", B.getVoidTy(), {Addr, Index, Count});
      return;
    }
    B.CreateCall(RT.StrDelete, {emitAddress(A[0]), emitI32(A[1]), emitI32(A[2])});
    return;
  }
  case BuiltinKind::Insert: {
    auto A = S->getArgs();
    if (A[1]->getType()->isAnsiString()) {
      llvm::Value *Src = emitAnsi(A[0]);
      llvm::Value *Addr = emitAddress(A[1]);
      llvm::Value *Index = emitI32(A[2]);
      astrCall("__pas_astr_insert", B.getVoidTy(), {Src, Addr, Index});
      return;
    }
    auto *SS = llvm::cast<ShortStringType>(A[1]->getType());
    B.CreateCall(RT.StrInsert, {emitPStr(A[0]), emitAddress(A[1]),
                                B.getInt32(SS->getMaxLength()), emitI32(A[2])});
    return;
  }
  case BuiltinKind::Inc:
  case BuiltinKind::Dec: {
    Expr *Var = S->getArgs()[0];
    Type *T = Var->getType();
    bool AllowTBAA = !isAliasedExpr(Var);
    llvm::Value *Addr = emitAddress(Var);
    llvm::Value *Old = loadScalar(Addr, T, "", AllowTBAA);
    llvm::Value *Step =
        S->getArgs().size() == 2
            ? toI64(visitExpr(S->getArgs()[1]), S->getArgs()[1]->getType())
            : B.getInt64(1);
    bool IsInc = BD->getBuiltinKind() == BuiltinKind::Inc;
    SourceLocation L = S->getBeginLoc();
    if (auto *PT = llvm::dyn_cast<PointerType>(T)) { // by n of its pointee
      llvm::Value *Delta = IsInc ? Step : B.CreateNeg(Step);
      if (pointerMathChecked(L))
        emitPointerAddCheck(PT, Old, Delta, L);
      llvm::StoreInst *PS =
          B.CreateStore(B.CreateGEP(pointeeMemType(PT), Old, Delta), Addr);
      if (AllowTBAA)
        if (llvm::MDNode *Tag = tbaaTagFor(T))
          PS->setMetadata(llvm::LLVMContext::MD_tbaa, Tag);
      return;
    }
    if (T->isIntegerLike() && intBits(T) == 64) { // no wider type to check in
      // (qword: unsigned)
      llvm::Value *New = emitCheckedArith(IsInc ? BinaryOp::Add : BinaryOp::Sub,
                                          isSignedOrdinal(T), Old, Step, L,
                                          S->getCallee().Name);
      emitRangeCheck(New, T->getBase(), T, L);
      storeScalar(Addr, New, T, AllowTBAA);
      return;
    }
    // Compute in i64 so the range check sees the true result.
    llvm::Value *Old64 = toI64(Old, T);
    llvm::Value *Delta = IsInc ? Step : B.CreateNeg(Step);
    llvm::Value *Wide = B.CreateAdd(Old64, Delta);
    if (T->isIntegerLike()
            ? overflowChecks(L) ||
                  (rangeChecks(L) && !(wraps(L) && llvm::isa<IntegerType>(T)))
            : rangeChecks(L)) {
      OrdinalBounds Bd = getOrdinalBounds(T);
      if (T->isIntegerLike())
        emitOverflowCheck(outsideRange(Wide, Bd.Lo, Bd.Hi),
                          "integer overflow in '" + S->getCallee().Name.str() + "'",
                          L);
      else
        emitRuntimeCheck(outsideRange(Wide, Bd.Lo, Bd.Hi), "value out of range", L);
    }
    storeScalar(Addr, B.CreateTrunc(Wide, regType(T)), T, AllowTBAA);
    return;
  }
  case BuiltinKind::GetMem: {
    auto A = S->getArgs();
    llvm::Value *Mem =
        B.CreateCall(RT.New, {toI64(visitExpr(A[1]), A[1]->getType())});
    B.CreateStore(Mem, emitAddress(A[0]));
    return;
  }
  case BuiltinKind::FreeMem:
    B.CreateCall(RT.FreeMem, {visitExpr(S->getArgs()[0])});
    return;
  case BuiltinKind::ReallocMem: {
    auto A = S->getArgs();
    llvm::Value *Addr = emitAddress(A[0]);
    llvm::Value *Size = toI64(visitExpr(A[1]), A[1]->getType());
    llvm::Value *Old = B.CreateLoad(B.getPtrTy(), Addr);
    B.CreateStore(B.CreateCall(RT.Realloc, {Old, Size}), Addr);
    return;
  }
  case BuiltinKind::FillChar:
  case BuiltinKind::Move: {
    // A negative count does nothing.
    auto A = S->getArgs();
    bool IsFill = BD->getBuiltinKind() == BuiltinKind::FillChar;
    llvm::Value *Dst = emitAddress(A[IsFill ? 0 : 1]);
    Expr *CountE = A[IsFill ? 1 : 2];
    llvm::Value *Count = toI64(visitExpr(CountE), CountE->getType());
    Count = B.CreateSelect(B.CreateICmpSLT(Count, B.getInt64(0)),
                           B.getInt64(0), Count);
    if (IsFill) {
      llvm::Value *V = B.CreateTrunc(toI64(visitExpr(A[2]), A[2]->getType()),
                                     B.getInt8Ty());
      B.CreateMemSet(Dst, V, Count, llvm::MaybeAlign(1));
    } else {
      B.CreateMemMove(Dst, llvm::MaybeAlign(1), emitAddress(A[0]),
                      llvm::MaybeAlign(1), Count);
    }
    return;
  }
  case BuiltinKind::VectorStore: { // at any address (alignment 1)
    auto A = S->getArgs();
    llvm::Value *P = visitExpr(A[0]);
    B.CreateAlignedStore(visitExpr(A[1]), P, llvm::MaybeAlign(1));
    return;
  }
  case BuiltinKind::Halt: {
    llvm::Value *Code =
        S->getArgs().empty() ? B.getInt32(0) : emitI32(S->getArgs()[0]);
    B.CreateCall(RT.Halt, {Code});
    B.CreateUnreachable();
    B.SetInsertPoint(newBlock("after.halt")); // unreachable code follows
    return;
  }
  case BuiltinKind::New: {
    Expr *Arg = S->getArgs()[0];
    Type *Pointee = llvm::cast<PointerType>(Arg->getType())->getPointee();
    llvm::Value *Mem = B.CreateCall(RT.New, {B.getInt64(sizeOf(Pointee))});
    emitFileLifetime(Mem, Pointee, /*Init=*/true);
    B.CreateStore(Mem, emitAddress(Arg));
    return;
  }
  case BuiltinKind::Dispose: {
    Expr *Arg = S->getArgs()[0];
    llvm::Value *P = visitExpr(Arg);
    Type *Pointee = llvm::cast<PointerType>(Arg->getType())->getPointee();
    if (containsFile(Pointee)) {
      if (rangeChecks(Arg->getBeginLoc()))
        emitRuntimeCheck(B.CreateIsNull(P), "'dispose' of a nil pointer",
                         Arg->getBeginLoc());
      emitFileLifetime(P, Pointee, /*Init=*/false);
    }
    if (isManaged(Pointee)) {
      if (rangeChecks(Arg->getBeginLoc()))
        emitRuntimeCheck(B.CreateIsNull(P), "'dispose' of a nil pointer",
                         Arg->getBeginLoc());
      finalizeValue(P, Pointee);
    }
    B.CreateCall(RT.Dispose, {P});
    return;
  }
  default:
    llvm_unreachable("Sema only allows procedures here");
  }
}


void CodeGenModule::visitIndirectCallStmt(IndirectCallStmt *S){
  visitIndirectCallExpr(S->getCall());
}


void CodeGenModule::visitIfStmt(IfStmt *S){
  bool Tail = AtRoutineTail;
  AtRoutineTail = false;
  llvm::Value *Cond = visitExpr(S->getCond());
  auto *ThenBB = newBlock("if.then");
  auto *EndBB = newBlock("if.end");
  auto *ElseBB = S->getElse() ? newBlock("if.else") : EndBB;
  B.CreateCondBr(Cond, ThenBB, ElseBB);

  bool TrackRanges = !RangeLocals.empty();
  RangeState Saved;
  if (TrackRanges) {
    Saved = saveRangeState();
    llvm::SmallVector<LengthBound, 4> Bounds;
    llvm::SmallVector<ConstBound, 4> CBounds;
    collectLengthBounds(S->getCond(), Bounds);
    collectConstBounds(S->getCond(), CBounds);
    applyLengthBounds(Bounds);
    applyConstBounds(CBounds);
  }
  B.SetInsertPoint(ThenBB);
  AtRoutineTail = Tail;
  visitStmt(S->getThen());
  AtRoutineTail = false;
  B.CreateBr(EndBB);
  if (TrackRanges)
    restoreRangeState(Saved);

  if (S->getElse()) {
    B.SetInsertPoint(ElseBB);
    AtRoutineTail = Tail;
    visitStmt(S->getElse());
    AtRoutineTail = false;
    B.CreateBr(EndBB);
    if (TrackRanges)
      restoreRangeState(Saved);
  }
  B.SetInsertPoint(EndBB);
}


void CodeGenModule::visitWhileStmt(WhileStmt *S){
  struct MonotonicVar {
    VarDecl *V;
    ValueRange Range;
    bool HadOld = false;
    ValueRange OldLoop;
  };
  llvm::SmallVector<MonotonicVar, 2> Monotonic;
  VarSet Written;
  llvm::SmallVector<LengthBound, 4> AfterSetupBounds;
  llvm::DenseMap<VarDecl *, ValueRange> PreCondRanges;
  bool TrackRanges = !RangeLocals.empty();
  if (TrackRanges) {
    Written = rangeLocalsWrittenBy(S);
    for (VarDecl *A : Written) {
      auto It = VarRanges.find(A);
      ValueRange TR;
      if (It == VarRanges.end() || !It->second.Known || !A->getType() ||
          !signedRange(intBits(A->getType()), TR) ||
          exprWritesVar(S->getCond(), A))
        continue;
      int64_t ModM = 0;
      ValueRange LoopR;
      if (monotonicNonNegSteps(S->getBody(), A))
        LoopR = {true, It->second.Lo, TR.Hi};
      else if (It->second.Lo >= 0 &&
               nonNegModLoop(S->getBody(), A, It->second.Hi, &Written, ModM))
        LoopR = {true, 0, ModM};
      else
        continue;
      MonotonicVar MV;
      MV.V = A;
      MV.Range = LoopR;
      auto LIt = LoopRanges.find(A);
      if (LIt != LoopRanges.end()) {
        MV.HadOld = true;
        MV.OldLoop = LIt->second;
      }
      Monotonic.push_back(MV);
    }
    forgetRanges(Written);
    for (const MonotonicVar &MV : Monotonic) {
      LoopRanges[MV.V] = MV.Range;
      setVarRange(MV.V, MV.Range);
    }
    AfterSetupBounds = ActiveLengthBounds;
    PreCondRanges = VarRanges;
  }

  auto *CondBB = newBlock("while.cond");
  auto *BodyBB = newBlock("while.body");
  auto *EndBB = newBlock("while.end");
  B.CreateBr(CondBB);

  B.SetInsertPoint(CondBB);
  B.CreateCondBr(visitExpr(S->getCond()), BodyBB, EndBB);

  if (TrackRanges) {
    llvm::SmallVector<LengthBound, 4> Bounds;
    llvm::SmallVector<ConstBound, 4> CBounds;
    collectLengthBounds(S->getCond(), Bounds);
    collectConstBounds(S->getCond(), CBounds);
    applyLengthBounds(Bounds);
    applyConstBounds(CBounds);
  }
  B.SetInsertPoint(BodyBB);
  Loops.push_back({CondBB, EndBB, Cleanups.size()});
  visitStmt(S->getBody());
  Loops.pop_back();
  B.CreateBr(CondBB);

  if (TrackRanges) {
    VarRanges = PreCondRanges;
    for (const MonotonicVar &MV : Monotonic) {
      if (MV.HadOld)
        LoopRanges[MV.V] = MV.OldLoop;
      else
        LoopRanges.erase(MV.V);
    }
    ActiveLengthBounds = AfterSetupBounds;
    forgetRanges(Written);
    for (const MonotonicVar &MV : Monotonic)
      setVarRange(MV.V, MV.Range);
  }
  B.SetInsertPoint(EndBB);
}


void CodeGenModule::visitRepeatStmt(RepeatStmt *S){
  auto *BodyBB = newBlock("repeat.body");
  auto *CondBB = newBlock("repeat.cond");
  auto *EndBB = newBlock("repeat.end");
  B.CreateBr(BodyBB);

  B.SetInsertPoint(BodyBB);
  Loops.push_back({CondBB, EndBB, Cleanups.size()});
  emitSequence(S->getBody());
  Loops.pop_back();
  B.CreateBr(CondBB);
  B.SetInsertPoint(CondBB);
  B.CreateCondBr(visitExpr(S->getCond()), EndBB, BodyBB);

  B.SetInsertPoint(EndBB);
}


//   start = Init; final = Final            (each evaluated once)
//   if !(start <= final) goto end          (>= for downto)
//   [range-check start and final against the control variable's type]
// loop:
//   i = phi [start, pre], [next, inc]; Var := i
//   Body
//   if i == final goto end                 (test before stepping: no
// inc:                                      overflow at maxint)
//   next = i + 1 (- 1); goto loop
void CodeGenModule::visitForStmt(ForStmt *S){
  auto *V = llvm::cast<VarDecl>(S->getVar()->getDecl());
  Type *VarTy = V->getType();
  bool Signed = isSignedOrdinal(VarTy);
  bool Down = S->isDownto();

  // Variables of RangeLocals the loop writes: their ranges are forgotten
  // in it and after it, except for accumulators (x := x + c), which stay
  // within their start value plus the most the loop can add.
  struct Accumulator {
    VarDecl *V;
    ValueRange Start;
    int64_t DLo, DHi;
  };
  llvm::SmallVector<Accumulator, 2> Accs;
  VarSet Written;
  if (!RangeLocals.empty()) {
    Written = rangeLocalsWrittenBy(S);
    for (VarDecl *A : Written) {
      auto It = VarRanges.find(A);
      int64_t DLo = 0, DHi = 0;
      if (It != VarRanges.end() &&
          accumulatorSteps(S->getBody(), A, DLo, DHi))
        Accs.push_back({A, It->second, DLo, DHi});
    }
  }

  llvm::Value *Start = visitExpr(S->getInit());
  llvm::Value *Final = visitExpr(S->getFinal());
  // The bounds may have other integer types than the variable: compare
  // them as i64, check them, then convert them to the variable's type.
  llvm::Value *Start64 = toI64(Start, S->getInit()->getType());
  llvm::Value *Final64 = toI64(Final, S->getFinal()->getType());
  // (a qword variable: its bounds compare unsigned)
  llvm::Value *Enter =
      isQWord(VarTy)
          ? (Down ? B.CreateICmpUGE(Start64, Final64)
                  : B.CreateICmpULE(Start64, Final64))
          : (Down ? B.CreateICmpSGE(Start64, Final64)
                  : B.CreateICmpSLE(Start64, Final64));
  (void)Signed;
  auto *CheckBB = newBlock("for.check");
  auto *LoopBB = newBlock("for.body");
  auto *LatchBB = newBlock("for.latch");
  auto *IncBB = newBlock("for.inc");
  auto *EndBB = newBlock("for.end");
  B.CreateCondBr(Enter, CheckBB, EndBB);

  // ISO 6.8.3.9: if the loop runs, both bounds must be assignment
  // compatible with the control variable.
  B.SetInsertPoint(CheckBB);
  emitRangeCheck(Start, S->getInit()->getType(), VarTy,
                 S->getInit()->getBeginLoc(), "'for' loop bound out of range");
  emitRangeCheck(Final, S->getFinal()->getType(), VarTy,
                 S->getFinal()->getBeginLoc(), "'for' loop bound out of range");
  Start = convertOrdinal(Start, S->getInit()->getType(), VarTy);
  Final = convertOrdinal(Final, S->getFinal()->getType(), VarTy);
  // LB-5 (localdocs/llvm-23.md): the hoisted length checks of the counted
  // bases, on the path where the loop is entered (Start ≤ Last). The
  // block for the loop's incoming phi is captured after (the checks
  // split the block).
  llvm::SmallVector<std::pair<VarDecl *, Expr *>, 2> Hoists =
      hoistableBases(S);
  bool Hoisted = !Hoists.empty();
  if (Hoisted) {
    bool WantChecks = rangeChecks(S->getInit()->getBeginLoc());
    llvm::SmallVector<llvm::Value *, 2> Datas;
    for (auto &[Base, BaseNode] : Hoists)
      Datas.push_back(Base->getType()->isOpenArray() ? emitAddress(BaseNode)
                                                     : visitExpr(BaseNode));
    if (WantChecks) {
      ValueRange StartRange = rangeOfValue(Start),
                 FinalRange = rangeOfValue(Final);
      llvm::Value *Last64 = toI64(Final, VarTy);
      llvm::Value *Start64v = toI64(Start, VarTy);
      bool QWord = isQWord(VarTy);
      bool StartGe0 = QWord || (StartRange.Known && StartRange.Lo >= 0);
      bool FinalGe0 = QWord || (FinalRange.Known && FinalRange.Lo >= 0);
      for (unsigned HI = 0; HI < Hoists.size(); ++HI) {
        auto &[Base, BaseNode] = Hoists[HI];
        llvm::Value *Data = Datas[HI];
        Type *BT = Base->getType();
        llvm::Value *Count =
            BT->isOpenArray()
                ? openCount(openDescriptor(BaseNode))
                : astrCall("__pas_dyn_length", B.getInt64Ty(), {Data});
        if (Down) {
          if (!FinalGe0)
            emitRuntimeCheck(B.CreateICmpSLT(Last64, B.getInt64(0)),
                             "array index out of bounds",
                             S->getInit()->getBeginLoc());
          emitRuntimeCheck(B.CreateICmpUGE(Start64v, Count),
                           "array index out of bounds",
                           S->getFinal()->getBeginLoc());
        } else {
          if (!StartGe0)
            emitRuntimeCheck(B.CreateICmpSLT(Start64v, B.getInt64(0)),
                             "array index out of bounds",
                             S->getInit()->getBeginLoc());
          emitRuntimeCheck(B.CreateICmpUGE(Last64, Count),
                           "array index out of bounds",
                           S->getFinal()->getBeginLoc());
        }
      }
    }
    for (unsigned HI = 0; HI < Hoists.size(); ++HI)
      CheckedBases.push_back({Hoists[HI].first, Datas[HI]});
  }
  B.CreateBr(LoopBB);

  B.SetInsertPoint(LoopBB);
  llvm::PHINode *I = B.CreatePHI(Start->getType(), 2, V->getName().Name);
  I->addIncoming(Start, B.GetInsertBlock()->getSinglePredecessor());
  storeScalar(varAddress(V), I, VarTy,
              !(V->isAliased() || V->getAbsolute() != nullptr ||
                (V->getAbsoluteRoot() && V->getAbsoluteRoot()->isAliased())));
  auto OldRange = VarRanges.find(V);
  bool HadRange = OldRange != VarRanges.end();
  ValueRange Saved = HadRange ? OldRange->second : ValueRange{};
  ValueRange StartRange = rangeOfValue(Start), FinalRange = rangeOfValue(Final);
  if (isSignedOrdinal(VarTy) && StartRange.Known && FinalRange.Known)
    VarRanges[V] = {true, std::min(StartRange.Lo, FinalRange.Lo),
                    std::max(StartRange.Hi, FinalRange.Hi)};
  else
    VarRanges.erase(V);
  // At most Trips iterations: an accumulator stays within its start
  // range plus Trips times the most one iteration adds (or subtracts),
  // at every point of the loop and after it. Only a bound that fits its
  // type counts (so that no step can have wrapped), computed in int64
  // (an overflow: no bound).
  llvm::SmallVector<std::pair<VarDecl *, ValueRange>, 2> Bounds;
  llvm::SmallVector<std::pair<VarDecl *, std::optional<ValueRange>>, 2> OldLoop;
  if (!Accs.empty() && isSignedOrdinal(VarTy) && StartRange.Known &&
      FinalRange.Known) {
    int64_t Trips;
    bool Overflow =
        Down ? __builtin_sub_overflow(StartRange.Hi, FinalRange.Lo, &Trips)
             : __builtin_sub_overflow(FinalRange.Hi, StartRange.Lo, &Trips);
    Overflow = Overflow || __builtin_add_overflow(Trips, 1, &Trips);
    if (!Overflow && Trips < 0)
      Trips = 0;
    for (const Accumulator &A : Accs) {
      ValueRange TypeRange;
      int64_t Lo, Hi;
      if (Overflow ||
          !signedRange(regType(A.V->getType())->getIntegerBitWidth(), TypeRange) ||
          __builtin_mul_overflow(Trips, A.DLo, &Lo) ||
          __builtin_add_overflow(A.Start.Lo, Lo, &Lo) ||
          __builtin_mul_overflow(Trips, A.DHi, &Hi) ||
          __builtin_add_overflow(A.Start.Hi, Hi, &Hi) ||
          Lo < TypeRange.Lo || Hi > TypeRange.Hi)
        continue;
      Bounds.push_back({A.V, {true, Lo, Hi}});
    }
  }
  for (VarDecl *A : Written) {
    auto It = VarRanges.find(A);
    int64_t ModM = 0;
    if (It != VarRanges.end() && It->second.Known && It->second.Lo >= 0 &&
        nonNegModLoop(S->getBody(), A, It->second.Hi, &Written, ModM)) {
      bool Covered = false;
      for (auto &[BVar, BR] : Bounds)
        if (BVar == A) {
          Covered = true;
          break;
        }
      if (!Covered)
        Bounds.push_back({A, {true, 0, ModM}});
    }
  }
  forgetRanges(Written);
  for (auto &[A, R] : Bounds) {
    auto It = LoopRanges.find(A);
    OldLoop.push_back({A, It != LoopRanges.end()
                              ? std::optional<ValueRange>(It->second)
                              : std::nullopt});
    LoopRanges[A] = R;
    VarRanges[A] = R;
  }
  Loops.push_back({LatchBB, EndBB, Cleanups.size()});
  visitStmt(S->getBody());
  if (Hoisted)
    for (unsigned K = 0; K < Hoists.size(); ++K)
      CheckedBases.pop_back(); // (the loop's own, innermost last)
  Loops.pop_back();
  if (HadRange)
    VarRanges[V] = Saved;
  else
    VarRanges.erase(V);
  for (auto &[A, Old] : OldLoop) {
    if (Old)
      LoopRanges[A] = *Old;
    else
      LoopRanges.erase(A);
  }
  forgetRanges(Written);
  for (auto &[A, R] : Bounds)
    VarRanges[A] = R;
  B.CreateBr(LatchBB);
  B.SetInsertPoint(LatchBB);
  B.CreateCondBr(B.CreateICmpEQ(I, Final, "for.done"), EndBB, IncBB);

  B.SetInsertPoint(IncBB);
  llvm::Value *One = llvm::ConstantInt::get(I->getType(), 1);
  llvm::Value *Next = Down ? B.CreateSub(I, One, "for.next")
                           : B.CreateAdd(I, One, "for.next");
  I->addIncoming(Next, IncBB);
  B.CreateBr(LoopBB);

  B.SetInsertPoint(EndBB);
}


//   [hold the collection] lo, hi = its first and last index (i64)
//   if lo > hi goto end
// loop:
//   i = phi [lo, pre], [next, inc]
//   [bind the element variable to element i]; Var := element; Body
// latch:                                    (continue)
//   if i == hi goto end                     (test before stepping)
// inc:
//   next = i + 1; goto loop
// An enumerator: e := Coll.GetEnumerator; while e.MoveNext do begin
// Var := e.Current; Body end, a class's e freed in a finally part.
void CodeGenModule::visitForInStmt(ForInStmt *S){
  Expr *C = S->getCollection();
  Type *Elem = S->getElementType();
  auto Iteration = [&](llvm::Value *ElemAddr) {
    if (VarDecl *EV = S->getElementVar())
      HandlerVars[EV] = ElemAddr;
    if (S->getStep())
      visitStmt(S->getStep());
    visitStmt(S->getBody());
  };
  auto Indexed = [&](llvm::Value *Lo, llvm::Value *Hi, llvm::Type *ElemTy,
                     llvm::Value *Base, bool ClampHeapAlign = false) {
    emitForInLoop(Lo, Hi, [&](llvm::Value *I, llvm::BasicBlock *) {
      llvm::Value *ElemAddr = B.CreateInBoundsGEP(ElemTy, Base, I, "in.elem");
      if (ClampHeapAlign && alignOf(Elem) > 16)
        AddrAligns[ElemAddr] = 16;
      Iteration(ElemAddr);
    });
  };
  switch (S->getInKind()) {
  case ForInStmt::Invalid:
    return; // (errors were reported)
  case ForInStmt::Enumerator:
    emitForInEnumerator(S);
    return;
  case ForInStmt::Iterator: {
    // Held during the loop (and released after it, which stops it if it
    // has more values: its finally parts run).
    llvm::PointerType *Ptr = B.getPtrTy();
    llvm::Value *It = visitExpr(C);
    B.CreateCall(classRuntime("__pas_gen_addref", B.getVoidTy(), {Ptr}), {It});
    holdResult(It, C->getType());
    auto *CondBB = newBlock("forin.cond");
    auto *BodyBB = newBlock("forin.body");
    auto *EndBB = newBlock("forin.end");
    B.CreateBr(CondBB);
    B.SetInsertPoint(CondBB);
    B.CreateCondBr(emitIteratorNext(It), BodyBB, EndBB);
    B.SetInsertPoint(BodyBB);
    Loops.push_back({CondBB, EndBB, Cleanups.size()});
    Iteration(iteratorValueAddr(It));
    Loops.pop_back();
    B.CreateBr(CondBB);
    B.SetInsertPoint(EndBB);
    return;
  }
  case ForInStmt::Array: {
    auto *AT = llvm::cast<ArrayType>(C->getType());
    llvm::Value *Base = emitAddress(C);
    OrdinalBounds Bd = getOrdinalBounds(AT->getIndexType());
    if (AT->hasBitElements()) { // each element's bits, copied
      Type *ET = AT->getElementType();
      llvm::Value *Tmp = createTemp(memType(ET), "in.bits");
      emitForInLoop(B.getInt64(0), B.getInt64(Bd.Hi - Bd.Lo),
                    [&](llvm::Value *I, llvm::BasicBlock *) {
                      llvm::Value *Bit = B.CreateMul(
                          I, B.getInt64(bitPackedWidth(ET)), "bit");
                      storeScalar(Tmp, bitLoad({Base, Bit, 0, sizeOf(AT), ET}), ET);
                      Iteration(Tmp);
                    });
      return;
    }
    emitForInLoop(B.getInt64(0), B.getInt64(Bd.Hi - Bd.Lo),
                  [&](llvm::Value *I, llvm::BasicBlock *) {
                    Iteration(B.CreateInBoundsGEP(memType(AT), Base,
                                                  {B.getInt64(0), I}, "in.elem"));
                  });
    return;
  }
  case ForInStmt::OpenArray: {
    llvm::Value *Desc = openDescriptor(C);
    llvm::Value *Data = B.CreateLoad(B.getPtrTy(), Desc, "data");
    llvm::Value *Hi = B.CreateSub(openCount(Desc), B.getInt64(1), "in.hi");
    Indexed(B.getInt64(0), Hi, memType(Elem), Data, true);
    return;
  }
  case ForInStmt::DynArray:
  case ForInStmt::AnsiString: {
    // A reference is held during the loop: changing the variable does
    // not change the elements enumerated, as in Free Pascal.
    bool Dyn = S->getInKind() == ForInStmt::DynArray;
    llvm::Value *V = visitExpr(C);
    astrCall(Dyn ? "__pas_dyn_addref" : "__pas_astr_addref", B.getVoidTy(), {V});
    holdResult(V, C->getType());
    llvm::Value *N =
        Dyn ? dynLength(V)
            : B.CreateZExt(astrCall("__pas_astr_length", B.getInt32Ty(), {V}),
                           B.getInt64Ty());
    Indexed(B.getInt64(0), B.CreateSub(N, B.getInt64(1), "in.hi"),
            memType(Elem), V, Dyn);
    return;
  }
  case ForInStmt::ShortString: {
    llvm::Value *Base = emitPStr(C); // its characters are 1..length
    llvm::Value *N = B.CreateZExt(B.CreateLoad(B.getInt8Ty(), Base, "length"),
                                  B.getInt64Ty());
    Indexed(B.getInt64(1), N, B.getInt8Ty(), Base);
    return;
  }
  case ForInStmt::StringConst:
    Indexed(B.getInt64(0),
            B.getInt64(static_cast<int64_t>(C->getType()->getStringLength()) - 1),
            B.getInt8Ty(), emitAddress(C));
    return;
  case ForInStmt::Set: {
    // The members in order: the set is evaluated once.
    llvm::Value *V = visitExpr(C);
    int64_t Lo = 0, Hi = -1; // [] has none
    if (Elem) {
      OrdinalBounds Bd = getOrdinalBounds(Elem);
      Lo = std::max<int64_t>(Bd.Lo, 0);
      Hi = std::min<int64_t>(Bd.Hi, setBits(C->getType()) - 1);
    }
    llvm::Value *Slot = Elem ? createTemp(memType(Elem), "in.elem") : nullptr;
    emitForInLoop(B.getInt64(Lo), B.getInt64(Hi),
                  [&](llvm::Value *I, llvm::BasicBlock *Latch) {
                    llvm::Value *Bit = B.CreateTrunc(
                        B.CreateLShr(V, B.CreateZExtOrTrunc(I, V->getType())),
                        B.getInt1Ty(), "in.member");
                    auto *Member = newBlock("forin.member");
                    B.CreateCondBr(Bit, Member, Latch);
                    B.SetInsertPoint(Member);
                    if (Slot)
                      storeScalar(Slot, B.CreateTrunc(I, regType(Elem)), Elem);
                    Iteration(Slot);
                  });
    return;
  }
  case ForInStmt::Ordinals: {
    OrdinalBounds Bd = getOrdinalBounds(Elem);
    llvm::Value *Slot = createTemp(memType(Elem), "in.elem");
    emitForInLoop(B.getInt64(Bd.Lo), B.getInt64(Bd.Hi),
                  [&](llvm::Value *I, llvm::BasicBlock *) {
                    storeScalar(Slot, B.CreateTrunc(I, regType(Elem)), Elem);
                    Iteration(Slot);
                  });
    return;
  }
  }
}


/// for I := Lo to Hi (i64, signed; nothing if Lo > Hi), with break and
/// continue: Fn(I, latch) emits an iteration, which may branch to the
/// latch to skip the rest of it.
void CodeGenModule::emitForInLoop(
    llvm::Value *Lo, llvm::Value *Hi,
    llvm::function_ref<void(llvm::Value *, llvm::BasicBlock *)> Fn){
  auto *LoopBB = newBlock("forin.body");
  auto *LatchBB = newBlock("forin.latch");
  auto *IncBB = newBlock("forin.inc");
  auto *EndBB = newBlock("forin.end");
  B.CreateCondBr(B.CreateICmpSLE(Lo, Hi), LoopBB, EndBB);
  B.SetInsertPoint(LoopBB);
  llvm::PHINode *I = B.CreatePHI(B.getInt64Ty(), 2, "in.index");
  llvm::BasicBlock *InPreBB = B.GetInsertBlock()->getSinglePredecessor();
  I->addIncoming(Lo, InPreBB);
  Loops.push_back({LatchBB, EndBB, Cleanups.size()});
  Fn(I, LatchBB);
  Loops.pop_back();
  B.CreateBr(LatchBB);
  B.SetInsertPoint(LatchBB);
  B.CreateCondBr(B.CreateICmpEQ(I, Hi, "in.done"), EndBB, IncBB);
  B.SetInsertPoint(IncBB);
  I->addIncoming(B.CreateAdd(I, B.getInt64(1), "in.next"), IncBB);
  B.CreateBr(LoopBB);
  B.SetInsertPoint(EndBB);
}


void CodeGenModule::emitForInEnumerator(ForInStmt *S){
  Expr *C = S->getCollection();
  Type *CT = C->getType();
  if (CT->isClass() || CT->isInterface()) {
    llvm::Value *Slot = createTemp(memType(CT), "in.coll");
    B.CreateStore(visitExpr(C), Slot);
    HandlerVars[S->getCollectionVar()] = Slot;
  } else { // a record: its methods get its address
    HandlerVars[S->getCollectionVar()] = emitAddress(C);
  }
  VarDecl *EnumV = S->getEnumeratorVar();
  Type *ET = EnumV->getType();
  if (!ET->isClass() && !ET->isInterface()) {
    HandlerVars[EnumV] = emitAddress(S->getGetEnumerator());
    emitEnumeratorLoop(S);
    return;
  }
  llvm::Value *Slot = createTemp(memType(ET), "in.enum");
  B.CreateStore(visitExpr(S->getGetEnumerator()), Slot);
  HandlerVars[EnumV] = Slot;
  if (!ET->isClass()) {
    emitEnumeratorLoop(S);
    return;
  }
  emitTryFinally([&] { emitEnumeratorLoop(S); }, [&](llvm::Value *) {
    emitFree(B.CreateLoad(B.getPtrTy(), Slot, "enum"));
  });
}


//   while e.MoveNext do begin Var := e.Current; Body end
void CodeGenModule::emitEnumeratorLoop(ForInStmt *S){
  auto *CondBB = newBlock("forin.cond");
  auto *BodyBB = newBlock("forin.body");
  auto *EndBB = newBlock("forin.end");
  B.CreateBr(CondBB);
  B.SetInsertPoint(CondBB);
  B.CreateCondBr(visitExpr(S->getMoveNext()), BodyBB, EndBB);
  B.SetInsertPoint(BodyBB);
  Loops.push_back({CondBB, EndBB, Cleanups.size()});
  if (S->getStep())
    visitStmt(S->getStep());
  visitStmt(S->getBody());
  Loops.pop_back();
  B.CreateBr(CondBB);
  B.SetInsertPoint(EndBB);
}


void CodeGenModule::visitCaseStmt(CaseStmt *S){
  llvm::Value *Sel = visitExpr(S->getSelector());
  auto *SelTy = llvm::cast<llvm::IntegerType>(Sel->getType());
  bool Signed = isSignedOrdinal(S->getSelector()->getType());
  auto *DefaultBB = newBlock("case.default");
  auto *EndBB = newBlock("case.end");
  llvm::SwitchInst *Switch = B.CreateSwitch(Sel, DefaultBB);

  // Ranges (TP) are expanded into switch cases when small; large ones are
  // tested with comparisons in the default block.
  constexpr int64_t MaxExpandedRange = 256;
  struct LargeRange { int64_t Lo, Hi; llvm::BasicBlock *Target; };
  llvm::SmallVector<LargeRange, 4> LargeRanges;

  auto Const = [&](int64_t V) {
    return llvm::ConstantInt::get(SelTy, static_cast<uint64_t>(V), Signed);
  };

  for (CaseArm *Arm : S->getArms()) {
    auto *ArmBB = newBlock("case.arm");
    for (CaseLabel *L : Arm->getLabels()) {
      // (the number of values less one; unsigned for qword's labels)
      uint64_t Span = static_cast<uint64_t>(L->HiValue) -
                      static_cast<uint64_t>(L->LoValue);
      if (Span >= static_cast<uint64_t>(MaxExpandedRange)) {
        LargeRanges.push_back({L->LoValue, L->HiValue, ArmBB});
        continue;
      }
      for (uint64_t K = 0; K <= Span; ++K)
        Switch->addCase(
            Const(static_cast<int64_t>(static_cast<uint64_t>(L->LoValue) + K)),
            ArmBB);
    }
    B.SetInsertPoint(ArmBB);
    visitStmt(Arm->getBody());
    B.CreateBr(EndBB);
  }

  B.SetInsertPoint(DefaultBB);
  for (const LargeRange &R : LargeRanges) {
    llvm::Value *AboveLo =
        Signed ? B.CreateICmpSGE(Sel, Const(R.Lo)) : B.CreateICmpUGE(Sel, Const(R.Lo));
    llvm::Value *BelowHi =
        Signed ? B.CreateICmpSLE(Sel, Const(R.Hi)) : B.CreateICmpULE(Sel, Const(R.Hi));
    llvm::Value *InRange = B.CreateAnd(AboveLo, BelowHi);
    auto *NextBB = newBlock("case.range");
    B.CreateCondBr(InRange, R.Target, NextBB);
    B.SetInsertPoint(NextBB);
  }
  if (S->hasElse()) {
    emitSequence(S->getElseBody());
    B.CreateBr(EndBB);
  } else if (LangOpts.Mode == LangMode::ISO) {
    // ISO 7185 6.8.3.5: it is an error if no label equals the selector.
    emitRuntimeError("no 'case' label matches the selector value",
                     S->getBeginLoc());
  } else {
    B.CreateBr(EndBB); // Turbo/FPC: do nothing
  }

  B.SetInsertPoint(EndBB);
}


void CodeGenModule::visitLabeledStmt(LabeledStmt *S){
  llvm::BasicBlock *BB = labelBlock(S->getDecl());
  B.CreateBr(BB);
  B.SetInsertPoint(BB);
  visitStmt(S->getBody());
}


void CodeGenModule::visitGotoStmt(GotoStmt *S){
  LabelDecl *L = S->getDecl();
  if (L->getOwner() == CurRoutine) {
    B.CreateBr(labelBlock(L));
  } else {
    // Non-local: unwind to the owner's activation with _longjmp.
    RoutineDecl *Owner = L->getOwner();
    llvm::Value *Buf =
        Owner ? B.CreateStructGEP(FrameTypes[Owner], framePtrFor(Owner),
                                  JmpBufSlots[Owner])
              : static_cast<llvm::Value *>(MainJmpBuf);
    B.CreateCall(RT.LongJmp, {Buf, B.getInt32(L->getIndex())});
    B.CreateUnreachable();
  }
  B.SetInsertPoint(newBlock("after.goto")); // unreachable code follows
}


//===--------------------------------------------------------------------===//
// Exceptions (objfpc)
//===--------------------------------------------------------------------===//

/// PasExcFrame (runtime/include/pascalrt.h): prev, object, where,
/// address, jmp_buf.
llvm::StructType *CodeGenModule::excFrameType(){
  return llvm::StructType::get(B.getPtrTy(), B.getPtrTy(), B.getPtrTy(),
                               B.getPtrTy(), jmpBufType());
}


/// The runtime's exception entry points, declared when first used.
llvm::FunctionCallee CodeGenModule::excRuntime(llvm::StringRef Name){
  llvm::Type *Ptr = B.getPtrTy();
  llvm::Type *Void = B.getVoidTy();
  bool NoReturn = false;
  llvm::FunctionCallee F;
  if (Name == "__pas_raise_divzero" || Name == "__pas_raise_overflow") {
    F = classRuntime(Name, Void, {Ptr});
    llvm::cast<llvm::Function>(F.getCallee())->addFnAttr(llvm::Attribute::Cold);
    NoReturn = true;
  } else if (Name == "__pas_raise" || Name == "__pas_reraise_free") {
    F = classRuntime(Name, Void, {Ptr, Ptr});
    NoReturn = true;
  } else if (Name == "__pas_raise_at") {
    F = classRuntime(Name, Void, {Ptr, Ptr, Ptr});
    NoReturn = true;
  } else {
    // __pas_exc_pop, __pas_free_object, __pas_reraise
    F = classRuntime(Name, Void, {Ptr});
    NoReturn = Name == "__pas_reraise";
  }
  if (NoReturn)
    llvm::cast<llvm::Function>(F.getCallee())
        ->addFnAttr(llvm::Attribute::NoReturn);
  return F;
}


/// Pushes a new PasExcFrame and calls _setjmp on it: an exception raised
/// to the frame continues at Landing, the protected code in a new block
/// named BodyName, or in Body if given (the insertion point). Returns
/// the frame.
llvm::Value *CodeGenModule::emitProtect(llvm::BasicBlock *Landing, const llvm::Twine &BodyName,
                         llvm::BasicBlock *Body){
  llvm::Value *F = createTemp(excFrameType(), "excframe");
  B.CreateCall(excRuntime("__pas_exc_push"), {F});
  llvm::Value *R = B.CreateCall(
      RT.SetJmp, {B.CreateStructGEP(excFrameType(), F, 4)}, "jmp");
  if (!Body)
    Body = newBlock(BodyName);
  B.CreateCondBr(B.CreateICmpNE(R, B.getInt32(0)), Landing, Body);
  B.SetInsertPoint(Body);
  return F;
}


/// Branches to Target (exit, break, continue), leaving the Cleanups
/// above Depth on the way.
void CodeGenModule::emitJump(llvm::BasicBlock *Target, size_t Depth){
  for (size_t I = Cleanups.size(); I > Depth; --I) {
    Cleanup &C = Cleanups[I - 1];
    B.CreateCall(excRuntime("__pas_exc_pop"), {C.Frame});
    if (C.K == Cleanup::Handler) {
      B.CreateCall(excRuntime("__pas_free_object"), {C.Object});
    } else if (C.K == Cleanup::Finally) {
      B.CreateStore(B.getInt32(2 + C.Exits.size()), C.State);
      auto *Cont = newBlock("finally.cont");
      C.Exits.push_back(Cont);
      B.CreateBr(C.FinallyBB);
      B.SetInsertPoint(Cont);
    }
  }
  B.CreateBr(Target);
}


void CodeGenModule::visitTryStmt(TryStmt *S){
  if (S->isFinally()) {
    emitTryFinally(S);
    return;
  }
  //   push F; if (_setjmp(F)) goto except;  body; pop F; goto end
  // except:
  //   exc = F.object; push H; if (_setjmp(H)) goto escape
  //   if (exc is C1) { E1 := exc; handler1; pop H; free exc; goto end } ...
  //   [else part; pop H; free exc; goto end]  or  pop H; reraise F
  // escape: (an exception left a handler) free exc unless it is the one
  //   raised; reraise it
  auto *Except = newBlock("try.except");
  auto *End = newBlock("try.end");
  llvm::Value *F = emitProtect(Except, "try.body");
  Cleanups.emplace_back(Cleanup::TryBody, F);
  emitSequence(S->getBody());
  Cleanups.pop_back();
  B.CreateCall(excRuntime("__pas_exc_pop"), {F});
  B.CreateBr(End);

  B.SetInsertPoint(Except);
  llvm::Value *Obj = B.CreateLoad(
      B.getPtrTy(), B.CreateStructGEP(excFrameType(), F, 1), "exc");
  auto *Escape = newBlock("handler.raise");
  llvm::Value *H = emitProtect(Escape, "handler.match");
  Cleanups.emplace_back(Cleanup::Handler, H, Obj);
  HandledFrames.push_back(F);
  auto Done = [&] { // a handler ran to its end
    B.CreateCall(excRuntime("__pas_exc_pop"), {H});
    B.CreateCall(excRuntime("__pas_free_object"), {Obj});
    B.CreateBr(End);
  };
  for (ExceptHandler *Handler : S->getHandlers()) {
    llvm::Value *HandlerVMT = B.CreateLoad(B.getPtrTy(), Obj, "vmt");
    llvm::Value *Is = classIsVMT(HandlerVMT, Handler->getClass());
    auto *Do = newBlock("handler.do");
    auto *Next = newBlock("handler.next");
    B.CreateCondBr(Is, Do, Next);
    B.SetInsertPoint(Do);
    if (VarDecl *V = Handler->getVar()) {
      llvm::Value *Slot = createTemp(B.getPtrTy(), V->getName().Name);
      B.CreateStore(Obj, Slot);
      HandlerVars[V] = Slot;
    }
    visitStmt(Handler->getBody());
    Done();
    B.SetInsertPoint(Next);
  }
  if (S->catchesAll()) {
    emitSequence(S->getExceptBody());
    Done();
  } else { // no handler matches: on to the next try statement
    B.CreateCall(excRuntime("__pas_exc_pop"), {H});
    B.CreateCall(excRuntime("__pas_reraise"), {F});
    B.CreateUnreachable();
  }
  HandledFrames.pop_back();
  Cleanups.pop_back();

  B.SetInsertPoint(Escape);
  B.CreateCall(excRuntime("__pas_reraise_free"), {H, Obj});
  B.CreateUnreachable();
  B.SetInsertPoint(End);
}


//   push F; if (_setjmp(F)) goto raised;  body; pop F; state = 0
// finally:
//   finally part
//   switch (state) { 0: end; 1: reraise F; 2 + k: exit k (exit, break or
//     continue out of the body) }
// raised: state = 1; goto finally
void CodeGenModule::emitTryFinally(TryStmt *S){
  emitTryFinally(
      [&] { emitSequence(S->getBody()); },
      [&](llvm::Value *) { emitSequence(S->getFinallyBody()); });
}


/// The finally part of a routine with owned variables: frees their
/// objects, the last declared first. A destructor's exception does not
/// stop the others; the first one is raised afterwards, unless an
/// exception is already leaving the routine (State 1), which goes on.
void CodeGenModule::emitFreeOwned(llvm::ArrayRef<VarDecl *> Owned, llvm::Value *State){
  llvm::Type *Ptr = B.getPtrTy();
  llvm::StructType *PendingTy = llvm::StructType::get(Ptr, Ptr, Ptr);
  llvm::Value *Pending = createTemp(PendingTy, "owned.exc");
  B.CreateStore(llvm::Constant::getNullValue(PendingTy), Pending);
  for (VarDecl *V : llvm::reverse(Owned))
    B.CreateCall(classRuntime("__pas_owned_free", B.getVoidTy(), {Ptr, Ptr}),
                 {varAddress(V), Pending});
  llvm::Value *Raised = B.CreateICmpEQ(
      B.CreateLoad(B.getInt32Ty(), State, "state"), B.getInt32(1), "raised");
  B.CreateCall(classRuntime("__pas_owned_done", B.getVoidTy(),
                            {Ptr, B.getInt32Ty()}),
               {Pending, B.CreateZExt(Raised, B.getInt32Ty())});
}


/// FinallyPart is given the state's variable: 0 after the body's normal
/// end, 1 after an exception, 2 + k after an exit, break or continue.
void CodeGenModule::emitTryFinally(llvm::function_ref<void()> Body,
                    llvm::function_ref<void(llvm::Value *)> FinallyPart){
  auto *Raised = newBlock("finally.raise");
  auto *Finally = newBlock("finally");
  auto *End = newBlock("try.end");
  llvm::Value *State = createTemp(B.getInt32Ty(), "finally.state");
  llvm::Value *F = emitProtect(Raised, "try.body");
  Cleanups.emplace_back(Cleanup::Finally, F, nullptr, State, Finally);
  Body();
  llvm::SmallVector<llvm::BasicBlock *, 2> Exits =
      std::move(Cleanups.back().Exits);
  Cleanups.pop_back();
  B.CreateCall(excRuntime("__pas_exc_pop"), {F});
  B.CreateStore(B.getInt32(0), State);
  B.CreateBr(Finally);

  B.SetInsertPoint(Raised);
  B.CreateStore(B.getInt32(1), State);
  B.CreateBr(Finally);

  B.SetInsertPoint(Finally);
  FinallyPart(State);
  auto *Reraise = newBlock("finally.reraise");
  llvm::SwitchInst *SW =
      B.CreateSwitch(B.CreateLoad(B.getInt32Ty(), State, "state"), End);
  SW->addCase(B.getInt32(1), Reraise);
  for (size_t K = 0; K < Exits.size(); ++K)
    SW->addCase(B.getInt32(2 + K), Exits[K]);
  B.SetInsertPoint(Reraise);
  B.CreateCall(excRuntime("__pas_reraise"), {F});
  B.CreateUnreachable();
  B.SetInsertPoint(End);
}


/// "file:line:col" for the runtime's exception functions, or null.
llvm::Value *CodeGenModule::whereText(SourceLocation L){
  std::string Where = where(L);
  if (Where.empty())
    return llvm::ConstantPointerNull::get(B.getPtrTy());
  return B.CreateGlobalString(Where.substr(4), ".where");
}


/// if (Cond) integer division by zero: a runtime error, or (objfpc) an
/// EDivByZero exception, as in Free Pascal with SysUtils.
void CodeGenModule::emitDivZeroCheck(llvm::Value *Cond, SourceLocation L){
  if (!LangOpts.ObjKeywords) {
    emitRuntimeCheck(Cond, "division by zero", L);
    return;
  }
  if (auto *C = llvm::dyn_cast<llvm::ConstantInt>(Cond); C && C->isZero())
    return; // provably fine
  auto *ErrBB = newBlock("rt.error");
  auto *ContBB = newBlock("rt.ok");
  llvm::MDBuilder MDB(M.getContext());
  B.CreateCondBr(Cond, ErrBB, ContBB, MDB.createUnlikelyBranchWeights());
  B.SetInsertPoint(ErrBB);
  B.CreateCall(excRuntime("__pas_raise_divzero"), {whereText(L)});
  B.CreateUnreachable();
  B.SetInsertPoint(ContBB);
}


/// ExceptAddr: the address of the exception the innermost enclosing
/// handler handles (the frame it was raised to), nil outside handlers.
llvm::Value *CodeGenModule::exceptAddr(){
  if (HandledFrames.empty())
    return llvm::ConstantPointerNull::get(B.getPtrTy());
  return B.CreateLoad(
      B.getPtrTy(),
      B.CreateStructGEP(excFrameType(), HandledFrames.back(), 3),
      "exceptaddr");
}


void CodeGenModule::visitRaiseStmt(RaiseStmt *S){
  if (S->isReraise()) {
    B.CreateCall(excRuntime("__pas_reraise"), {HandledFrames.back()});
  } else if (S->getAddress()) {
    // Free Pascal's order: the frame (only evaluated), the address, the
    // object.
    if (S->getFrame())
      visitExpr(S->getFrame());
    llvm::Value *Addr = visitExpr(S->getAddress());
    llvm::Value *Obj = visitExpr(S->getObject());
    B.CreateCall(excRuntime("__pas_raise_at"),
                 {Obj, whereText(S->getBeginLoc()), Addr});
  } else {
    llvm::Value *Obj = visitExpr(S->getObject());
    B.CreateCall(excRuntime("__pas_raise"), {Obj, whereText(S->getBeginLoc())});
  }
  B.CreateUnreachable();
  B.SetInsertPoint(newBlock("after.raise")); // unreachable code follows
}


void CodeGenModule::visitWithStmt(WithStmt *S){
  // The record designators are evaluated once, on entry (ISO 6.8.3.10).
  // An object (objfpc) is evaluated, a record's address taken.
  for (WithContext *W : S->getRecords())
    WithAddrs[W] = W->Record->getType()->isClass() ? emitObject(W->Record)
                                                   : emitAddress(W->Record);
  visitStmt(S->getBody());
}


//===--------------------------------------------------------------------===//
// Expressions (scalar values in register representation)
//===--------------------------------------------------------------------===//

llvm::Value *CodeGenModule::visitIntegerLiteralExpr(IntegerLiteralExpr *E){
  llvm::Value *V =
      llvm::ConstantInt::get(regType(E->getType()), E->getValue());
  if (isSignedOrdinal(E->getType()))
    noteRange(V, {true, static_cast<int64_t>(E->getValue()),
                  static_cast<int64_t>(E->getValue())});
  return V;
}


llvm::Value *CodeGenModule::visitRealLiteralExpr(RealLiteralExpr *E){
  return llvm::ConstantFP::get(B.getDoubleTy(), E->getValue());
}


llvm::Value *CodeGenModule::visitStringLiteralExpr(StringLiteralExpr *E){
  // Longer strings are aggregates and go through emitAddress.
  if (E->isWideCharLiteral())
    return B.getInt16(static_cast<uint16_t>(E->getWideCode()));
  assert(E->isCharLiteral() && "string values are handled by address");
  return B.getInt8(static_cast<uint8_t>(E->getValue()[0]));
}


llvm::Value *CodeGenModule::visitNameRefExpr(NameRefExpr *E){
  Decl *D = E->getDecl();
  if (auto *P = llvm::dyn_cast<PropertyDecl>(D)) {
    if (P->isClassProperty())
      return emitPropertyRead(P, nullptr, nullptr);
    WithContext *W = E->getWith();
    return emitPropertyRead(P, withBase(W), W->Record->getType());
  }
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isStatic())
    return emitCall(R, {}); // a record's static method
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isMethod())
    return emitMethodCall(R, withBase(E->getWith()), {}, /*Inherited=*/false,
                          E->getBeginLoc(), withIsClass(E->getWith()));
  if (auto *TD = llvm::dyn_cast<TypeDecl>(D)) // a class as a value
    return vmtOf(llvm::cast<ClassType>(TD->getType()));
  if (E->getType()->isShortString() &&
      (llvm::isa<VarDecl>(D) || llvm::isa<FieldDecl>(D)))
    return emitAddress(E);
  if (auto *V = llvm::dyn_cast<VarDecl>(D);
      V && llvm::isa_and_nonnull<RoutineType>(V->getType())) {
    if (llvm::isa<RoutineType>(E->getType())) // the value itself (TP)
      return B.CreateLoad(memType(V->getType()), varAddress(V));
    return emitIndirectCall(V, {}); // parameterless functional parameter
  }
  if (isBitPackedComponent(E))
    return bitLoad(E);
  if (llvm::isa<VarDecl>(D) || llvm::isa<FieldDecl>(D)) {
    llvm::Value *V = loadScalar(emitAddress(E), E->getType(), E->getName().Name,
                                !isAliasedExpr(E));
    if (auto *VD = llvm::dyn_cast<VarDecl>(D)) {
      auto It = VarRanges.find(VD);
      if (It != VarRanges.end())
        noteRange(V, It->second);
    }
    return V;
  }
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D))
    return emitCall(R, {}); // parameterless function
  if (auto *BD = llvm::dyn_cast<BuiltinDecl>(D)) {
    if (BD->getBuiltinKind() == BuiltinKind::ParamCount)
      return B.CreateCall(RT.ParamCount, {}, "paramcount");
    if (BD->getBuiltinKind() == BuiltinKind::ExceptAddr)
      return exceptAddr();
    return emitFileQuery(BD->getBuiltinKind(), nullptr); // eof / eoln
  }
  auto *C = llvm::cast<ConstDecl>(D);
  const ConstValue &V = C->getValue();
  if (V.K == ConstValue::Real)
    return llvm::ConstantFP::get(B.getDoubleTy(), V.RealVal);
  llvm::Value *R = llvm::ConstantInt::get(regType(C->getType()),
                                          static_cast<uint64_t>(V.Int), true);
  if (isSignedOrdinal(C->getType()))
    noteRange(R, {true, V.Int, V.Int});
  return R;
}


/// move p: p's object; p is nil afterwards.
llvm::Value *CodeGenModule::visitMoveExpr(MoveExpr *E){
  llvm::Value *Slot = emitAddress(E->getOperand());
  llvm::Value *Obj = B.CreateLoad(B.getPtrTy(), Slot, "moved");
  B.CreateStore(llvm::ConstantPointerNull::get(B.getPtrTy()), Slot);
  if (E->getType()->isDynArray()) // (its reference, held to the statement's end)
    return holdResult(Obj, E->getType());
  return Obj;
}


llvm::Value *CodeGenModule::visitUnaryExpr(UnaryExpr *E){
  if (RoutineDecl *Op = E->getOperator()) // a record's class operator
    return emitCall(Op, {E->getOperand()});
  llvm::Value *V = visitExpr(E->getOperand());
  Type *T = E->getType();
  if (T->isIntegerLike()) // promoted operand (byte -> integer, ...)
    V = convertOrdinal(V, E->getOperand()->getType(), T);
  switch (E->getOp()) {
  case UnaryOp::Plus:
    return V;
  case UnaryOp::Minus:
    if (T->isReal())
      return B.CreateFNeg(V, "neg");
    return emitCheckedArith(BinaryOp::Sub, isSignedOrdinal(T), intConst(V, 0),
                            V, E->getOpLoc(), "neg");
  case UnaryOp::Not:
    // a vector of booleans: lanes of 0 and 1
    if (auto *VT = llvm::dyn_cast<VectorType>(T);
        VT && VT->getElementType()->getBase()->isBoolean())
      return B.CreateXor(V, llvm::ConstantInt::get(V->getType(), 1), "not");
    return B.CreateNot(V, "not");
  }
  llvm_unreachable("bad unary op");
}


/// vector[N] of T: a comparison lane by lane (a vector of booleans, lanes
/// of 0 and 1), or and/or/xor of the lanes; a scalar operand is
/// broadcast to every lane.
llvm::Value *CodeGenModule::emitVectorLanes(BinaryExpr *E){
  Type *LT = E->getLHS()->getType(), *RTy = E->getRHS()->getType();
  auto *OT = llvm::cast<VectorType>(LT->isVector() ? LT : RTy);
  Type *ET = OT->getElementType();
  llvm::Type *LaneTy = memType(ET);
  auto Operand = [&](Expr *X) -> llvm::Value * {
    llvm::Value *V = visitExpr(X);
    if (X->getType()->isVector())
      return V;
    if (ET->isReal())
      V = toDouble(V, X->getType());
    else
      V = B.CreateZExtOrTrunc(convertOrdinal(V, X->getType(), ET), LaneTy);
    return B.CreateVectorSplat(OT->getCount(), V, "splat");
  };
  llvm::Value *L = Operand(E->getLHS());
  llvm::Value *R = Operand(E->getRHS());
  BinaryOp Op = E->getOp();
  switch (Op) {
  case BinaryOp::And: return B.CreateAnd(L, R, "ve.and");
  case BinaryOp::Or: return B.CreateOr(L, R, "ve.or");
  case BinaryOp::Xor: return B.CreateXor(L, R, "ve.xor");
  default: break;
  }
  llvm::Value *C;
  if (ET->isReal()) {
    llvm::CmpInst::Predicate P =
        Op == BinaryOp::Eq ? llvm::CmpInst::FCMP_OEQ
        : Op == BinaryOp::Ne ? llvm::CmpInst::FCMP_UNE
        : Op == BinaryOp::Lt ? llvm::CmpInst::FCMP_OLT
        : Op == BinaryOp::Le ? llvm::CmpInst::FCMP_OLE
        : Op == BinaryOp::Gt ? llvm::CmpInst::FCMP_OGT
                             : llvm::CmpInst::FCMP_OGE;
    C = B.CreateFCmp(P, L, R, "ve.cmp");
  } else {
    bool S = isSignedOrdinal(ET);
    llvm::CmpInst::Predicate P =
        Op == BinaryOp::Eq ? llvm::CmpInst::ICMP_EQ
        : Op == BinaryOp::Ne ? llvm::CmpInst::ICMP_NE
        : Op == BinaryOp::Lt ? (S ? llvm::CmpInst::ICMP_SLT : llvm::CmpInst::ICMP_ULT)
        : Op == BinaryOp::Le ? (S ? llvm::CmpInst::ICMP_SLE : llvm::CmpInst::ICMP_ULE)
        : Op == BinaryOp::Gt ? (S ? llvm::CmpInst::ICMP_SGT : llvm::CmpInst::ICMP_UGT)
                             : (S ? llvm::CmpInst::ICMP_SGE : llvm::CmpInst::ICMP_UGE);
    C = B.CreateICmp(P, L, R, "ve.cmp");
  }
  return B.CreateZExt(C, memType(E->getType()), "ve.mask");
}


llvm::Value *CodeGenModule::visitBinaryExpr(BinaryExpr *E){
  if (NameRefExpr *Ref = E->getConstRef()) // 'a' + 'b': a constant
    return visitExpr(Ref);
  BinaryOp Op = E->getOp();
  if (RoutineDecl *ClassOp = E->getOperator()) { // a record's operator
    llvm::Value *V = emitCall(ClassOp, {E->getLHS(), E->getRHS()});
    return E->isNegated() ? B.CreateNot(V, "ne") : V;
  }
  if (Op == BinaryOp::Is || Op == BinaryOp::As)
    return emitIsAs(E);
  if ((Op == BinaryOp::And || Op == BinaryOp::Or) && E->getType()->isBoolean())
    return emitShortCircuit(E);
  if (E->getType()->isAnsiString()) {
    // (a + b) + c, the characters of all three joined in one allocation
    // (optimization3.md item 7): __pas_astr_join reads its pieces before
    // the fresh buffer is written, so no piece can be it. The middle
    // piece may be a character: an own byte, instead of a fresh string.
    bool NoHold = NoHoldAnsiResult;
    NoHoldAnsiResult = false;
    if (auto *BLHS = llvm::dyn_cast<BinaryExpr>(E->getLHS());
        BLHS && BLHS->getOp() == BinaryOp::Add &&
        BLHS->getType()->isAnsiString() &&
        (BLHS->getRHS()->getType()->isAnsiString() ||
         BLHS->getRHS()->getType()->isCharLike())) {
      llvm::Value *A = emitAnsi(BLHS->getLHS());
      llvm::Value *M = nullptr, *N = nullptr;
      if (BLHS->getRHS()->getType()->isAnsiString()) {
        M = emitAnsi(BLHS->getRHS());
        N = B.CreateZExt(astrCall("__pas_astr_length", B.getInt32Ty(), {M}),
                         B.getInt64Ty());
      } else {
        M = emitCharPiece(BLHS->getRHS());
        N = B.getInt64(1);
      }
      llvm::Value *C = emitAnsi(E->getRHS());
      return takeOrHoldAnsi(
          astrCall("__pas_astr_join", B.getPtrTy(), {A, M, N, C}), NoHold);
    }
    // c + s and s + c, with the character on its own byte and the
    // string's or the empty end's characters as the nil side of the
    // join (item 7): one allocation, where a fresh one-character
    // string and a concat were two.
    if (E->getLHS()->getType()->isCharLike() &&
        E->getRHS()->getType()->isAnsiString()) {
      llvm::Value *M = emitCharPiece(E->getLHS());
      llvm::Value *C = emitAnsi(E->getRHS());
      return takeOrHoldAnsi(
          astrCall("__pas_astr_join", B.getPtrTy(),
                   {llvm::ConstantPointerNull::get(B.getPtrTy()), M,
                    B.getInt64(1), C}),
          NoHold);
    }
    if (E->getLHS()->getType()->isAnsiString() &&
        E->getRHS()->getType()->isCharLike()) {
      llvm::Value *L = emitAnsi(E->getLHS());
      llvm::Value *M = emitCharPiece(E->getRHS());
      return takeOrHoldAnsi(
          astrCall("__pas_astr_join", B.getPtrTy(),
                   {L, M, B.getInt64(1),
                    llvm::ConstantPointerNull::get(B.getPtrTy())}),
          NoHold);
    }
    llvm::Value *L = emitAnsi(E->getLHS());
    llvm::Value *R = emitAnsi(E->getRHS());
    return takeOrHoldAnsi(astrCall("__pas_astr_concat", B.getPtrTy(), {L, R}),
                          NoHold);
  }
  if (E->getType()->isShortString()) { // Turbo/FPC concatenation
    llvm::Value *Tmp = newStringTemp();
    B.CreateCall(RT.StrConcat,
                 {Tmp, emitPStr(E->getLHS()), emitPStr(E->getRHS())});
    return Tmp;
  }
  if (E->getType()->isVector() &&
      (isRelational(Op) || Op == BinaryOp::And || Op == BinaryOp::Or ||
       Op == BinaryOp::Xor))
    return emitVectorLanes(E);
  if (isRelational(Op))
    return emitCompare(E);

  Type *LT = E->getLHS()->getType(), *RTy = E->getRHS()->getType();
  llvm::Value *L = visitExpr(E->getLHS());
  llvm::Value *R = visitExpr(E->getRHS());

  // Turbo/FPC pointer arithmetic, in units of what is pointed to.
  if (auto *PT = llvm::dyn_cast<PointerType>(E->getType())) {
    if (!LT->isPointer()) { // n + p
      llvm::Value *N = toI64(L, LT);
      if (pointerMathChecked(E->getOpLoc()))
        emitPointerAddCheck(PT, R, N, E->getOpLoc());
      return B.CreateGEP(pointeeMemType(PT), R, N, "ptr.add");
    }
    llvm::Value *N = toI64(R, RTy);
    if (Op == BinaryOp::Sub)
      N = B.CreateNeg(N);
    if (pointerMathChecked(E->getOpLoc()))
      emitPointerAddCheck(PT, L, N, E->getOpLoc());
    return B.CreateGEP(pointeeMemType(PT), L, N, "ptr.add");
  }
  if (auto *PT = llvm::dyn_cast<PointerType>(LT)) { // p - q
    // (instructions even for constant addresses: the self-hosted compiler
    // writes the same)
    if (pointerMathChecked(E->getOpLoc()))
      emitPointerDiffCheck(L, R, E->getOpLoc());
    llvm::Value *LI = B.Insert(llvm::CastInst::Create(
        llvm::Instruction::PtrToInt, L, B.getInt64Ty()));
    llvm::Value *RI = B.Insert(llvm::CastInst::Create(
        llvm::Instruction::PtrToInt, R, B.getInt64Ty()));
    llvm::Value *Diff = B.CreateSub(LI, RI, "ptr.diff");
    uint64_t Size = DL.getTypeAllocSize(pointeeMemType(PT));
    return Size > 1 ? B.CreateSDiv(Diff, B.getInt64(Size), "ptr.count") : Diff;
  }

  if (E->getType()->isSet()) {
    // (the result has the larger of the two representations)
    L = convertSet(L, LT, E->getType());
    R = convertSet(R, RTy, E->getType());
    switch (Op) {
    case BinaryOp::Add: return B.CreateOr(L, R, "union");
    case BinaryOp::Mul: return B.CreateAnd(L, R, "intersection");
    default:            return B.CreateAnd(L, B.CreateNot(R), "difference");
    }
  }

  if (E->getType()->isReal()) {
    L = toDouble(L, LT);
    R = toDouble(R, RTy);
    switch (Op) {
    case BinaryOp::Add: return B.CreateFAdd(L, R, "add");
    case BinaryOp::Sub: return B.CreateFSub(L, R, "sub");
    case BinaryOp::Mul: return B.CreateFMul(L, R, "mul");
    case BinaryOp::RealDiv:
      if (rangeChecks(E->getOpLoc()))
        emitRuntimeCheck(
            B.CreateFCmpOEQ(R, llvm::ConstantFP::get(B.getDoubleTy(), 0.0)),
            "division by zero", E->getOpLoc());
      return B.CreateFDiv(L, R, "div");
    default: llvm_unreachable("not a real operator");
    }
  }

  // vector[N] of T (llvm-23.md LB-6): elementwise, without the per-lane
  // overflow checks (LLVM's vectors have no checked add/sub/mul).
  if (auto *VT = llvm::dyn_cast<VectorType>(E->getType())) {
    // The scalar operand of a vector-scalar op: broadcast to the lanes.
    auto Broadcast = [&](llvm::Value *V, Type *T) -> llvm::Value * {
      if (llvm::isa<VectorType>(T))
        return V;
      Type *ET = VT->getElementType();
      V = ET->isReal() ? toDouble(V, T) : convertOrdinal(V, T, ET);
      return B.CreateVectorSplat(VT->getCount(), V, "splat");
    };
    L = Broadcast(L, LT);
    R = Broadcast(R, RTy);
    bool RealLane = VT->getElementType()->isReal();
    switch (Op) {
    case BinaryOp::Add:
      return RealLane ? B.CreateFAdd(L, R, "ve.add")
                      : B.CreateAdd(L, R, "ve.add");
    case BinaryOp::Sub:
      return RealLane ? B.CreateFSub(L, R, "ve.sub")
                      : B.CreateSub(L, R, "ve.sub");
    case BinaryOp::Mul:
      return RealLane ? B.CreateFMul(L, R, "ve.mul")
                      : B.CreateMul(L, R, "ve.mul");
    default:
      llvm_unreachable("operator rejected by Sema");
    }
  }

  SourceLocation Loc = E->getOpLoc();
  // Integer operators: both operands in the operand type chosen by Sema.
  bool Signed = true;
  if (Type *OpTy = E->getOperandType()) {
    L = convertOrdinal(L, LT, OpTy);
    R = convertOrdinal(R, RTy, OpTy);
    Signed = isSignedOrdinal(OpTy);
  }
  switch (Op) {
  // Turbo/FPC bitwise operators (booleans: xor only).
  case BinaryOp::And: {
    llvm::Value *V = B.CreateAnd(L, R, "and");
    if (Signed)
      noteRange(V, andRange(rangeOfValue(L), rangeOfValue(R)));
    return V;
  }
  case BinaryOp::Or:  return B.CreateOr(L, R, "or");
  case BinaryOp::Xor: return B.CreateXor(L, R, "xor");
  case BinaryOp::Shl: {
    unsigned Bits = L->getType()->getIntegerBitWidth();
    return B.CreateShl(L, B.CreateAnd(R, intConst(R, Bits - 1)), "shl");
  }
  case BinaryOp::Shr: { // logical, like Turbo/FPC
    unsigned Bits = L->getType()->getIntegerBitWidth();
    return B.CreateLShr(L, B.CreateAnd(R, intConst(R, Bits - 1)), "shr");
  }
  case BinaryOp::Add: case BinaryOp::Sub: case BinaryOp::Mul:
    return emitCheckedArith(Op, Signed, L, R, Loc,
                            Op == BinaryOp::Add   ? "add"
                            : Op == BinaryOp::Sub ? "sub"
                                                  : "mul");
  case BinaryOp::IntDiv: {
    llvm::Value *V = emitDiv(L, R, Signed, Loc);
    if (Signed)
      noteRange(V, divRange(L, R));
    return V;
  }
  case BinaryOp::Mod: {
    llvm::Value *V = emitMod(L, R, Signed, Loc);
    if (Signed)
      noteRange(V, modRange(L, R));
    return V;
  }
  default:
    llvm_unreachable("operator rejected by Sema");
  }
}


llvm::Value *CodeGenModule::visitCallExpr(CallExpr *E){
  if (E->isMethodCast())
    return emitAddress(E);
  if (Expr *V = E->getCalleeValue()) // a procedural field or property
    return emitCallThrough(visitExpr(V), llvm::cast<RoutineType>(V->getType()),
                           E->getArgs(), E->getBeginLoc());
  if (E->isVarCast()) { // T(x) of an untyped parameter x
    if (E->getType()->isShortString())
      return emitAddress(E);
    return loadScalar(emitAddress(E), E->getType(), "", /*AllowTBAA=*/false);
  }
  if (llvm::isa<TypeDecl>(E->getDecl()))
    return emitTypecast(E);
  if (auto *R = llvm::dyn_cast<RoutineDecl>(E->getDecl())) {
    if (R->isStatic()) // a record's static method
      return emitCall(R, E->getArgs());
    if (R->isMethod())
      return emitMethodCall(R, withBase(E->getWith()), E->getArgs(),
                            /*Inherited=*/false, E->getBeginLoc(),
                            withIsClass(E->getWith()));
    return emitCall(R, E->getArgs());
  }
  if (auto *V = llvm::dyn_cast<VarDecl>(E->getDecl()))
    return emitIndirectCall(V, E->getArgs());
  return emitBuiltinCall(E, llvm::cast<BuiltinDecl>(E->getDecl()));
}


/// `a ${x} b`: each value is written, as `write` writes it, to the
/// runtime's text file that collects characters (__pas_tmpl_sink);
/// __pas_tmpl_add then makes the string so far, what was collected and
/// the text that follows into one string. (Nothing but the write itself
/// runs between the two, so a template inside a value is done by then.)
llvm::Value *CodeGenModule::visitTemplateExpr(TemplateExpr *E){
  llvm::ArrayRef<Expr *> Parts = E->getParts();
  llvm::Type *Ptr = B.getPtrTy();
  auto Text = [&](Expr *P) -> llvm::Value * {
    auto *SL = llvm::cast<StringLiteralExpr>(P);
    if (SL->getValue().empty())
      return llvm::ConstantPointerNull::get(B.getPtrTy());
    return getAnsiConstant(SL->getValue());
  };
  llvm::Value *R = Text(Parts[0]);
  for (size_t I = 1; I < Parts.size(); I += 2) {
    llvm::Value *File = astrCall("__pas_tmpl_sink", Ptr, {});
    emitWriteArg(File, Parts[I]);
    R = holdResult(astrCall("__pas_tmpl_add", Ptr, {R, Text(Parts[I + 1])}));
  }
  return R;
}


llvm::Value *CodeGenModule::visitFormattedExpr(FormattedExpr *){
  llvm_unreachable("formatted expressions are handled by emitWriteArg");
}


llvm::Value *CodeGenModule::visitNamedArgExpr(NamedArgExpr *){
  llvm_unreachable("Sema puts named arguments in the parameters' order");
}


llvm::Value *CodeGenModule::visitIndirectCallExpr(IndirectCallExpr *E){
  Expr *Callee = E->getCallee();
  if (auto *I = llvm::dyn_cast<InheritedExpr>(Callee))
    return emitInheritedCall(I, E->getArgs());
  if (auto *F = llvm::dyn_cast<FieldExpr>(Callee);
      F && llvm::isa_and_nonnull<RoutineDecl>(F->getMember()))
    return emitMemberCall(F, E->getArgs());
  auto *RT = llvm::cast<RoutineType>(Callee->getType());
  llvm::Value *Pair = RT->isOfObject() || RT->isReference()
                          ? visitExpr(Callee) // (a closure: also a call's result)
                          : B.CreateLoad(memType(RT), emitAddress(Callee));
  return emitCallThrough(Pair, RT, E->getArgs(), Callee->getBeginLoc());
}


llvm::Value *CodeGenModule::visitInheritedExpr(InheritedExpr *E){
  return emitInheritedCall(E, {});
}


/// Is the reference of type T at Addr nil? (an i1)
llvm::Value *CodeGenModule::isNilAt(llvm::Value *Addr, Type *T){
  llvm::Value *P;
  if (T->isInterface()) // {object, table}
    P = B.CreateLoad(B.getPtrTy(), Addr, "obj");
  else
    P = B.CreateLoad(B.getPtrTy(), Addr, "ref");
  return B.CreateICmpEQ(P, llvm::ConstantPointerNull::get(B.getPtrTy()), "isnil");
}


/// The default value of type T (0, false, '', nil), as visitExpr gives it.
llvm::Value *CodeGenModule::zeroValue(Type *T){
  if (T->getBase()->isBoolean())
    return B.getInt1(false);
  return llvm::Constant::getNullValue(memType(T));
}


/// x?.m: x's value in a hidden variable; where it is not nil, m (in
/// another, which is the value; else the default value) or the call.
llvm::Value *CodeGenModule::visitSafeNavExpr(SafeNavExpr *E){
  Expr *Base = E->getBase();
  Type *BT = Base->getType();
  llvm::Value *Tmp = varAddress(E->getTmp());
  storeExpr(Tmp, BT, Base);
  llvm::Value *Res = E->getRes() ? varAddress(E->getRes()) : nullptr;
  Type *RT = E->getRes() ? E->getRes()->getType() : nullptr;
  if (Res)
    storeValue(Res, zeroValue(RT), RT);
  auto *ThenBB = newBlock("nav.then");
  auto *EndBB = newBlock("nav.end");
  B.CreateCondBr(isNilAt(Tmp, BT), EndBB, ThenBB);
  B.SetInsertPoint(ThenBB);
  if (E->isStatement()) {
    visitStmt(E->getCall());
  } else {
    storeExpr(Res, RT, E->getAccess());
    if (VarDecl *Ok = E->getOk())
      storeScalar(varAddress(Ok), B.getInt1(true), Ok->getType());
  }
  B.CreateBr(EndBB);
  B.SetInsertPoint(EndBB);
  return E->isStatement() ? nullptr : visitExpr(E->getResRef());
}


/// a ?? b: a in a hidden variable, replaced by b where a is nil (or where
/// a's chain of ?. stopped).
llvm::Value *CodeGenModule::visitCoalesceExpr(CoalesceExpr *E){
  Expr *L = E->getLeft();
  while (auto *P = llvm::dyn_cast<ParenExpr>(L))
    L = P->getInner();
  VarDecl *Ok = nullptr;
  if (auto *Nav = llvm::dyn_cast<SafeNavExpr>(L)) {
    while (auto *Inner = llvm::dyn_cast<SafeNavExpr>(Nav->getAccess()))
      Nav = Inner;
    Ok = Nav->getOk();
  }
  if (Ok)
    storeScalar(varAddress(Ok), B.getInt1(false), Ok->getType());
  Type *T = E->getType();
  llvm::Value *Res = varAddress(E->getRes());
  storeExpr(Res, T, E->getLeft());
  llvm::Value *Missing =
      Ok ? B.CreateNot(loadScalar(varAddress(Ok), Ok->getType()), "missing")
         : isNilAt(Res, T);
  auto *ThenBB = newBlock("coalesce.else");
  auto *EndBB = newBlock("coalesce.end");
  B.CreateCondBr(Missing, ThenBB, EndBB);
  B.SetInsertPoint(ThenBB);
  storeExpr(Res, T, E->getRight());
  B.CreateBr(EndBB);
  B.SetInsertPoint(EndBB);
  return visitExpr(E->getResRef());
}


/// if c then a else b: the if statement Sema made of it sets a hidden
/// variable, which is the value.
llvm::Value *CodeGenModule::visitCondExpr(CondExpr *E){
  visitStmt(E->getLowered());
  return E->getType()->isStructured() ? emitAddress(E->getResult())
                                      : visitExpr(E->getResult());
}


llvm::Value *CodeGenModule::visitAggregateExpr(AggregateExpr *){
  llvm_unreachable("aggregates only initialize typed constants");
}


llvm::Value *CodeGenModule::visitAddrOfExpr(AddrOfExpr *E){
  if (E->getMethod())
    return emitMethodPointer(E);
  if (RoutineDecl *L = E->getLambda(); L && L->isMethodLambda()) {
    // {code, object}: the object is the Self where it is made
    VarDecl *Obj = L->getLambdaSelf();
    llvm::Value *Pair = llvm::UndefValue::get(
        llvm::StructType::get(B.getPtrTy(), B.getPtrTy()));
    Pair = B.CreateInsertValue(Pair, Functions[L], 0);
    return B.CreateInsertValue(
        Pair,
        Obj ? loadScalar(varAddress(Obj), Obj->getType(), "obj")
            : llvm::ConstantPointerNull::get(B.getPtrTy()),
        1);
  }
  if (E->getCode())
    return Functions[E->getCode()];
  return emitAddress(E->getOperand());
}


llvm::Value *CodeGenModule::visitNilExpr(NilExpr *){
  return llvm::ConstantPointerNull::get(B.getPtrTy());
}


llvm::Value *CodeGenModule::visitDerefExpr(DerefExpr *E){
  if (E->getType()->isShortString())
    return emitAddress(E);
  return loadScalar(emitAddress(E), E->getType(), "", /*AllowTBAA=*/false);
}


llvm::Value *CodeGenModule::visitSetCtorExpr(SetCtorExpr *E){
  if (auto *DA = llvm::dyn_cast_or_null<DynArrayType>(E->getType()))
    return emitDynArrayCtor(E, DA);
  return emitSetCtor(E);
}


llvm::Value *CodeGenModule::visitIndexExpr(IndexExpr *E){
  if (E->getProperty())
    return emitArrayPropertyRead(E);
  if (isBitPackedComponent(E))
    return bitLoad(E);
  if (E->getType()->isShortString())
    return emitAddress(E);
  if (E->getBase()->getType()->isAnsiString()) { // reading: no copy
    if (isSliceExpr(E->getBase()))
      return emitSliceIndex(E);
    bool SafeBounds = canElideAnsiBoundsCheck(E->getBase(), E->getIndex());
    llvm::Value *S = visitExpr(E->getBase());
    llvm::Value *X = toI64(visitExpr(E->getIndex()), E->getIndex()->getType());
    if (!SafeBounds)
      emitAnsiIndexCheck(S, X, E->getIndex()->getBeginLoc());
    return loadScalar(B.CreateGEP(B.getInt8Ty(), S,
                                  B.CreateSub(X, B.getInt64(1)), "",
                                  indexFlags(E->getIndex(), 1)),
                      E->getType());
  }
  return loadScalar(emitAddress(E), E->getType(), "", !isAliasedExpr(E));
}


llvm::Value *CodeGenModule::visitFieldExpr(FieldExpr *E){
  if (NameRefExpr *Ref = E->getConstRef()) // TFoo.C
    return visitExpr(Ref);
  if (isBitPackedComponent(E))
    return bitLoad(E);
  if (E->getBase()->getType()->isRecord() &&
      !llvm::isa_and_nonnull<FieldDecl>(E->getMember())) {
    if (llvm::isa_and_nonnull<RoutineDecl>(E->getMember()))
      return emitMemberCall(E, {}); // a record's method
    return emitMemberValue(E);      // a record's property
  }
  if (!llvm::isa_and_nonnull<FieldDecl>(E->getMember()) ||
      E->getBase()->getType()->isClass() ||
      E->getBase()->getType()->isClassRef())
    return emitMemberValue(E); // objfpc: a member of an object
  if (E->getType()->isShortString())
    return emitAddress(E);
  return loadScalar(emitAddress(E), E->getType(), E->getField().Name,
                    !isAliasedExpr(E));
}

static bool tryAnsiConstLength(Expr *E, uint32_t &Len) {
  E = CodeGenModule::unwrapConst(E);
  if (auto *SL = llvm::dyn_cast_or_null<StringLiteralExpr>(E)) {
    Len = static_cast<uint32_t>(SL->getValue().size());
    return true;
  }
  if (auto *N = llvm::dyn_cast_or_null<NameRefExpr>(E); N && !N->getWith()) {
    if (auto *C = llvm::dyn_cast_or_null<ConstDecl>(N->getDecl());
        C && C->getValue().K == ConstValue::String) {
      Len = static_cast<uint32_t>(C->getValue().Str.size());
      return true;
    }
  }
  return false;
}

bool CodeGenModule::isSliceExpr(Expr *E) const {
  E = skipParens(E);
  if (auto *N = llvm::dyn_cast_or_null<NameRefExpr>(E); N && !N->getWith())
    if (auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl()))
      return SliceLocals.count(V) || SliceParams.count(V);
  if (auto *C = llvm::dyn_cast_or_null<CallExpr>(E);
      C && !C->isVarCast() && !C->isMethodCast() && !C->getCalleeValue() &&
      !C->getWith() && C->getArgs().size() == 3 &&
      C->getArgs()[0]->getType()->isAnsiString()) {
    if (auto *BD = llvm::dyn_cast_or_null<BuiltinDecl>(C->getDecl());
        BD && BD->getBuiltinKind() == BuiltinKind::Copy) {
      Expr *Src = skipParens(C->getArgs()[0]);
      if (auto *SN = llvm::dyn_cast_or_null<NameRefExpr>(Src);
          SN && !SN->getWith())
        if (auto *SV = llvm::dyn_cast_or_null<VarDecl>(SN->getDecl()))
          return !SV->isCaptured() && !SV->isAliased() && !SV->isVarParam() &&
                 !SV->isOutParam() && (SV->isConstParam() || SliceLocals.count(SV) || SliceParams.count(SV));
    }
  }
  return false;
}

CodeGenModule::AnsiSliceVal CodeGenModule::emitSliceCopy(Expr *SrcExpr,
                                                         Expr *IdxExpr,
                                                         Expr *CntExpr) {
  AnsiSliceVal Src = emitSliceExpr(SrcExpr);
  llvm::Value *Idx = emitI32(IdxExpr);
  llvm::Value *Cnt = emitI32(CntExpr);
  llvm::Value *N = B.CreateSExt(Src.Len, B.getInt64Ty());
  llvm::Value *I = B.CreateSExt(Idx, B.getInt64Ty());
  llvm::Value *C = B.CreateSExt(Cnt, B.getInt64Ty());
  llvm::Value *ILt1 = B.CreateICmpSLT(I, B.getInt64(1));
  llvm::Value *Start = B.CreateSelect(ILt1, B.getInt64(1), I, "slice.start");
  llvm::Value *ValidC = B.CreateICmpSGT(C, B.getInt64(0));
  llvm::Value *ValidS = B.CreateICmpSLE(Start, N);
  llvm::Value *Valid = B.CreateAnd(ValidC, ValidS, "slice.ok");
  llvm::Value *Avail =
      B.CreateAdd(B.CreateSub(N, Start), B.getInt64(1), "slice.avail");
  llvm::Value *MinLen = B.CreateSelect(B.CreateICmpSGT(C, Avail), Avail, C);
  llvm::Value *ClipLen = B.CreateTrunc(MinLen, B.getInt32Ty());
  llvm::Value *Len = B.CreateSelect(Valid, ClipLen, B.getInt32(0), "slice.len");
  llvm::Value *Off = B.CreateSub(Start, B.getInt64(1), "slice.off");
  llvm::Value *RawPtr = B.CreateGEP(B.getInt8Ty(), Src.Ptr, Off, "slice.raw");
  llvm::Value *Ptr = B.CreateSelect(
      Valid, RawPtr, llvm::ConstantPointerNull::get(B.getPtrTy()), "slice.ptr");
  return {Ptr, Len};
}

CodeGenModule::AnsiSliceVal CodeGenModule::emitSliceExpr(Expr *E) {
  E = skipParens(E);
  if (auto *N = llvm::dyn_cast_or_null<NameRefExpr>(E); N && !N->getWith()) {
    if (auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl())) {
      if (auto It = SliceLocals.find(V); It != SliceLocals.end()) {
        llvm::Value *Ptr =
            B.CreateLoad(B.getPtrTy(), It->second.Ptr, V->getName().Name + ".ptr");
        llvm::Value *Len =
            B.CreateLoad(B.getInt32Ty(), It->second.Len, V->getName().Name + ".len");
        return {Ptr, Len};
      }
      if (auto It = SliceParams.find(V); It != SliceParams.end())
        return It->second;
    }
  }
  if (auto *C = llvm::dyn_cast_or_null<CallExpr>(E);
      C && !C->isVarCast() && !C->isMethodCast() && !C->getCalleeValue() &&
      !C->getWith() && C->getArgs().size() == 3 &&
      C->getArgs()[0]->getType()->isAnsiString()) {
    if (auto *BD = llvm::dyn_cast_or_null<BuiltinDecl>(C->getDecl());
        BD && BD->getBuiltinKind() == BuiltinKind::Copy)
      return emitSliceCopy(C->getArgs()[0], C->getArgs()[1], C->getArgs()[2]);
  }
  llvm::Value *Ptr = emitAnsi(E);
  uint32_t ConstLen = 0;
  if (tryAnsiConstLength(E, ConstLen))
    return {Ptr, B.getInt32(ConstLen)};
  return {Ptr, astrCall("__pas_astr_length", B.getInt32Ty(), {Ptr})};
}

llvm::Value *CodeGenModule::emitSliceIndex(IndexExpr *E) {
  AnsiSliceVal S = emitSliceExpr(E->getBase());
  llvm::Value *X = toI64(visitExpr(E->getIndex()), E->getIndex()->getType());
  if (rangeChecks(E->getIndex()->getBeginLoc())) {
    llvm::Value *Lt1 = B.CreateICmpSLT(X, B.getInt64(1));
    llvm::Value *GtLen =
        B.CreateICmpSGT(X, B.CreateSExt(S.Len, B.getInt64Ty()));
    emitRuntimeCheck(B.CreateOr(Lt1, GtLen), "string index out of bounds",
                     E->getIndex()->getBeginLoc());
  }
  return loadScalar(B.CreateGEP(B.getInt8Ty(), S.Ptr,
                                B.CreateSub(X, B.getInt64(1)), "",
                                indexFlags(E->getIndex(), 1)),
                    E->getType());
}

llvm::Value *CodeGenModule::emitAnsiEqSlices(BinaryOp Op, AnsiSliceVal L,
                                             AnsiSliceVal R) {
  auto *CL = llvm::dyn_cast<llvm::ConstantInt>(L.Len);
  auto *CR = llvm::dyn_cast<llvm::ConstantInt>(R.Len);
  if (CL || CR) {
    bool LeftConst = CL != nullptr;
    uint32_t K = LeftConst ? CL->getZExtValue() : CR->getZExtValue();
    AnsiSliceVal Var = LeftConst ? R : L;
    AnsiSliceVal Con = LeftConst ? L : R;
    if (K == 0)
      return B.CreateICmp(Op == BinaryOp::Eq ? llvm::CmpInst::ICMP_EQ
                                             : llvm::CmpInst::ICMP_NE,
                          Var.Len, B.getInt32(0), "str.empty");
    llvm::BasicBlock *EntryBB = B.GetInsertBlock();
    llvm::BasicBlock *CmpBB = newBlock("str.cmp");
    llvm::BasicBlock *DoneBB = newBlock("str.eq.done");
    llvm::Value *LenEq =
        B.CreateICmpEQ(Var.Len, B.getInt32(K), "str.leneq");
    B.CreateCondBr(LenEq, CmpBB, DoneBB);

    B.SetInsertPoint(CmpBB);
    llvm::Value *Cmp = B.CreateCall(RT.Memcmp, {Var.Ptr, Con.Ptr, B.getInt64(K)});
    llvm::Value *MemEq = B.CreateICmpEQ(Cmp, B.getInt32(0), "str.memeq");
    llvm::BasicBlock *CmpEndBB = B.GetInsertBlock();
    B.CreateBr(DoneBB);

    B.SetInsertPoint(DoneBB);
    llvm::PHINode *Eq = B.CreatePHI(B.getInt1Ty(), 2, "str.eq");
    Eq->addIncoming(B.getInt1(false), EntryBB);
    Eq->addIncoming(MemEq, CmpEndBB);
    return Op == BinaryOp::Ne ? B.CreateNot(Eq, "str.ne") : Eq;
  }

  llvm::BasicBlock *EntryBB = B.GetInsertBlock();
  llvm::BasicBlock *EmptyBB = newBlock("str.empty.chk");
  llvm::BasicBlock *CmpBB = newBlock("str.cmp");
  llvm::BasicBlock *DoneBB = newBlock("str.eq.done");
  llvm::Value *LenEq = B.CreateICmpEQ(L.Len, R.Len, "str.leneq");
  B.CreateCondBr(LenEq, EmptyBB, DoneBB);

  B.SetInsertPoint(EmptyBB);
  llvm::Value *IsEmpty = B.CreateICmpEQ(L.Len, B.getInt32(0), "str.empty");
  B.CreateCondBr(IsEmpty, DoneBB, CmpBB);

  B.SetInsertPoint(CmpBB);
  llvm::Value *Cmp = B.CreateCall(
      RT.Memcmp, {L.Ptr, R.Ptr, B.CreateSExt(L.Len, B.getInt64Ty())});
  llvm::Value *MemEq = B.CreateICmpEQ(Cmp, B.getInt32(0), "str.memeq");
  llvm::BasicBlock *CmpEndBB = B.GetInsertBlock();
  B.CreateBr(DoneBB);

  B.SetInsertPoint(DoneBB);
  llvm::PHINode *Eq = B.CreatePHI(B.getInt1Ty(), 3, "str.eq");
  Eq->addIncoming(B.getInt1(false), EntryBB);
  Eq->addIncoming(B.getInt1(true), EmptyBB);
  Eq->addIncoming(MemEq, CmpEndBB);
  return Op == BinaryOp::Ne ? B.CreateNot(Eq, "str.ne") : Eq;
}

llvm::Value *CodeGenModule::emitSliceCompare(BinaryOp Op, Expr *LHS, Expr *RHS) {
  AnsiSliceVal L = emitSliceExpr(LHS);
  AnsiSliceVal R = emitSliceExpr(RHS);
  if (Op == BinaryOp::Eq || Op == BinaryOp::Ne)
    return emitAnsiEqSlices(Op, L, R);
  llvm::Value *Cmp = astrCall("__pas_astr_slice_compare", B.getInt32Ty(),
                              {L.Ptr, L.Len, R.Ptr, R.Len});
  return compareInts(Op, Cmp, B.getInt32(0), /*Signed=*/true);
}

