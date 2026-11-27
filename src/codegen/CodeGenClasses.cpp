// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- CodeGenClasses.cpp - AST -> LLVM IR ---------------------------------------===//

#include "CodeGenInternal.h"

using namespace p3;
using namespace p3::detail;


/// The interface D defines (not a forward declaration), or null.
InterfaceType *CodeGenModule::definedInterface(Decl *D){
  auto *TD = llvm::dyn_cast<TypeDecl>(D);
  auto *IS = TD ? llvm::dyn_cast_or_null<InterfaceTypeSpec>(TD->getTypeSpec())
                : nullptr;
  if (!IS || IS->isForward())
    return nullptr;
  return llvm::dyn_cast_or_null<InterfaceType>(TD->getType());
}


/// The class D defines (a type declaration `TFoo = class ... end`, not a
/// forward one), or null.
/// The member table of the advanced record D declares (or the type
/// helper D declares), or null.
ClassType *CodeGenModule::advancedRecord(Decl *D){
  auto *TD = llvm::dyn_cast<TypeDecl>(D);
  if (TD && llvm::isa_and_nonnull<HelperTypeSpec>(TD->getTypeSpec()))
    return llvm::dyn_cast_or_null<ClassType>(TD->getType());
  auto *RT = TD ? llvm::dyn_cast_or_null<RecordType>(TD->getType()) : nullptr;
  return RT ? RT->getMemberTable() : nullptr;
}


/// Does E name a type (TFoo, TOuter.TInner)?
bool CodeGenModule::namesType(Expr *E){
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
    return llvm::isa_and_nonnull<TypeDecl>(N->getDecl());
  auto *F = llvm::dyn_cast<FieldExpr>(E);
  return F && llvm::isa_and_nonnull<TypeDecl>(F->getMember());
}


/// Decls, each followed by the types declared in it when it is a class
/// or an advanced record (recursively).
llvm::SmallVector<Decl *, 16> CodeGenModule::withNested(llvm::ArrayRef<Decl *> Decls){
  llvm::SmallVector<Decl *, 16> Out;
  std::function<void(Decl *)> Add = [&](Decl *D) {
    Out.push_back(D);
    ClassType *C = definedClass(D);
    if (!C)
      C = advancedRecord(D);
    if (C)
      for (Decl *M : C->getMembers())
        if (llvm::isa<TypeDecl>(M))
          Add(M);
  };
  for (Decl *D : Decls)
    Add(D);
  return Out;
}


ClassType *CodeGenModule::definedClass(Decl *D){
  auto *TD = llvm::dyn_cast<TypeDecl>(D);
  auto *CS = TD ? llvm::dyn_cast_or_null<ClassTypeSpec>(TD->getTypeSpec())
                : nullptr;
  if (!CS || CS->isForward())
    return nullptr;
  return llvm::dyn_cast_or_null<ClassType>(TD->getType());
}


bool CodeGenModule::hasNonLocalLabels(Block &Blk){
  for (Decl *D : Blk.getDecls())
    if (auto *L = llvm::dyn_cast<LabelDecl>(D); L && L->isNonLocal())
      return true;
  return false;
}


void CodeGenModule::declareInterface(Decl *D, InterfaceType *I){
  auto Export = ExportNames.find(D);
  std::string Text = std::string(1, static_cast<char>(I->getDeclName().size())) +
                     I->getDeclName().str();
  llvm::Constant *Init =
      llvm::ConstantDataArray::getString(M.getContext(), Text, false);
  auto *G = new llvm::GlobalVariable(
      M, Init->getType(), /*isConstant=*/true,
      Export == ExportNames.end() ? llvm::GlobalValue::InternalLinkage
                                  : llvm::GlobalValue::ExternalLinkage,
      ImportedDecls.count(D) ? nullptr : Init,
      Export == ExportNames.end() ? "iid." + I->getDeclName().lower()
                                  : Export->second);
  IIDs[I] = G;
}


/// The interface table of class C (null without interfaces): its
/// interfaces' iids and method tables.
llvm::Constant *CodeGenModule::interfaceTable(ClassType *C){
  bool Cleanup = hasManagedFields(C);
  if (C->getInterfaces().empty() && !Cleanup)
    return llvm::ConstantPointerNull::get(B.getPtrTy());
  std::string Name = C->getDeclName().lower();
  llvm::Constant *CleanupFn = llvm::ConstantPointerNull::get(B.getPtrTy());
  if (Cleanup) {
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false),
        llvm::Function::InternalLinkage, "cleanup." + Name, M);
    F->getArg(0)->setName("self");
    CleanupsToEmit.push_back({C, F});
    CleanupFn = F;
  }
  llvm::SmallVector<llvm::Constant *, 8> Entries{
      llvm::ConstantExpr::getIntToPtr(B.getInt64(C->getInterfaces().size()),
                                      B.getPtrTy()),
      CleanupFn};
  for (const InterfaceImpl &I : C->getInterfaces()) {
    llvm::SmallVector<llvm::Constant *, 8> Methods;
    for (RoutineDecl *R : I.Methods)
      Methods.push_back(
          R->isAbstract()
              ? llvm::cast<llvm::Function>(
                    classRuntime("__pas_abstract_error", B.getVoidTy(), {})
                        .getCallee())
              : static_cast<llvm::Constant *>(Functions[R]));
    auto *Ty = llvm::ArrayType::get(B.getPtrTy(), Methods.size());
    auto *T = new llvm::GlobalVariable(
        M, Ty, /*isConstant=*/true, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(Ty, Methods),
        "itable." + Name + "." + I.Intf->getDeclName().lower());
    Entries.push_back(iidOf(I.Intf));
    Entries.push_back(T);
  }
  auto *Ty = llvm::ArrayType::get(B.getPtrTy(), Entries.size());
  return new llvm::GlobalVariable(M, Ty, /*isConstant=*/true,
                                  llvm::GlobalValue::InternalLinkage,
                                  llvm::ConstantArray::get(Ty, Entries),
                                  "intfs." + Name);
}


/// A class: its VMT (defined once all methods are declared), and for a
/// class of a separately compiled unit its methods' declarations.
void CodeGenModule::declareClass(Decl *D, ClassType *C){
  if (ClassSet.insert(C).second)
    AllClasses.push_back(C);
  auto Export = ExportNames.find(D);
  bool Imported = ImportedDecls.count(D);
  auto *Ty = llvm::ArrayType::get(B.getPtrTy(), 6 + C->getVTable().size());
  std::string Name = Export == ExportNames.end()
                         ? "vmt." + C->getDeclName().lower()
                         : Export->second;
  VMTs[C] = new llvm::GlobalVariable(
      M, Ty, /*isConstant=*/true,
      Export == ExportNames.end() ? llvm::GlobalValue::InternalLinkage
                                  : llvm::GlobalValue::ExternalLinkage,
      nullptr, Name);
  if (!Imported) {
    VMTsToDefine.push_back(C);
    return;
  }
  for (Decl *M : C->getMembers())
    if (auto *R = llvm::dyn_cast<RoutineDecl>(M);
        R && !R->isAbstract() && !R->isGenericMethod()) {
      Functions[R] = llvm::Function::Create(routineFnType(R),
                                            llvm::Function::ExternalLinkage,
                                            ExportNames[R], this->M);
      if (R->isAsync() || R->isIterator())
        declareAsync(R, ExportNames[R], /*External=*/true);
    }
}


/// A runtime function used only by programs with classes.
llvm::FunctionCallee CodeGenModule::classRuntime(llvm::StringRef Name, llvm::Type *Ret,
                                  llvm::ArrayRef<llvm::Type *> Params){
  llvm::FunctionCallee F = M.getOrInsertFunction(
      Name, llvm::FunctionType::get(Ret, Params, /*isVarArg=*/false));
  auto *Fn = llvm::cast<llvm::Function>(F.getCallee());
  Fn->addFnAttr(llvm::Attribute::NoUnwind);
  addRuntimeAttrs(Fn);
  RuntimeFns.insert(Fn);
  return F;
}


/// Can a call of the runtime function Name raise an exception? Those
/// that raise one, and those that may run a destructor (which is Pascal
/// code): releasing an object or an interface reference.
bool CodeGenModule::runtimeMayRaise(llvm::StringRef Name){
  return Name == "__pas_raise" || Name == "__pas_raise_at" ||
         Name == "__pas_raise_divzero" || Name == "__pas_raise_overflow" ||
         Name == "__pas_reraise" || Name == "__pas_reraise_free" ||
         Name == "__pas_free_object" || Name == "__pas_free_instance" ||
         Name == "__pas_intf_assign" || Name == "__pas_intf_clear" ||
         Name == "__pas_frame_release" || Name == "__pas_closure_assign" ||
         Name == "__pas_task_await" || Name == "__pas_scope_leave" ||
         Name == "__pas_task_assign" || Name == "__pas_task_clear" ||
         Name == "__pas_gen_next" || Name == "__pas_gen_yield" ||
         Name == "__pas_gen_assign" || Name == "__pas_gen_clear" ||
         Name == "__pas_closure_clear" ||
         Name == "__pas_dyn_clear" || Name == "__pas_dyn_assign" ||
         Name == "__pas_dyn_setlength" ||
         Name == "__pas_format_pstr" ||
         Name == "__pas_owned_set" || Name == "__pas_owned_done";
}


/// Does the code emitted for the current function so far have a call
/// that may raise an exception? Any call does, except of an LLVM
/// intrinsic, of a runtime function that cannot (runtimeMayRaise), of
/// the function itself and of a routine emitted before that was found
/// not to (NoRaiseFns). So calls through procedural values and virtual
/// methods, of external routines and of routines emitted later count:
/// this errs on the side of protecting.
bool CodeGenModule::mayRaise() const{
  for (const llvm::BasicBlock &BB : *CurFn)
    for (const llvm::Instruction &I : BB) {
      auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
      if (!Call)
        continue;
      const llvm::Function *Callee = Call->getCalledFunction();
      if (!Callee)
        return true;
      if (Callee->isIntrinsic() || Callee == CurFn || NoRaiseFns.count(Callee))
        continue;
      if (!RuntimeFns.count(Callee) || runtimeMayRaise(Callee->getName()))
        return true;
    }
  return false;
}


/// The VMT of C; the predeclared classes' (TObject, Exception, EDivByZero
/// and its ancestors) are in the runtime, as __pas_<class>_vmt.
llvm::Constant *CodeGenModule::vmtOf(ClassType *C){
  if (llvm::GlobalVariable *G = VMTs.lookup(C))
    return G;
  std::string Name = "__pas_" + C->getDeclName().lower() + "_vmt";
  llvm::GlobalVariable *G = M.getNamedGlobal(Name);
  if (!G)
    G = new llvm::GlobalVariable(
        M, llvm::ArrayType::get(B.getPtrTy(), 6 + C->getVTable().size()),
        /*isConstant=*/true, llvm::GlobalValue::ExternalLinkage, nullptr,
        Name);
  VMTs[C] = G;
  return G;
}


/// The depth of C in its hierarchy (TObject: 0).
unsigned CodeGenModule::classDepth(ClassType *C){
  unsigned Depth = 0;
  for (ClassType *A = C; A && A->getParent(); A = A->getParent())
    ++Depth;
  return Depth;
}


/// The VMTs from C's root to C itself, for a VMT's ancestor display.
llvm::SmallVector<llvm::Constant *, 8> CodeGenModule::classAncestors(ClassType *C){
  llvm::SmallVector<llvm::Constant *, 8> Ancestors;
  for (ClassType *A = C; A; A = A->getParent())
    Ancestors.push_back(vmtOf(A));
  std::reverse(Ancestors.begin(), Ancestors.end());
  return Ancestors;
}


/// Fills in the VMTs: parent VMT, class name, instance size, interface
/// table, hierarchy depth, ancestor display, then the virtual methods.
void CodeGenModule::defineVMTs(){
  for (ClassType *C : VMTsToDefine) {
    llvm::SmallVector<llvm::Constant *, 8> Slots;
    Slots.push_back(vmtOf(C->getParent()));
    Slots.push_back(getPStrConstant(C->getDeclName()));
    Slots.push_back(llvm::ConstantExpr::getIntToPtr(
        B.getInt64(instanceSize(C)), B.getPtrTy()));
    Slots.push_back(interfaceTable(C));
    llvm::SmallVector<llvm::Constant *, 8> Ancestors = classAncestors(C);
    auto *AncTy = llvm::ArrayType::get(B.getPtrTy(), Ancestors.size());
    auto *Anc = new llvm::GlobalVariable(
        M, AncTy, /*isConstant=*/true, llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(AncTy, Ancestors),
        "ancestors." + C->getDeclName().lower());
    Anc->setAlignment(llvm::Align(8));
    Slots.push_back(llvm::ConstantExpr::getIntToPtr(
        B.getInt64(Ancestors.size() - 1), B.getPtrTy()));
    Slots.push_back(Anc);
    for (RoutineDecl *R : C->getVTable()) {
      if (R->getBuiltin() == TObjectMethod::Destroy)
        Slots.push_back(llvm::cast<llvm::Function>(
            classRuntime("__pas_tobject_destroy", B.getVoidTy(),
                         {B.getPtrTy(), B.getPtrTy()})
                .getCallee()));
      else if (R->isAbstract())
        Slots.push_back(llvm::cast<llvm::Function>(
            classRuntime("__pas_abstract_error", B.getVoidTy(), {})
                .getCallee()));
      else
        Slots.push_back(Functions[R]);
    }
    auto *G = llvm::cast<llvm::GlobalVariable>(VMTs[C]);
    G->setInitializer(llvm::ConstantArray::get(
        llvm::cast<llvm::ArrayType>(G->getValueType()), Slots));
  }
  VMTsToDefine.clear();
}


/// Is C TInterfacedObject (the predeclared one) or a descendant?
bool CodeGenModule::isInterfacedClass(const ClassType *C){
  for (; C; C = C->getParent())
    if (!C->getModule() && C->getDeclName() == "TInterfacedObject")
      return true;
  return false;
}


/// Does class C have fields of its own holding COM interfaces?
bool CodeGenModule::hasManagedFields(const ClassType *C){
  for (Decl *D : C->getMembers())
    if (auto *F = llvm::dyn_cast<FieldDecl>(D); F && isManaged(F->getType()))
      return true;
  return false;
}


/// The iid of interface I (the predeclared IUnknown's is in the runtime).
llvm::Constant *CodeGenModule::iidOf(InterfaceType *I){
  if (llvm::GlobalVariable *G = IIDs.lookup(I))
    return G;
  std::string Name = "__pas_" + I->getDeclName().lower() + "_iid";
  llvm::GlobalVariable *G = M.getNamedGlobal(Name);
  if (!G)
    G = new llvm::GlobalVariable(M, B.getInt8Ty(), /*isConstant=*/true,
                                 llvm::GlobalValue::ExternalLinkage, nullptr,
                                 Name);
  IIDs[I] = G;
  return G;
}


/// The cleanup function of class C: releases its own fields' COM
/// interfaces (the runtime calls it for each class of a freed object).
void CodeGenModule::emitCleanup(ClassType *C, llvm::Function *F){
  CurFn = F;
  AddrAligns.clear();
  CurRoutine = nullptr;
  CurFrame = nullptr;
  CurPin = nullptr;
  PinTemps = true;
  CurSP = nullptr;
  B.SetCurrentDebugLocation(llvm::DebugLoc());
  B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", F));
  for (Decl *D : C->getMembers())
    if (auto *Fd = llvm::dyn_cast<FieldDecl>(D); Fd && isManaged(Fd->getType()))
      finalizeValue(fieldAddress(F->getArg(0), C, Fd), Fd->getType());
  B.CreateRetVoid();
}


void CodeGenModule::emitCleanups(){
  for (auto &[C, F] : CleanupsToEmit)
    emitCleanup(C, F);
  CleanupsToEmit.clear();
}


llvm::Value *CodeGenModule::withBase(WithContext *W){
  if (W->IsSelf) // load Self; a record's (a type helper's) Self is its address
    return W->Record->getType()->isRecord() || isVarParamRef(W->Record)
               ? emitAddress(W->Record)
               : visitExpr(W->Record);
  return WithAddrs.lookup(W);
}


//===--------------------------------------------------------------------===//
// Advanced records (objfpc)
//===--------------------------------------------------------------------===//

/// The value of E, a user-defined conversion applied (its operator called
/// with E's unconverted value); an address for a record.
/// A constant used as a member (TFoo.C, obj.C): its reference, which is
/// emitted instead; otherwise E.
Expr *CodeGenModule::unwrapConst(Expr *E){
  if (auto *F = llvm::dyn_cast<FieldExpr>(E))
    if (NameRefExpr *Ref = F->getConstRef())
      return Ref;
  if (auto *B = llvm::dyn_cast<BinaryExpr>(E)) // 'a' + 'b'
    if (NameRefExpr *Ref = B->getConstRef())
      return Ref;
  return E;
}


bool CodeGenModule::isVarParamRef(Expr *E){
  auto *N = llvm::dyn_cast<NameRefExpr>(E);
  auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(N->getDecl()) : nullptr;
  return V && V->isVarParam();
}


/// The method of type helper member D (a property: its getter), or null.
RoutineDecl *CodeGenModule::helperRoutine(Decl *D){
  if (auto *P = llvm::dyn_cast<PropertyDecl>(D))
    D = P->getRead();
  auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D);
  return R && R->getClass() && R->getClass()->isHelper() ? R : nullptr;
}


/// The Self of a type helper's member used through F: the address of
/// its base, or of a temporary holding the base's value.
llvm::Value *CodeGenModule::helperSelf(FieldExpr *F){
  Expr *Base = F->getBase();
  if (!F->isHelperValue())
    return emitAddress(Base);
  Type *T = helperRoutine(F->getMember())->getClass()->getHelperFor();
  if (T->isAnsiString()) {
    llvm::Value *V = emitAnsi(Base);
    astrCall("__pas_astr_addref", B.getVoidTy(), {V});
    return heldTemp(V);
  }
  if (T->isDynArray()) {
    llvm::Value *V = visitExpr(Base);
    astrCall("__pas_dyn_addref", B.getVoidTy(), {V});
    return heldTemp(V, T);
  }
  if (auto *S = llvm::dyn_cast<ShortStringType>(T)) {
    llvm::Value *Tmp = newStringTemp(S->getMaxLength());
    emitStringAssign(Tmp, S, Base);
    return Tmp;
  }
  llvm::Value *Tmp = createTemp(memType(T), "self.tmp");
  if (T->isStructured())
    copyAggregate(Tmp, visitExpr(Base), T);
  else
    storeScalar(Tmp, visitExpr(Base), T);
  return Tmp;
}


/// R(Args) on record Obj (its address): a method, or a class method or
/// operator (static: no Self).
llvm::Value *CodeGenModule::emitRecordMethodCall(RoutineDecl *R, llvm::Value *Obj,
                                  llvm::ArrayRef<Expr *> Args){
  if (!R->hasSelf())
    return emitCall(R, Args);
  llvm::Value *Result =
      emitCallTo(Functions[R], llvm::ConstantPointerNull::get(B.getPtrTy()),
                 R->getParams(), Args, Obj, resultOf(R));
  return R->isFunction() ? Result : nullptr;
}


/// TR.Create(args): a new record (zeroed, in a temporary) initialized by
/// the constructor Ctor.
llvm::Value *CodeGenModule::emitRecordConstruction(RecordType *RT, RoutineDecl *Ctor,
                                    llvm::ArrayRef<Expr *> Args){
  llvm::Type *Ty = memType(RT);
  llvm::Value *Tmp = createTemp(Ty, "rec");
  B.CreateMemSet(Tmp, B.getInt8(0), DL.getTypeAllocSize(Ty),
                 DL.getABITypeAlign(Ty));
  emitCallTo(Functions[Ctor], llvm::ConstantPointerNull::get(B.getPtrTy()),
             Ctor->getParams(), Args, Tmp);
  return Tmp;
}


/// The interface I value of object Obj: {Obj, its class's table for I}.
llvm::Value *CodeGenModule::toInterface(llvm::Value *Obj, InterfaceType *I){
  llvm::Value *Table = B.CreateCall(
      classRuntime("__pas_intf_lookup", B.getPtrTy(), {B.getPtrTy(), B.getPtrTy()}),
      {Obj, iidOf(I)}, "itable");
  return makeInterface(Obj, Table);
}


llvm::Value *CodeGenModule::makeInterface(llvm::Value *Obj, llvm::Value *Table){
  llvm::Type *Ty = llvm::StructType::get(B.getPtrTy(), B.getPtrTy());
  llvm::Value *V = B.CreateInsertValue(llvm::UndefValue::get(Ty), Obj, 0);
  return B.CreateInsertValue(V, Table, 1);
}


/// The object of an interface value (nil: itself).
llvm::Value *CodeGenModule::interfaceObject(llvm::Value *V){
  return V->getType()->isStructTy() ? B.CreateExtractValue(V, 0, "obj") : V;
}


/// Is W's object a class (Self in a class method)?
bool CodeGenModule::withIsClass(WithContext *W){
  return W->Record->getType()->isClassRef();
}


/// if (Obj == nil) runtime error, under range checks.
void CodeGenModule::emitNilObjectCheck(llvm::Value *Obj, SourceLocation L){
  if (rangeChecks(L))
    emitRuntimeCheck(B.CreateIsNull(Obj), "use of a nil object reference", L);
}


/// The object an expression of a class type refers to, checked for nil.
llvm::Value *CodeGenModule::emitObject(Expr *E){
  llvm::Value *Obj = visitExpr(E);
  emitNilObjectCheck(Obj, E->getBeginLoc());
  return Obj;
}


/// Address of virtual method Index in the VMT of object Obj.
llvm::Value *CodeGenModule::loadVirtual(llvm::Value *Obj, unsigned Index){
  return loadVMTSlot(B.CreateLoad(B.getPtrTy(), Obj, "vmt"), 6 + Index,
                     "method");
}


/// Can no separately compiled unit see C (and so subclass it)?
bool CodeGenModule::classIsClosed(ClassType *C) const{
  return !CGOpts.SeparateUnits ||
         (C->getModule() != nullptr && !ExportedClasses.count(C));
}


/// Does C have descendants in what this compilation can see? Unknown
/// (separately compiled) descendants count when C is not closed.
bool CodeGenModule::classHasDescendants(ClassType *C){
  if (C->getModule() == nullptr) // a predeclared class: the runtime's own
    return true;                 // descendants are not in AllClasses
  if (!classIsClosed(C))
    return true;
  for (ClassType *D : AllClasses)
    if (D != C && D->inheritsFrom(C))
      return true;
  return false;
}


/// Can a virtual call of R be a direct call? Only if no class of the
/// whole-program hierarchy that descends from R's overrides it; with
/// -fseparate-units only for a class that a separately compiled unit
/// cannot see (item 11).
bool CodeGenModule::canDevirtualize(RoutineDecl *R){
  ClassType *C = R->getClass();
  if (!C || R->isAbstract() || !R->isDispatched() || !Functions.count(R))
    return false;
  if (!classIsClosed(C))
    return false;
  int Idx = R->getVMTIndex();
  llvm::ArrayRef<RoutineDecl *> VTable = C->getVTable();
  if (Idx < 0 || static_cast<size_t>(Idx) >= VTable.size() ||
      VTable[Idx] != R)
    return false;
  for (ClassType *D : AllClasses) {
    if (D == C || !D->inheritsFrom(C))
      continue;
    llvm::ArrayRef<RoutineDecl *> DVT = D->getVTable();
    if (static_cast<size_t>(Idx) < DVT.size() && DVT[Idx] != R)
      return false;
  }
  return true;
}


/// Entry Slot of a VMT: 0 the parent's VMT, 1 the class name, 2 the
/// instance size, 3 the interface table, 4... the virtual methods.
llvm::Value *CodeGenModule::loadVMTSlot(llvm::Value *VMT, unsigned Slot,
                         const llvm::Twine &Name){
  llvm::Value *P = B.CreateConstInBoundsGEP1_64(B.getPtrTy(), VMT, Slot);
  auto *L = B.CreateLoad(B.getPtrTy(), P, Name);
  // A VMT is a constant global: its slots never change (item 9).
  L->setMetadata(llvm::LLVMContext::MD_invariant_load,
                 llvm::MDNode::get(M.getContext(), {}));
  return L;
}


/// The class (VMT) of a method call's receiver: Obj itself if SelfIsClass,
/// else the object's (checked for nil).
llvm::Value *CodeGenModule::receiverClass(llvm::Value *Obj, bool SelfIsClass,
                           SourceLocation L){
  if (SelfIsClass)
    return Obj;
  emitNilObjectCheck(Obj, L);
  return B.CreateLoad(B.getPtrTy(), Obj, "vmt");
}


/// if (VMT == nil) runtime error, under range checks.
void CodeGenModule::emitNilClassCheck(llvm::Value *VMT, SourceLocation L){
  if (rangeChecks(L))
    emitRuntimeCheck(B.CreateIsNull(VMT), "use of a nil class reference", L);
}


/// obj.Free: if obj <> nil then obj.Destroy (virtual) and release it.
void CodeGenModule::emitFree(llvm::Value *Obj){
  auto *DoBB = newBlock("free.do");
  auto *EndBB = newBlock("free.end");
  B.CreateCondBr(B.CreateIsNull(Obj), EndBB, DoBB);
  B.SetInsertPoint(DoBB);
  llvm::Value *Fn = loadVirtual(Obj, 0);
  llvm::FunctionType *FTy =
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy(), B.getPtrTy()}, false);
  B.CreateCall(FTy, Fn, {llvm::ConstantPointerNull::get(B.getPtrTy()), Obj});
  B.CreateCall(intfRuntime("__pas_free_instance"), {Obj});
  B.CreateBr(EndBB);
  B.SetInsertPoint(EndBB);
}


/// Calls method R on object Obj. Inherited: a static call of the
/// inherited method (no dispatch, a destructor does not release the
/// object); otherwise a virtual method is called through the VMT and a
/// destructor releases the object afterwards. Returns the result (an
/// address for strings and records), or null.
/// SelfIsClass: Obj is a class (a VMT), as in TFoo.M, c.M (c: class of
/// T) and in class methods; then R is a class method or a constructor,
/// which creates an object of that class.
llvm::Value *CodeGenModule::emitMethodCall(RoutineDecl *R, llvm::Value *Obj,
                            llvm::ArrayRef<Expr *> Args, bool Inherited,
                            SourceLocation L, bool SelfIsClass){
  if (R->isAsync())
    return emitAsyncCall(R, nullptr,
                         R->isClassMethod() ? receiverClass(Obj, SelfIsClass, L)
                                            : Obj,
                         Args);
  if (R->isIterator())
    return emitIteratorCall(R, nullptr,
                            R->isClassMethod() ? receiverClass(Obj, SelfIsClass, L)
                                               : Obj,
                            Args);
  if (R->getClass() && R->getClass()->hasSelfByReference())
    return emitRecordMethodCall(R, Obj, Args);
  if (R->isConstructor() && SelfIsClass && !Inherited)
    return emitConstructionThrough(Obj, R, Args, L);
  switch (R->getBuiltin()) {
  case TObjectMethod::Create: // TObject's constructor does nothing
    return nullptr;
  case TObjectMethod::ClassType:
    return B.CreateLoad(B.getPtrTy(), Obj, "vmt");
  case TObjectMethod::ClassParent:
    return loadVMTSlot(receiverClass(Obj, SelfIsClass, L), 0, "parent");
  case TObjectMethod::InstanceSize:
    return B.CreatePtrToInt(
        loadVMTSlot(receiverClass(Obj, SelfIsClass, L), 2, "size"),
        B.getInt64Ty(), "instancesize");
  case TObjectMethod::InheritsFrom: {
    llvm::Value *VMT = receiverClass(Obj, SelfIsClass, L);
    llvm::Value *Arg = visitExpr(Args[0]);
    llvm::FunctionCallee Fn = classRuntime(
        "__pas_class_inherits", B.getInt32Ty(), {B.getPtrTy(), B.getPtrTy()});
    llvm::Value *R = B.CreateCall(Fn, {VMT, Arg});
    return B.CreateICmpNE(R, B.getInt32(0), "inherits");
  }
  case TObjectMethod::ExceptionCreate: { // FMessage := msg
    ClassType *C = R->getClass();
    auto *Msg = llvm::cast<FieldDecl>(C->findOwnMember("fmessage"));
    emitStringAssign(fieldAddress(Obj, C, Msg),
                     llvm::cast<ShortStringType>(Msg->getType()), Args[0]);
    return nullptr;
  }
  case TObjectMethod::ExceptionCreateFmt: { // FMessage := Format(msg, args)
    ClassType *C = R->getClass();
    auto *Msg = llvm::cast<FieldDecl>(C->findOwnMember("fmessage"));
    llvm::Value *Fmt = emitAnsi(Args[0]);
    llvm::Value *A = emitConstArray(Args[1]);
    astrCall("__pas_format_pstr", B.getVoidTy(),
             {fieldAddress(Obj, C, Msg), Fmt, A});
    return nullptr;
  }
  case TObjectMethod::Free:
    emitFree(Obj);
    return nullptr;
  case TObjectMethod::ClassName:
    return loadVMTSlot(
        SelfIsClass ? Obj : B.CreateLoad(B.getPtrTy(), Obj, "vmt"), 1,
        "classname");
  case TObjectMethod::Destroy:
    if (Inherited) // TObject's destructor does nothing
      return nullptr;
    break;
  case TObjectMethod::None:
    break;
  }
  llvm::Value *Result;
  if (R->isClassMethod()) { // Self: the class
    llvm::Value *Self = receiverClass(Obj, SelfIsClass, L);
    llvm::FunctionCallee Fn = Functions[R];
    if (!Inherited && R->isDispatched() && !canDevirtualize(R)) {
      emitNilClassCheck(Self, L);
      Fn = {routineFnType(R), loadVMTSlot(Self, 6 + R->getVMTIndex(), "method")};
    }
    Result = emitCallTo(Fn, llvm::ConstantPointerNull::get(B.getPtrTy()),
                        R->getParams(), Args, Self, resultOf(R));
    return R->isFunction() ? Result : nullptr;
  }
  ClassType *Exact = KnownClass.lookup(Obj);
  RoutineDecl *Impl = nullptr;
  if (!Inherited && R->isDispatched() && Exact && R->getVMTIndex() >= 0) {
    auto VTable = Exact->getVTable();
    unsigned Idx = static_cast<unsigned>(R->getVMTIndex());
    if (Idx < VTable.size()) {
      Impl = VTable[Idx];
      if (!Impl || Impl->isAbstract() || !Functions.count(Impl))
        Impl = nullptr;
    }
  }
  if (!Inherited && R->isDispatched() && !Impl && !canDevirtualize(R)) {
    emitNilObjectCheck(Obj, L);
    llvm::Value *Fn = loadVirtual(Obj, static_cast<unsigned>(R->getVMTIndex()));
    Result = emitCallTo({routineFnType(R), Fn},
                        llvm::ConstantPointerNull::get(B.getPtrTy()),
                        R->getParams(), Args, Obj, resultOf(R));
  } else {
    RoutineDecl *Direct = Impl ? Impl : R;
    Result = emitCallTo(Functions[Direct],
                        llvm::ConstantPointerNull::get(B.getPtrTy()),
                        R->getParams(), Args, Obj, resultOf(Direct));
  }
  if (R->isDestructor() && !Inherited)
    B.CreateCall(intfRuntime("__pas_free_instance"), {Obj});
  return R->isFunction() ? Result : nullptr;
}


/// TFoo.Create(args): a new object of class C, initialized by Ctor.
/// The arguments are evaluated first. An exception leaving a constructor
/// written in Pascal destroys and releases the new object (as in Free
/// Pascal), so the call runs protected.
llvm::Value *CodeGenModule::emitConstruction(ClassType *C, RoutineDecl *Ctor,
                              llvm::ArrayRef<Expr *> Args, SourceLocation L){
  if (Ctor->getBuiltin() != TObjectMethod::None) {
    llvm::Value *Obj =
        B.CreateCall(RT.New, {B.getInt64(instanceSize(C))}, "obj");
    B.CreateStore(vmtOf(C), Obj);
    KnownClass[Obj] = C;
    emitMethodCall(Ctor, Obj, Args, /*Inherited=*/true, L);
    return Obj;
  }
  llvm::SmallVector<llvm::Value *, 8> Values{
      llvm::ConstantPointerNull::get(B.getPtrTy())};
  Values.push_back(nullptr); // Self
  emitArgValues(Ctor->getParams(), Args, Values);
  llvm::Value *Obj =
      B.CreateCall(RT.New, {B.getInt64(instanceSize(C))}, "obj");
  B.CreateStore(vmtOf(C), Obj);
  Values[1] = Obj;
  bool Counted = isInterfacedClass(Ctor->getClass());
  if (Counted)
    B.CreateCall(intfRuntime("__pas_interfaced_construct"), {Obj});
  if (NoRaiseFns.count(Functions[Ctor])) { // nothing to protect against
    B.CreateCall(Functions[Ctor], Values);
    if (Counted)
      B.CreateCall(intfRuntime("__pas_interfaced_constructed"), {Obj});
    KnownClass[Obj] = C;
    return Obj;
  }
  auto *Failed = newBlock("ctor.raise");
  llvm::Value *F = emitProtect(Failed, "ctor.call");
  B.CreateCall(Functions[Ctor], Values);
  B.CreateCall(excRuntime("__pas_exc_pop"), {F});
  if (Counted)
    B.CreateCall(intfRuntime("__pas_interfaced_constructed"), {Obj});
  auto *Done = newBlock("ctor.done");
  B.CreateBr(Done);
  B.SetInsertPoint(Failed);
  B.CreateCall(excRuntime("__pas_reraise_free"), {F, Obj});
  B.CreateUnreachable();
  B.SetInsertPoint(Done);
  KnownClass[Obj] = C;
  return Obj;
}


/// c.Create(args) with c a class (a VMT; also Self in a class method): a
/// new object of the class c refers to, of its size, initialized by Ctor
/// (virtual: c's). Protected like emitConstruction.
llvm::Value *CodeGenModule::emitConstructionThrough(llvm::Value *VMT, RoutineDecl *Ctor,
                                     llvm::ArrayRef<Expr *> Args,
                                     SourceLocation L){
  emitNilClassCheck(VMT, L);
  auto Allocate = [&] {
    llvm::Value *Size =
        B.CreatePtrToInt(loadVMTSlot(VMT, 2, "size"), B.getInt64Ty());
    llvm::Value *Obj = B.CreateCall(RT.New, {Size}, "obj");
    B.CreateStore(VMT, Obj);
    return Obj;
  };
  if (Ctor->getBuiltin() != TObjectMethod::None) {
    llvm::Value *Obj = Allocate();
    emitMethodCall(Ctor, Obj, Args, /*Inherited=*/true, L);
    return Obj;
  }
  llvm::SmallVector<llvm::Value *, 8> Values{
      llvm::ConstantPointerNull::get(B.getPtrTy())};
  Values.push_back(nullptr); // Self
  emitArgValues(Ctor->getParams(), Args, Values);
  llvm::Value *Obj = Allocate();
  Values[1] = Obj;
  bool Counted = isInterfacedClass(Ctor->getClass());
  if (Counted)
    B.CreateCall(intfRuntime("__pas_interfaced_construct"), {Obj});
  auto *Failed = newBlock("ctor.raise");
  llvm::Value *F = emitProtect(Failed, "ctor.call");
  if (Ctor->isDispatched())
    B.CreateCall(routineFnType(Ctor),
                 loadVMTSlot(VMT, 6 + Ctor->getVMTIndex(), "ctor"), Values);
  else
    B.CreateCall(Functions[Ctor], Values);
  B.CreateCall(excRuntime("__pas_exc_pop"), {F});
  if (Counted)
    B.CreateCall(intfRuntime("__pas_interfaced_constructed"), {Obj});
  auto *Done = newBlock("ctor.done");
  B.CreateBr(Done);
  B.SetInsertPoint(Failed);
  B.CreateCall(excRuntime("__pas_reraise_free"), {F, Obj});
  B.CreateUnreachable();
  B.SetInsertPoint(Done);
  return Obj;
}


/// obj.M(args) or TFoo.Create(args) through FieldExpr F.
/// Base.R(Args), Base an interface value: R through its method table.
llvm::Value *CodeGenModule::emitInterfaceCall(RoutineDecl *R, Expr *Base, SourceLocation L,
                               llvm::ArrayRef<Expr *> Args){
  llvm::Value *V = visitExpr(Base);
  llvm::Value *Obj = B.CreateExtractValue(V, 0, "obj");
  llvm::Value *Table = B.CreateExtractValue(V, 1, "itable");
  if (rangeChecks(L))
    emitRuntimeCheck(B.CreateIsNull(Obj), "use of a nil interface reference", L);
  llvm::Value *Slot =
      B.CreateConstInBoundsGEP1_64(B.getPtrTy(), Table, R->getVMTIndex());
  llvm::Value *Fn = B.CreateLoad(B.getPtrTy(), Slot, "method");
  llvm::Value *Result =
      emitCallTo({routineFnType(R), Fn},
                 llvm::ConstantPointerNull::get(B.getPtrTy()),
                 R->getParams(), Args, Obj, resultOf(R));
  return R->isFunction() ? Result : nullptr;
}


llvm::Value *CodeGenModule::emitMemberCall(FieldExpr *F, llvm::ArrayRef<Expr *> Args){
  auto *R = llvm::cast<RoutineDecl>(F->getMember());
  if (R->getClass() && R->getClass()->isHelper()) // a type helper's
    return R->hasSelf() ? emitRecordMethodCall(R, helperSelf(F), Args)
                        : emitCall(R, Args);
  if (R->isStatic() && !F->getBase()->getType()->isRecord())
    return emitCall(R, Args); // a class's static method: no Self
  if (auto *RT = llvm::dyn_cast<RecordType>(F->getBase()->getType())) {
    if (namesType(F->getBase())) { // TR.Create, TR.M
      if (R->isConstructor())
        return emitRecordConstruction(RT, R, Args);
      return emitCall(R, Args);
    }
    if (!R->hasSelf())
      return emitCall(R, Args);
    return emitRecordMethodCall(R, emitAddress(F->getBase()), Args);
  }
  if (F->getBase()->getType()->isInterface()) // through the method table
    return emitInterfaceCall(R, F->getBase(), F->getField().Loc, Args);
  if (namesType(F->getBase())) { // TFoo.Create, TOuter.TInner.Create
    auto *C = llvm::cast<ClassType>(F->getBase()->getType());
    if (R->isConstructor())
      return emitConstruction(C, R, Args, F->getBeginLoc());
    return emitMethodCall(R, vmtOf(C), Args, /*Inherited=*/false,
                          F->getField().Loc, /*SelfIsClass=*/true);
  }
  if (F->getBase()->getType()->isClassRef()) // c.M, c: class of T
    return emitMethodCall(R, visitExpr(F->getBase()), Args, /*Inherited=*/false,
                          F->getField().Loc, /*SelfIsClass=*/true);
  // Not checked for nil: Free accepts nil, and a static method may be
  // called on nil (as in Free Pascal); virtual calls are checked.
  llvm::Value *Obj = visitExpr(F->getBase());
  return emitMethodCall(R, Obj, Args, /*Inherited=*/false, F->getField().Loc);
}


/// inherited [M] [(args)]: on the enclosing method's Self.
llvm::Value *CodeGenModule::emitInheritedCall(InheritedExpr *I, llvm::ArrayRef<Expr *> Args){
  if (!I->getMethod()) // a bare `inherited` with no inherited method
    return nullptr;
  llvm::Value *Self = B.CreateLoad(B.getPtrTy(),
                                   varAddress(I->getEnclosing()->getSelfVar()),
                                   "self");
  if (I->isBare() && Args.empty())
    Args = I->getImplicitArgs();
  return emitMethodCall(I->getMethod(), Self, Args, /*Inherited=*/true,
                        I->getBeginLoc(), I->getEnclosing()->isClassMethod());
}


/// The value of property P of object Obj (static class C; for an advanced
/// record, Obj is its address and C the record type): an address for
/// strings and records. Indexes: an array property's.
llvm::Value *CodeGenModule::emitPropertyRead(PropertyDecl *P, llvm::Value *Obj, Type *C,
                              llvm::ArrayRef<Expr *> Indexes){
  Type *T = P->getType();
  if (P->isClassProperty()) { // a class variable or a static method
    if (auto *V = llvm::dyn_cast<VarDecl>(P->getRead()))
      return T->isStructured() ? Globals[V]
                               : loadScalar(Globals[V], T, P->getName().Name);
    return emitCall(llvm::cast<RoutineDecl>(P->getRead()),
                    accessorArgs(P, Indexes, nullptr));
  }
  if (auto *F = llvm::dyn_cast<FieldDecl>(P->getRead())) {
    llvm::Value *Addr = fieldAddress(Obj, C, F);
    if (T->isStructured())
      return Addr;
    return loadScalar(Addr, T, P->getName().Name);
  }
  return emitMethodCall(llvm::cast<RoutineDecl>(P->getRead()), Obj,
                        accessorArgs(P, Indexes, nullptr),
                        /*Inherited=*/false, P->getLocation());
}


/// The arguments of a property's accessor method: the indexes of an
/// array property, the `index` constant, the value written (if any).
llvm::SmallVector<Expr *, 4> CodeGenModule::accessorArgs(PropertyDecl *P,
                                          llvm::ArrayRef<Expr *> Indexes,
                                          Expr *Value){
  llvm::SmallVector<Expr *, 4> Args(Indexes.begin(), Indexes.end());
  if (Expr *I = P->getIndexSpec())
    Args.push_back(I);
  if (Value)
    Args.push_back(Value);
  return Args;
}


/// The object of an array property access E (obj.P[i], P[i], obj[i]),
/// and its static class C (an advanced record: its address and type).
llvm::Value *CodeGenModule::propertyObject(IndexExpr *E, Type *&C){
  Expr *Root = E->getPropertyRoot();
  if (auto *F = llvm::dyn_cast<FieldExpr>(Root);
      F && F->getMember() == E->getProperty()) {
    C = F->getBase()->getType();
    return memberObject(F->getBase());
  }
  if (auto *N = llvm::dyn_cast<NameRefExpr>(Root);
      N && N->getDecl() == E->getProperty()) {
    C = N->getWith()->Record->getType();
    return withBase(N->getWith());
  }
  C = Root->getType(); // the default property (a record's: its address)
  return memberObject(Root);
}


/// The object whose member Base.M is used: Base's object (checked for
/// nil), or a record's address.
llvm::Value *CodeGenModule::memberObject(Expr *Base){
  if (Base->getType()->isRecord())
    return emitAddress(Base);
  return emitObject(Base);
}


/// The value of array property access E (an address for strings and
/// records).
llvm::Value *CodeGenModule::emitArrayPropertyRead(IndexExpr *E){
  if (E->getProperty()->isClassProperty())
    return emitPropertyRead(E->getProperty(), nullptr, nullptr,
                            E->getPropertyArgs());
  Type *C;
  llvm::Value *Obj = propertyObject(E, C);
  return emitPropertyRead(E->getProperty(), Obj, C, E->getPropertyArgs());
}


/// A member of a class used as a value through FieldExpr F: a field, a
/// property, or a call of a method without arguments.
llvm::Value *CodeGenModule::emitMemberValue(FieldExpr *F){
  Decl *D = F->getMember();
  if (llvm::isa<RoutineDecl>(D))
    return emitMemberCall(F, {});
  if (auto *V = llvm::dyn_cast<VarDecl>(D)) { // a class variable
    if (V->getType()->isShortString())
      return Globals[V];
    return loadScalar(Globals[V], V->getType(), V->getName().Name);
  }
  Type *C = F->getBase()->getType();
  if (auto *P = llvm::dyn_cast<PropertyDecl>(D); P && P->isClassProperty())
    return emitPropertyRead(P, nullptr, nullptr); // (no object)
  if (auto *P = llvm::dyn_cast<PropertyDecl>(D);
      P && helperRoutine(P)) // a type helper's
    return emitPropertyRead(P, helperSelf(F), C);
  if (auto *P = llvm::dyn_cast<PropertyDecl>(D); P && C->isInterface())
    return emitInterfaceCall(llvm::cast<RoutineDecl>(P->getRead()), F->getBase(),
                             F->getField().Loc, {}); // (its getter)
  llvm::Value *Obj = memberObject(F->getBase());
  if (auto *P = llvm::dyn_cast<PropertyDecl>(D))
    return emitPropertyRead(P, Obj, C);
  llvm::Value *Addr = fieldAddress(Obj, C, F->getFieldDecl());
  if (F->getType()->isShortString())
    return Addr;
  return loadScalar(Addr, F->getType(), F->getField().Name);
}


/// obj is TFoo / obj as TFoo: the runtime walks obj's VMT chain.
/// Is a VMT the VMT of C or of a descendant? A class with no descendants
/// only needs the equality test; otherwise the depth and ancestor display
/// make it two loads and two compares (item 12).
llvm::Value *CodeGenModule::classIsVMT(llvm::Value *VMT, ClassType *C){
  llvm::Constant *Target = vmtOf(C);
  if (!classHasDescendants(C))
    return B.CreateICmpEQ(VMT, Target, "is");
  unsigned Depth = classDepth(C);
  llvm::Value *Exact = B.CreateICmpEQ(VMT, Target, "is.exact");
  llvm::Value *ODepth =
      B.CreatePtrToInt(loadVMTSlot(VMT, 4, "depth"), B.getInt64Ty());
  llvm::Value *Ancestors = loadVMTSlot(VMT, 5, "ancestors");
  llvm::Value *Slot =
      B.CreateConstInBoundsGEP1_64(B.getPtrTy(), Ancestors, Depth);
  llvm::Value *Ancestor = B.CreateLoad(B.getPtrTy(), Slot, "ancestor");
  llvm::Value *In = B.CreateAnd(
      B.CreateICmpUGE(ODepth, B.getInt64(Depth)),
      B.CreateICmpEQ(Ancestor, Target), "is.display");
  return B.CreateOr(Exact, In, "is");
}


/// obj is C: false for nil, else classIsVMT.
llvm::Value *CodeGenModule::emitClassIs(llvm::Value *Obj, ClassType *C){
  auto *NullBB = newBlock("is.null");
  auto *CheckBB = newBlock("is.check");
  auto *EndBB = newBlock("is.end");
  B.CreateCondBr(B.CreateIsNull(Obj), NullBB, CheckBB);
  B.SetInsertPoint(NullBB);
  B.CreateBr(EndBB);
  B.SetInsertPoint(CheckBB);
  llvm::Value *VMT = B.CreateLoad(B.getPtrTy(), Obj, "vmt");
  llvm::Value *R = classIsVMT(VMT, C);
  llvm::BasicBlock *CheckEnd = B.GetInsertBlock();
  B.CreateBr(EndBB);
  B.SetInsertPoint(EndBB);
  auto *Phi = B.CreatePHI(B.getInt1Ty(), 2, "is");
  Phi->addIncoming(B.getFalse(), NullBB);
  Phi->addIncoming(R, CheckEnd);
  return Phi;
}


/// obj as C: obj, or a runtime error if it is not a C (item 12).
llvm::Value *CodeGenModule::emitClassAs(llvm::Value *Obj, ClassType *C, SourceLocation L){
  auto *NullBB = newBlock("as.null");
  auto *CheckBB = newBlock("as.check");
  auto *ErrBB = newBlock("as.error");
  auto *OkBB = newBlock("as.ok");
  B.CreateCondBr(B.CreateIsNull(Obj), NullBB, CheckBB);
  B.SetInsertPoint(NullBB);
  B.CreateBr(OkBB);
  B.SetInsertPoint(CheckBB);
  llvm::Value *VMT = B.CreateLoad(B.getPtrTy(), Obj, "vmt");
  B.CreateCondBr(classIsVMT(VMT, C), OkBB, ErrBB);
  B.SetInsertPoint(ErrBB);
  emitRuntimeError("invalid class typecast", L);
  B.SetInsertPoint(OkBB);
  return Obj;
}


llvm::Value *CodeGenModule::emitIsAs(BinaryExpr *E){
  llvm::Value *Obj = visitExpr(E->getLHS());
  if (E->getLHS()->getType()->isInterface()) // a COM interface's object
    Obj = B.CreateExtractValue(Obj, 0, "obj");
  if (auto *I = llvm::dyn_cast<InterfaceType>(E->getRHS()->getType())) {
    if (E->getOp() == BinaryOp::Is) {
      llvm::Value *T = B.CreateCall(
          classRuntime("__pas_intf_lookup", B.getPtrTy(),
                       {B.getPtrTy(), B.getPtrTy()}),
          {Obj, iidOf(I)});
      return B.CreateIsNotNull(T, "is");
    }
    llvm::Constant *Msg = B.CreateGlobalString(
        ("invalid interface typecast" + where(E->getOpLoc())), ".rterr");
    llvm::Value *T = B.CreateCall(
        classRuntime("__pas_intf_as", B.getPtrTy(),
                     {B.getPtrTy(), B.getPtrTy(), B.getPtrTy()}),
        {Obj, iidOf(I), Msg}, "itable");
    return makeInterface(Obj, T);
  }
  auto *C = llvm::cast<ClassType>(E->getRHS()->getType());
  if (E->getOp() == BinaryOp::Is)
    return emitClassIs(Obj, C);
  return emitClassAs(Obj, C, E->getOpLoc());
}

