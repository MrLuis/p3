// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- CodeGen.cpp - AST -> LLVM IR ---------------------------------------===//

#include "CodeGenInternal.h"

using namespace p3;
using namespace p3::detail;

namespace {
struct RuntimeFuncAttr {
  llvm::StringLiteral Name;
  bool PureQuery;
  bool ReturnsFresh;
  bool ReturnsBool;
  bool NeverFree;
  uint8_t ReadMask;
  uint8_t WriteMask;
  uint8_t SlotMask;
};

constexpr RuntimeFuncAttr RuntimeFuncs[] = {
#define RUNTIME_FUNC(Name, PureQuery, Fresh, BoolRet, NeverFree, ReadMask,     \
                     WriteMask, SlotMask)                                      \
  {Name, (PureQuery) != 0, (Fresh) != 0, (BoolRet) != 0, (NeverFree) != 0,     \
   static_cast<uint8_t>(ReadMask), static_cast<uint8_t>(WriteMask),            \
   static_cast<uint8_t>(SlotMask)},
#include "p3/CodeGen/RuntimeFuncs.def"
};

const RuntimeFuncAttr *findRuntimeFunc(llvm::StringRef Name) {
  for (const auto &Entry : RuntimeFuncs)
    if (Entry.Name == Name)
      return &Entry;
  return nullptr;
}

bool hasParamBit(uint8_t Mask, unsigned I) {
  return I < 8 && (Mask & (1u << I)) != 0;
}

/// Adds to the runtime function F the attributes that are true of it: its
/// memory effects and return, plus those of its parameters (item 9).
} // namespace

void p3::detail::addRuntimeAttrs(llvm::Function *F) {
  llvm::LLVMContext &C = F->getContext();
  llvm::StringRef Name = F->getName();
  const RuntimeFuncAttr *RF = findRuntimeFunc(Name);
  if (RF && RF->PureQuery) {
    F->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        C, llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)));
    F->addFnAttr(llvm::Attribute::WillReturn);
  } else if (Name == "__pas_astr_pchar") {
    F->addFnAttr(llvm::Attribute::getWithMemoryEffects(
        C, llvm::MemoryEffects::none()));
    F->addFnAttr(llvm::Attribute::WillReturn);
  }
  if (RF && RF->ReturnsFresh)
    F->addRetAttr(llvm::Attribute::NoAlias);
  if (RF && RF->ReturnsBool) { // 0 or 1: the range's return attribute
    llvm::AttrBuilder B(C);
    B.addRangeAttr(llvm::ConstantRange(llvm::APInt(32, 0),
                                          llvm::APInt(32, 2)));
    F->addRetAttrs(B);
  }
  if (Name == "__pas_abstract_error") {
    F->addFnAttr(llvm::Attribute::Cold);
    F->addFnAttr(llvm::Attribute::NoReturn);
  }
  if (Name == "__pas_new") { // calloc(1, size), and returns zeroed memory
    llvm::AttrBuilder B(C);
    B.addAllocKindAttr(llvm::AllocFnKind::Alloc |
                       llvm::AllocFnKind::Zeroed);
    B.addAllocSizeAttr(0, std::nullopt);
    F->addFnAttrs(B);
  } else if (Name == "__pas_dyn_new") { // calloc(1, header + count * size)
    llvm::AttrBuilder B(C);
    B.addAllocKindAttr(llvm::AllocFnKind::Alloc |
                       llvm::AllocFnKind::Zeroed);
    F->addFnAttrs(B);
  } else if (Name == "__pas_realloc") {
    llvm::AttrBuilder B(C);
    B.addAllocKindAttr(llvm::AllocFnKind::Realloc);
    B.addAllocSizeAttr(1, std::nullopt);
    F->addFnAttrs(B);
  }
  if (RF) {
    for (unsigned I = 0; I < F->arg_size(); ++I) {
      if (!F->getArg(I)->getType()->isPointerTy())
        continue;
      llvm::AttrBuilder B(C);
      bool Any = false;
      if (hasParamBit(RF->ReadMask, I)) {
        B.addCapturesAttr(llvm::CaptureInfo::none());
        B.addAttribute(llvm::Attribute::ReadOnly);
        Any = true;
      } else if (hasParamBit(RF->WriteMask, I)) {
        B.addCapturesAttr(llvm::CaptureInfo::none());
        B.addAttribute(llvm::Attribute::WriteOnly);
        Any = true;
      }
      if (hasParamBit(RF->SlotMask, I)) {
        B.addCapturesAttr(llvm::CaptureInfo::none());
        B.addDereferenceableOrNullAttr(8);
        Any = true;
      }
      if (Any)
        F->addParamAttrs(I, B);
    }
    if (RF->NeverFree)
      F->addFnAttr(llvm::Attribute::NoFree);
  }
}

Runtime::Runtime(llvm::Module &M) {
  llvm::LLVMContext &C = M.getContext();
  auto *Void = llvm::Type::getVoidTy(C);
  auto *I8 = llvm::Type::getInt8Ty(C);
  auto *I32 = llvm::Type::getInt32Ty(C);
  auto *I64 = llvm::Type::getInt64Ty(C);
  auto *F64 = llvm::Type::getDoubleTy(C);
  auto *Ptr = llvm::PointerType::getUnqual(C);

  auto Decl = [&](llvm::StringRef Name, llvm::Type *Ret,
                  llvm::ArrayRef<llvm::Type *> Params) {
    llvm::FunctionCallee F = M.getOrInsertFunction(
        Name, llvm::FunctionType::get(Ret, Params, /*isVarArg=*/false));
    auto *Fn = llvm::cast<llvm::Function>(F.getCallee());
    Fn->addFnAttr(llvm::Attribute::NoUnwind);
    addRuntimeAttrs(Fn);
    return F;
  };
  Init = Decl("__pas_init", Void, {I32, Ptr});
  Fini = Decl("__pas_fini", Void, {});
  // Text I/O: the first argument is the PasFile*.
  WriteBegin = Decl("__pas_write_begin", Void, {Ptr});
  WriteEnd = Decl("__pas_write_end", Void, {Ptr});
  WriteDiscard = Decl("__pas_write_discard", Void, {Ptr});
  WriteStr = Decl("__pas_write_str", Void, {Ptr, Ptr, I64, I32});
  WriteChar = Decl("__pas_write_char", Void, {Ptr, I8, I32});
  WriteInt = Decl("__pas_write_int", Void, {Ptr, I64, I32});
  WriteBool = Decl("__pas_write_bool", Void, {Ptr, I32, I32});
  WriteReal = Decl("__pas_write_real", Void, {Ptr, F64, I32, I32});
  Writeln = Decl("__pas_writeln", Void, {Ptr});
  ReadInt = Decl("__pas_read_int", I32, {Ptr});
  ReadInt64 = Decl("__pas_read_int64", I64, {Ptr});
  ReadChar = Decl("__pas_read_char", I8, {Ptr});
  ReadReal = Decl("__pas_read_real", F64, {Ptr});
  Readln = Decl("__pas_readln", Void, {Ptr});
  // Files.
  FileNew = Decl("__pas_file_new", Ptr, {I64, I32, Ptr});
  FileFree = Decl("__pas_file_free", Void, {Ptr});
  ProgramParam = Decl("__pas_program_param", Ptr, {I32, Ptr});
  Reset = Decl("__pas_reset", Void, {Ptr});
  Rewrite = Decl("__pas_rewrite", Void, {Ptr});
  Get = Decl("__pas_get", Void, {Ptr});
  Put = Decl("__pas_put", Void, {Ptr});
  Page = Decl("__pas_page", Void, {Ptr});
  Eof = Decl("__pas_eof", I32, {Ptr});
  Eoln = Decl("__pas_eoln", I32, {Ptr});
  Buffer = Decl("__pas_buffer", Ptr, {Ptr});
  ReadElem = Decl("__pas_read_elem", Void, {Ptr, Ptr});
  WriteElem = Decl("__pas_write_elem", Void, {Ptr, Ptr});
  RuntimeError = Decl("__pas_runtime_error", Void, {Ptr});
  auto *RE = llvm::cast<llvm::Function>(RuntimeError.getCallee());
  RE->addFnAttr(llvm::Attribute::NoReturn);
  RE->addFnAttr(llvm::Attribute::Cold);
  RuntimeErrorAt = Decl("__pas_runtime_error_at", Void, {Ptr, Ptr});
  auto *REA = llvm::cast<llvm::Function>(RuntimeErrorAt.getCallee());
  REA->addFnAttr(llvm::Attribute::NoReturn);
  REA->addFnAttr(llvm::Attribute::Cold);
  Memcmp = Decl("memcmp", I32, {Ptr, Ptr, I64});
  New = Decl("__pas_new", Ptr, {I64});
  Dispose = Decl("__pas_dispose", Void, {Ptr});
  Atan = Decl("atan", F64, {F64});
  FreeMem = Decl("__pas_freemem", Void, {Ptr});
  Realloc = Decl("__pas_realloc", Ptr, {Ptr, I64});
  Halt = Decl("__pas_halt", Void, {I32});
  llvm::cast<llvm::Function>(Halt.getCallee())->addFnAttr(llvm::Attribute::NoReturn);
  ParamCount = Decl("__pas_paramcount", I32, {});
  ParamStr = Decl("__pas_paramstr", Void, {Ptr, I32});
  // Short strings (Turbo/FPC).
  StrAssign = Decl("__pas_str_assign", Void, {Ptr, I32, Ptr});
  StrConcat = Decl("__pas_str_concat", Void, {Ptr, Ptr, Ptr});
  StrCompare = Decl("__pas_str_compare", I32, {Ptr, Ptr});
  StrCopy = Decl("__pas_str_copy", Void, {Ptr, Ptr, I32, I32});
  StrPos = Decl("__pas_str_pos", I32, {Ptr, Ptr});
  StrDelete = Decl("__pas_str_delete", Void, {Ptr, I32, I32});
  StrInsert = Decl("__pas_str_insert", Void, {Ptr, Ptr, I32, I32});
  WritePStr = Decl("__pas_write_pstr", Void, {Ptr, Ptr, I32});
  ReadStr = Decl("__pas_read_str", Void, {Ptr, Ptr, I32});
  // Non-local goto. The underscore variants do not save the signal mask.
  // Windows has no _longjmp and a different _setjmp: the runtime's own
  // pair, under its own names (MinGW's C library has a _setjmp too).
  bool Win = llvm::Triple(M.getTargetTriple()).isOSWindows();
  SetJmp = Decl(Win ? "__pas_setjmp" : "_setjmp", I32, {Ptr});
  llvm::cast<llvm::Function>(SetJmp.getCallee())
      ->addFnAttr(llvm::Attribute::ReturnsTwice);
  LongJmp = Decl(Win ? "__pas_longjmp" : "_longjmp", Void, {Ptr, I32});
  llvm::cast<llvm::Function>(LongJmp.getCallee())
      ->addFnAttr(llvm::Attribute::NoReturn);
}


void CodeGenModule::collectExports(UnitDecl &U, bool Imported){
  for (Decl *D : withNested(U.getInterface()->getDecls())) {
    if (ClassType *C = definedClass(D)) {
      // A class: its VMT and its methods.
      ExportedClasses.insert(C);
      std::string Prefix =
          ("pas." + U.getName().Name + "$" + C->getDeclName().lower()).str();
      ExportNames[D] = Prefix + "$vmt";
      if (Imported)
        ImportedDecls.insert(D);
      for (Decl *M : C->getMembers()) {
        if (auto *V = llvm::dyn_cast<VarDecl>(M)) { // a class variable
          ExportNames[V] = Prefix + "." + V->getName().Name.str();
          if (Imported)
            ImportedDecls.insert(V);
          continue;
        }
        // (a generic method: each module compiles the specializations
        // it uses)
        if (auto *R = llvm::dyn_cast<RoutineDecl>(M);
            R && !R->isAbstract() && !R->isGenericMethod()) {
          ExportNames[R] = Prefix + "." + R->getName().Name.str() +
                           overloadSuffix(R);
          if (Imported)
            ImportedDecls.insert(R);
        }
      }
      continue;
    }
    if (ClassType *MT = advancedRecord(D)) { // an advanced record: methods
      std::string Prefix =
          ("pas." + U.getName().Name + "$" + MT->getDeclName().lower()).str();
      for (Decl *M : MT->getMembers()) {
        if (auto *V = llvm::dyn_cast<VarDecl>(M)) { // a class variable
          ExportNames[V] = Prefix + "." + V->getName().Name.str();
          if (Imported)
            ImportedDecls.insert(V);
          continue;
        }
        if (auto *R = llvm::dyn_cast<RoutineDecl>(M)) {
          ExportNames[R] = Prefix + "." + R->getName().Name.str() +
                           overloadSuffix(R);
          if (Imported)
            ImportedDecls.insert(R);
        }
      }
      continue;
    }
    if (definedInterface(D)) { // an interface: its iid
      ExportNames[D] = ("pas." + U.getName().Name + "$" +
                        D->getName().Name + "$iid").str();
      if (Imported)
        ImportedDecls.insert(D);
      continue;
    }
    auto *R = llvm::dyn_cast<RoutineDecl>(D);
    if (!llvm::isa<VarDecl>(D) && !(R && !R->isExternal() && !R->isGeneric()))
      continue;
    if (auto *V = llvm::dyn_cast<VarDecl>(D); V && V->isExternal())
      continue; // a C variable keeps its symbol
    ExportNames[D] = ("pas." + U.getName().Name + "$" + D->getName().Name).str() +
                     (R ? overloadSuffix(R) : std::string());
    if (Imported)
      ImportedDecls.insert(D);
  }
}


std::string CodeGenModule::initName(UnitDecl &U){
  return ("pas." + U.getName().Name + "$$init").str();
}


/// A unit-level or program variable's global (or a class variable's,
/// pas.<class>.<name>).
llvm::GlobalVariable *CodeGenModule::createGlobal(VarDecl *V, llvm::Type *Ty,
                                   llvm::Constant *Init){
  auto It = ExportNames.find(V);
  if (It == ExportNames.end())
    return new llvm::GlobalVariable(
        M, Ty, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage, Init,
        V->getVarClass() ? "pas." + V->getVarClass()->getDeclName().lower() +
                               "." + V->getName().Name.str()
                         : "pas." + V->getName().Name.str());
  bool Imported = ImportedDecls.count(V);
  auto *G = new llvm::GlobalVariable(M, Ty, /*isConstant=*/false,
                                     llvm::GlobalValue::ExternalLinkage,
                                     Imported ? nullptr : Init, It->second);
  return G;
}


//===--------------------------------------------------------------------===//
// Declarations
//===--------------------------------------------------------------------===//

/// Initial value of a typed constant (or zero).
llvm::Constant *CodeGenModule::initializerFor(VarDecl *V, llvm::Type *Ty){
  if (V->getInit())
    return constantFor(V->getInit(), V->getInitValue(), V->getType(), Ty);
  return llvm::Constant::getNullValue(Ty);
}


/// The initial value of static variable V (memory type Ty) when it is
/// created: one holding procedural values gets its value once the
/// routines are declared (finishStaticInits).
llvm::Constant *CodeGenModule::staticInitializer(VarDecl *V, llvm::Type *Ty){
  if (V->getInit() && containsRoutineType(V->getType())) {
    DeferredInits.push_back(V);
    return llvm::Constant::getNullValue(Ty);
  }
  return initializerFor(V, Ty);
}


void CodeGenModule::finishStaticInits(){
  for (VarDecl *V : DeferredInits)
    Globals[V]->setInitializer(initializerFor(V, memType(V->getType())));
  DeferredInits.clear();
}


/// A procedural constant: nil, a routine, or a lambda (with no frame and
/// no object), as {function, nil}.
llvm::Constant *CodeGenModule::routineConstant(Expr *Init, llvm::Type *Ty){
  RoutineDecl *R = nullptr;
  if (auto *A = llvm::dyn_cast<AddrOfExpr>(Init))
    R = A->getLambda() ? A->getLambda()
        : A->getCode() ? A->getCode()
                       : llvm::dyn_cast_or_null<RoutineDecl>(
                             llvm::cast<NameRefExpr>(A->getOperand())->getDecl());
  if (!R) // nil
    return llvm::Constant::getNullValue(Ty);
  return llvm::ConstantStruct::get(
      llvm::cast<llvm::StructType>(Ty),
      {llvm::cast<llvm::Constant>(routineFunction(R)),
       llvm::ConstantPointerNull::get(B.getPtrTy())});
}


void CodeGenModule::emitStaticVar(VarDecl *V){
  llvm::Type *Ty = memType(V->getType());
  bool Imported = ImportedDecls.count(V);
  Globals[V] =
      createGlobal(V, Ty, Imported ? nullptr : staticInitializer(V, Ty));
  if (DIB && !Imported)
    declareGlobalDebugInfo(V, Globals[V]);
}


void CodeGenModule::emitGlobals(Block &Blk){
  for (Decl *D : withNested(Blk.getDecls())) {
    ClassType *C = definedClass(D);
    if (!C)
      C = advancedRecord(D); // (an advanced record's, a helper's)
    if (C) { // class variables: no debug info
      for (Decl *M : C->getMembers())
        if (auto *V = llvm::dyn_cast<VarDecl>(M)) {
          llvm::Type *Ty = memType(V->getType());
          Globals[V] = createGlobal(V, Ty,
                                    V->isTypedConst()
                                        ? staticInitializer(V, Ty)
                                        : llvm::Constant::getNullValue(Ty));
        }
      continue;
    }
    auto *V = llvm::dyn_cast<VarDecl>(D);
    if (!V || V->getAbsolute())
      continue;
    if (V->isExternal()) { // a C variable: its symbol, defined elsewhere
      if (!isErrno(V)) {
        llvm::GlobalVariable *G = M.getNamedGlobal(V->getExternalName());
        if (!G)
          G = new llvm::GlobalVariable(M, memType(V->getType()),
                                       /*isConstant=*/false,
                                       llvm::GlobalValue::ExternalLinkage,
                                       nullptr, V->getExternalName());
        Globals[V] = G;
      }
      continue;
    }
    if (V->isTypedConst() || V->isInitializedVar()) { // initialized once
      emitStaticVar(V);
      continue;
    }
    llvm::Type *Ty = memType(V->getType());
    // ISO leaves variables undefined; zero-initializing makes programs
    // deterministic at no cost for globals.
    Globals[V] = createGlobal(V, Ty, llvm::Constant::getNullValue(Ty));
    if (DIB && !ImportedDecls.count(V))
      declareGlobalDebugInfo(V, Globals[V]);
  }
}


/// Creates LLVM functions and frame types for every routine in Blk
/// (recursively), and queues their bodies for emission.
/// Every routine takes its static link first (null at program level), so
/// procedural parameters can call any routine the same way.
llvm::FunctionType *CodeGenModule::routineFnType(llvm::ArrayRef<VarDecl *> Params,
                                  Type *Result, bool HasSelf,
                                  bool HasLink, uint32_t SliceMask){
  llvm::SmallVector<llvm::Type *, 8> ParamTys;
  if (HasLink)
    ParamTys.push_back(B.getPtrTy());
  // The result pointer is first or second so that it can be sret; Self
  // follows it (a method has a static link first).
  if (returnsByPointer(Result)) {
    ParamTys.push_back(B.getPtrTy());
    Result = nullptr;
  }
  if (HasSelf)
    ParamTys.push_back(B.getPtrTy());
  for (size_t I = 0; I < Params.size(); ++I) {
    if (I < 32 && (SliceMask & (1u << I))) {
      ParamTys.push_back(B.getPtrTy());
      ParamTys.push_back(B.getInt32Ty());
      continue;
    }
    VarDecl *P = Params[I];
    ParamTys.push_back(P->isVarParam() || P->getType()->isStructured() ||
                               P->getType()->isUntyped() || isConstRefPtr(P)
                           ? B.getPtrTy()
                           : regType(P->getType()));
  }
  llvm::Type *RetTy = Result ? regType(Result) : B.getVoidTy();
  return llvm::FunctionType::get(RetTy, ParamTys, false);
}


/// Does R take the hidden static-link first parameter? Only a nested
/// routine needs it; methods keep it for method pointers, as do async and
/// iterator routines (their argument record has a link slot) (item 20).
bool CodeGenModule::needsLink(RoutineDecl *R){
  return R->hasStaticLink() || R->hasSelf() || R->isMethod() ||
         R->isAsync() || R->isIterator();
}


llvm::FunctionType *CodeGenModule::routineFnType(RoutineDecl *R){
  return routineFnType(R->getParams(),
                       R->isFunction() ? R->getResultType() : nullptr,
                       R->hasSelf(), needsLink(R));
}


/// An external (C) routine: no static link, C types (a callback: a
/// function pointer), maybe variadic.
void CodeGenModule::declareExternal(RoutineDecl *R){
  llvm::SmallVector<llvm::Type *, 8> ParamTys;
  for (VarDecl *P : R->getParams())
    ParamTys.push_back(P->isVarParam() || P->getType()->isUntyped() ||
                               isConstRefPtr(P) ||
                               llvm::isa<RoutineType>(P->getType())
                           ? B.getPtrTy()
                           : regType(P->getType()));
  llvm::Type *RetTy = R->isFunction() ? regType(R->getResultType()) : B.getVoidTy();
  llvm::StringRef Sym = R->getExternalName().empty() ? R->getName().Spelling
                                                     : R->getExternalName();
  ExternalFns[R] = M.getOrInsertFunction(
      Sym, llvm::FunctionType::get(RetTy, ParamTys, R->isVarArgs()));
}


void CodeGenModule::declareCThunk(RoutineDecl *R){
  if (!R->isCDecl() || R->isMethod() || R->hasStaticLink())
    return;
  llvm::Function *Target = Functions[R];
  llvm::Type *Result = Target->getReturnType();
  llvm::SmallVector<llvm::Type *, 8> ParamTys(
      Target->getFunctionType()->param_begin() + (needsLink(R) ? 1 : 0),
      Target->getFunctionType()->param_end());
  auto *T = llvm::Function::Create(llvm::FunctionType::get(Result, ParamTys, false),
                                   llvm::Function::InternalLinkage,
                                   "cthunk." + Target->getName(), M);
  unsigned First = R->isFunction() && returnsByPointer(R->getResultType());
  if (First)
    addResultParamAttrs(T->getArg(0), R->getResultType());
  llvm::IRBuilder<> TB(llvm::BasicBlock::Create(M.getContext(), "entry", T));
  llvm::SmallVector<llvm::Value *, 8> Args;
  if (needsLink(R))
    Args.push_back(llvm::ConstantPointerNull::get(B.getPtrTy()));
  for (unsigned I = 0; I < T->arg_size(); ++I) {
    if (I >= First)
      T->getArg(I)->setName(R->getParams()[I - First]->getName().Name);
    Args.push_back(T->getArg(I));
  }
  auto *Call = TB.CreateCall(Target, Args);
  if (First)
    addResultCallAttrs(Call, needsLink(R) ? 1 : 0, R->getResultType());
  llvm::Value *V = Call;
  if (Result->isVoidTy())
    TB.CreateRetVoid();
  else
    TB.CreateRet(V);
  CThunks[R] = T;
}


/// The function a procedural value of routine R points to.
llvm::Value *CodeGenModule::routineFunction(RoutineDecl *R){
  if (R->isExternal())
    return ExternalFns[R].getCallee();
  if (llvm::Function *T = CThunks.lookup(R))
    return T;
  if (needsLink(R))
    return Functions[R];
  if (llvm::Function *T = LinkThunks.lookup(R))
    return T;
  llvm::Function *Target = Functions[R];
  auto *T = llvm::Function::Create(
      routineFnType(R->getParams(),
                    R->isFunction() ? R->getResultType() : nullptr, false,
                    /*HasLink=*/true),
      llvm::Function::InternalLinkage,
      "linkthunk." + Target->getName(), M);
  T->arg_begin()->setName("link");
  unsigned First = R->isFunction() && returnsByPointer(R->getResultType());
  if (First)
    addResultParamAttrs(T->getArg(1), R->getResultType());
  llvm::IRBuilder<> TB(llvm::BasicBlock::Create(M.getContext(), "entry", T));
  llvm::SmallVector<llvm::Value *, 8> Args;
  for (unsigned I = 0; I < T->arg_size(); ++I) {
    if (I == 0) // the link this thunk ignores
      continue;
    if (I - 1 >= First)
      T->getArg(I)->setName(R->getParams()[I - 1 - First]->getName().Name);
    Args.push_back(T->getArg(I));
  }
  auto *Call = TB.CreateCall(Target, Args);
  if (First)
    addResultCallAttrs(Call, 0, R->getResultType());
  llvm::Value *V = Call;
  if (V->getType()->isVoidTy())
    TB.CreateRetVoid();
  else
    TB.CreateRet(V);
  LinkThunks[R] = T;
  return T;
}


void CodeGenModule::declareRoutines(Block &Blk){
  for (Decl *D : withNested(Blk.getDecls())) {
    if (ClassType *C = definedClass(D)) {
      declareClass(D, C);
      continue;
    }
    if (InterfaceType *I = definedInterface(D)) {
      declareInterface(D, I);
      continue;
    }
    if (ClassType *MT = advancedRecord(D)) {
      // A record of a separately compiled unit: its methods, declared.
      for (Decl *M : MT->getMembers())
        if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && ImportedDecls.count(R)) {
          auto *F = llvm::Function::Create(routineFnType(R),
                                           llvm::Function::ExternalLinkage,
                                           ExportNames[R], this->M);
          addResultAttrs(F, R);
          Functions[R] = F;
        }
      continue;
    }
    auto *R = llvm::dyn_cast<RoutineDecl>(D);
    if (R && R->isRedeclaration() && R->getCanonical()->isMethod()) {
      declareRoutine(R->getCanonical()); // a method's implementation
      continue;
    }
    if (!R || R->isRedeclaration() || R->isGeneric())
      continue; // the forward declaration is the canonical node; a
                // generic is compiled as its specializations
    if (R->isExternal()) {
      declareExternal(R);
      continue;
    }
    if (ImportedDecls.count(R)) {
      // A routine of a separately compiled unit: declared only.
      auto *F = llvm::Function::Create(
          routineFnType(R), llvm::Function::ExternalLinkage, ExportNames[R], M);
      addResultAttrs(F, R);
      Functions[R] = F;
      declareCThunk(R);
      if (R->isAsync() || R->isIterator())
        declareAsync(R, ExportNames[R], /*External=*/true);
      continue;
    }
    declareRoutine(R);
  }
}


/// A by-reference parameter is an address: never nil, and it points to a
/// value of its type (item 9). LLVM cannot infer this across a call.
void CodeGenModule::addByRefParamAttrs(llvm::Argument *A, Type *T){
  llvm::AttrBuilder B(A->getContext());
  B.addAttribute(llvm::Attribute::NonNull);
  if (!T->isUntyped())
    B.addDereferenceableAttr(DL.getTypeStoreSize(memType(T)).getFixedValue());
  A->addAttrs(B);
}


/// The hidden result pointer is a fresh caller temporary (item 4): it
/// cannot alias an argument, and LLVM's ABI can return through it.
void CodeGenModule::addResultParamAttrs(llvm::Argument *A, Type *T){
  llvm::AttrBuilder B(A->getContext());
  B.addAttribute(llvm::Attribute::NoAlias);
  B.addStructRetAttr(memType(T));
  B.addAttribute(llvm::Attribute::NonNull);
  B.addDereferenceableAttr(DL.getTypeStoreSize(memType(T)).getFixedValue());
  A->addAttrs(B);
}


/// The same facts at a call site, where they change the ABI on targets
/// with an indirect-result convention.
void CodeGenModule::addResultCallAttrs(llvm::CallBase *CB, unsigned ArgNo, Type *T){
  CB->addParamAttr(ArgNo, llvm::Attribute::NoAlias);
  CB->addParamAttr(ArgNo,
                   llvm::Attribute::getWithStructRetType(B.getContext(),
                                                         memType(T)));
}


/// Marks the hidden result pointer of a function declaration, if it has
/// one.
void CodeGenModule::addResultAttrs(llvm::Function *F, RoutineDecl *R){
  if (!R->isFunction() || !returnsByPointer(R->getResultType()))
    return;
  unsigned ArgNo = needsLink(R) ? 1 : 0;
  addResultParamAttrs(F->getArg(ArgNo), R->getResultType());
}


/// Creates the LLVM function and frame type of routine R (a method: of
/// its canonical declaration), queues its body and declares the
/// routines nested in it.
void CodeGenModule::declareRoutine(RoutineDecl *R){
  // Frame record: [static link], then those of [Self], params, locals
  // and [result] that have to be in it (see inFrame). The link is there
  // for the routines nested in R, which follow it to R's parents.
  llvm::SmallVector<llvm::Type *, 8> Slots;
  bool HasNested = false;
  for (Decl *Local : R->getBody()->getDecls())
    if (auto *N = llvm::dyn_cast<RoutineDecl>(Local))
      HasNested |= !N->isExternal();
  if (R->hasStaticLink() && HasNested) {
    LinkInFrame.insert(R);
    Slots.push_back(B.getPtrTy());
  }
  bool PinAll = hasNonLocalLabels(*R->getBody());
  VarSet PinVars;
  if (!PinAll && R->hasTry())
    PinVars = PinAnalysis(R).run();
  auto SliceVars = findSliceLocals(R);
  if (!SliceVars.empty())
    RoutineSliceLocals[R] = SliceVars;
  bool BorrowAnsi = canBorrowAnsiParams(R);
  auto AddSlot = [&](VarDecl *V) {
    bool IsBorrowedVal = BorrowAnsi && V->isParam() && !isByRef(V) &&
                         !V->isConstParam() && !V->isVarParam() &&
                         !V->isOutParam() && V->getType()->isAnsiString() &&
                         !V->isCaptured();
    if (llvm::is_contained(SliceVars, V) || IsBorrowedVal ||
        !inFrame(V, PinAll, PinVars))
      return;
    FrameSlots[V] = Slots.size();
    Slots.push_back(isByRef(V) ? B.getPtrTy() : memType(V->getType()));
  };
  if (R->hasSelf())
    AddSlot(R->getSelfVar());
  for (VarDecl *P : R->getParams())
    AddSlot(P);
  for (Decl *Local : R->getBody()->getDecls())
    if (auto *V = llvm::dyn_cast<VarDecl>(Local)) {
      if (V->isTypedConst())
        emitStaticVar(V); // static storage, local scope
      else if (!V->getAbsolute())
        AddSlot(V);
    }
  if (R->isFunction())
    AddSlot(R->getResultVar());
  std::string Name = mangle(R);
  if (!Slots.empty())
    FrameTypes[R] = llvm::StructType::create(M.getContext(), Slots,
                                             "frame." + Name.substr(4));

  if (hasNonLocalLabels(*R->getBody())) {
    JmpBufSlots[R] = Slots.size();
    Slots.push_back(jmpBufType());
    FrameTypes[R] = llvm::StructType::create(M.getContext(), Slots,
                                             "frame." + Name.substr(4) + ".jb");
  }

  auto Export = ExportNames.find(R);
  auto *F = llvm::Function::Create(
      routineFnType(R),
      Export == ExportNames.end() ? llvm::Function::InternalLinkage
                                  : llvm::Function::ExternalLinkage,
      Export == ExportNames.end() ? Name : Export->second, M);
  unsigned ArgNo = 0;
  if (needsLink(R))
    F->getArg(ArgNo++)->setName("link");
  if (R->isFunction() && returnsByPointer(R->getResultType())) {
    llvm::Argument *Result = F->getArg(ArgNo++);
    Result->setName("result.ptr");
    addResultParamAttrs(Result, R->getResultType());
  }
  if (R->hasSelf()) {
    llvm::Argument *Self = F->getArg(ArgNo++);
    Self->setName("self");
    if (R->getClass() && R->getClass()->hasSelfByReference())
      addByRefParamAttrs(Self, R->getSelfVar()->getType());
  }
  for (VarDecl *P : R->getParams()) {
    llvm::Argument *A = F->getArg(ArgNo++);
    A->setName(P->getName().Name);
    if (isByRef(P))
      addByRefParamAttrs(A, P->getType());
  }
  if (R->isInline()) // the `inline` directive
    F->addFnAttr(llvm::Attribute::InlineHint);
  Functions[R] = F;
  declareCThunk(R);
  if (R->isAsync() || R->isIterator())
    declareAsync(R, Export == ExportNames.end() ? Name : Export->second,
                 Export != ExportNames.end());
  RoutinesToEmit.push_back(R);
  if (DIB)
    // File scope even for nested routines: lexical nesting would make
    // debuggers look up enclosing variables through DW_AT_static_link,
    // which LLVM cannot emit. Use the debugger's `up` to reach them.
    Subprograms[R] = createSubprogram(
        F,
        R->isMethod() ? R->getClass()->getName() + "." +
                            R->getName().Spelling.str()
                      : R->getName().Spelling.str(),
        diFileFor(R->getBodyLocation()), lineOf(R->getBodyLocation()),
        R->isFunction() ? R->getResultType() : nullptr, R->getParams(),
        diFileFor(R->getBodyLocation()),
        R->hasSelf() ? R->getSelfVar() : nullptr);

  declareRoutines(*R->getBody());
}


/// The variables of R's frame that hold managed values R releases when
/// it returns: its value parameters (one more reference each) and local
/// variables.
llvm::SmallVector<VarDecl *, 8> CodeGenModule::managedFrameVars(RoutineDecl *R){
  llvm::SmallVector<VarDecl *, 8> Vars;
  for (VarDecl *P : R->getParams())
    if (!isByRef(P) && !P->isConstParam() && !P->isVarParam() &&
        isManaged(P->getType()))
      Vars.push_back(P);
  for (Decl *D : R->getBody()->getDecls())
    if (auto *V = llvm::dyn_cast<VarDecl>(D);
        V && !V->isTypedConst() && !V->getAbsolute() &&
        isManaged(V->getType()))
      Vars.push_back(V);
  return Vars;
}


/// R's frame is on the heap (a closure made in it uses it): the function
/// that finalizes it when its last reference goes, frame.fini.<R>. It
/// releases the managed variables (as R does on its way out when its
/// frame is not on the heap) and the frame of R's parent, if that is on
/// the heap too (R's frame holds a reference to it, its static link).
llvm::Function *CodeGenModule::emitFrameFini(RoutineDecl *R){
  llvm::StructType *FrameTy = FrameTypes.lookup(R);
  auto *Fn = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false),
      llvm::Function::InternalLinkage, "frame.fini." + mangle(R).substr(4), M);
  Fn->getArg(0)->setName("frame");
  CurFn = Fn;
  B.SetCurrentDebugLocation(llvm::DebugLoc());
  B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", Fn));
  llvm::Value *Frame = Fn->getArg(0);
  for (VarDecl *V : managedFrameVars(R))
    finalizeValue(B.CreateStructGEP(FrameTy, Frame, FrameSlots[V]),
                  V->getType());
  if (parentFrameOnHeap(R))
    B.CreateCall(frameRuntime("__pas_frame_release"),
                 {B.CreateLoad(B.getPtrTy(), B.CreateStructGEP(FrameTy, Frame, 0),
                               "link")});
  B.CreateRetVoid();
  return Fn;
}


/// Does R's frame hold its static link, to a frame on the heap?
bool CodeGenModule::parentFrameOnHeap(RoutineDecl *R){
  return LinkInFrame.count(R) && R->getParent() &&
         R->getParent()->hasHeapFrame() && FrameTypes.lookup(R->getParent());
}


/// __pas_frame_new, __pas_frame_addref / release (runtime/io.c).
llvm::FunctionCallee CodeGenModule::frameRuntime(llvm::StringRef Name){
  if (Name == "__pas_frame_new")
    return classRuntime(Name, B.getPtrTy(), {B.getInt64Ty(), B.getPtrTy()});
  return classRuntime(Name, B.getVoidTy(), {B.getPtrTy()});
}


llvm::Function *CodeGenModule::getOrCreateSliceSpec(RoutineDecl *R,
                                                    uint32_t SliceMask){
  llvm::Function *&Slot = SliceSpecFns[{R, SliceMask}];
  if (Slot)
    return Slot;
  std::string SpecName = mangle(R) + "$slice$" + std::to_string(SliceMask);
  Slot = llvm::Function::Create(
      routineFnType(R->getParams(),
                    R->isFunction() ? R->getResultType() : nullptr,
                    R->hasSelf(), needsLink(R), SliceMask),
      llvm::Function::InternalLinkage, SpecName, M);
  unsigned ArgNo = needsLink(R) ? 1 : 0;
  if (R->isFunction() && returnsByPointer(R->getResultType()))
    addResultParamAttrs(Slot->getArg(ArgNo++), R->getResultType());
  if (R->hasSelf()) {
    llvm::Argument *Self = Slot->getArg(ArgNo++);
    Self->setName("self");
    if (R->getClass() && R->getClass()->hasSelfByReference())
      addByRefParamAttrs(Self, R->getSelfVar()->getType());
  }
  for (size_t I = 0; I < R->getParams().size(); ++I) {
    VarDecl *P = R->getParams()[I];
    if (I < 32 && (SliceMask & (1u << I))) {
      Slot->getArg(ArgNo++)->setName(P->getName().Name + ".ptr");
      Slot->getArg(ArgNo++)->setName(P->getName().Name + ".len");
      continue;
    }
    llvm::Argument *A = Slot->getArg(ArgNo++);
    A->setName(P->getName().Name);
    if (isByRef(P))
      addByRefParamAttrs(A, P->getType());
  }
  if (R->isInline())
    Slot->addFnAttr(llvm::Attribute::InlineHint);
  if (auto *Orig = Functions.lookup(R); Orig && NoRaiseFns.count(Orig))
    NoRaiseFns.insert(Slot);
  SliceSpecsToEmit.push_back({R, SliceMask, Slot});
  return Slot;
}


void CodeGenModule::emitSliceSpecs(){
  for (size_t I = 0; I < SliceSpecsToEmit.size(); ++I) {
    SliceSpec S = SliceSpecsToEmit[I];
    emitRoutineBody(S.R, S.Mask, S.F);
  }
  SliceSpecsToEmit.clear();
}


void CodeGenModule::emitRoutineBody(RoutineDecl *R, uint32_t SliceMask,
                                    llvm::Function *SpecFn){
  if (!SpecFn && (R->isAsync() || R->isIterator())) // (its task's parts, after it)
    AsyncsToEmit.push_back(R);
  // A frame on the heap: the function that finalizes it, first.
  llvm::Function *FrameFini =
      !SpecFn && FrameTypes.lookup(R) && R->hasHeapFrame() ? emitFrameFini(R)
                                                           : nullptr;
  llvm::Function *F = SpecFn ? SpecFn : Functions[R];
  CurFn = F;
  AddrAligns.clear();
  CurRoutine = R;
  CurSP = DIB && !SpecFn ? Subprograms[R] : nullptr;
  B.SetCurrentDebugLocation(llvm::DebugLoc());
  B.SetInsertPoint(llvm::BasicBlock::Create(M.getContext(), "entry", F));
  setDebugLoc(R->getBodyLocation());

  // The frame record, if any, then an alloca for each other variable.
  // On the heap if a closure made here uses it (counted: see
  // emitFrameFini), zeroed.
  llvm::StructType *FrameTy = FrameTypes.lookup(R);
  if (FrameFini)
    CurFrame = B.CreateCall(
        frameRuntime("__pas_frame_new"),
        {B.getInt64(DL.getTypeAllocSize(FrameTy)), FrameFini}, "frame");
  else
    CurFrame = FrameTy ? B.CreateAlloca(FrameTy, nullptr, "frame") : nullptr;
  CurPin = nullptr;
  // Execution continues here after an exception (see inFrame).
  PinTemps = R->hasTry() || hasNonLocalLabels(*R->getBody());
  if (CurFrame && PinTemps)
    pinInMemory(CurFrame);
  LocalAddrs.clear();
  SliceLocals.clear();
  SliceParams.clear();
  llvm::SmallVector<VarDecl *, 4> SliceVars;
  if (!SpecFn)
    SliceVars = RoutineSliceLocals.lookup(R);
  VarDecl *ResultVar = R->isFunction() ? R->getResultVar() : nullptr;
  llvm::SmallVector<VarDecl *, 8> Locals; // start from zero
  for (Decl *D : R->getBody()->getDecls())
    if (auto *V = llvm::dyn_cast<VarDecl>(D);
        V && !V->isTypedConst() && !V->getAbsolute()) {
      if (!llvm::is_contained(SliceVars, V))
        Locals.push_back(V);
    }
  if (ResultVar)
    Locals.push_back(ResultVar);
  auto AddLocal = [&](VarDecl *V, bool Param) {
    if (!FrameSlots.count(V))
      LocalAddrs[V] = B.CreateAlloca(
          isByRef(V) ? B.getPtrTy() : memType(V->getType()), nullptr,
          Param ? V->getName().Name + ".addr" : V->getName().Name);
  };
  if (R->hasSelf())
    AddLocal(R->getSelfVar(), true);
  for (size_t I = 0; I < R->getParams().size(); ++I)
    if (I >= 32 || !(SliceMask & (1u << I)))
      AddLocal(R->getParams()[I], true);
  for (VarDecl *V : Locals)
    AddLocal(V, false);
  for (VarDecl *V : SliceVars) {
    llvm::Value *PtrSlot = B.CreateAlloca(B.getPtrTy(), nullptr,
                                          V->getName().Name + ".ptr");
    llvm::Value *LenSlot = B.CreateAlloca(B.getInt32Ty(), nullptr,
                                          V->getName().Name + ".len");
    B.CreateStore(llvm::ConstantPointerNull::get(B.getPtrTy()), PtrSlot);
    B.CreateStore(B.getInt32(0), LenSlot);
    SliceLocals[V] = {PtrSlot, LenSlot};
  }
  if (DIB && CurSP)
    declareFrameVariables(R);
  bool ResultByPointer = ResultVar && isByRef(ResultVar);
  for (VarDecl *V : Locals)
    if (V != ResultVar || !ResultByPointer)
      emitZeroInit(varSlot(V), V->getType());

  unsigned ArgNo = needsLink(R) ? 1 : 0;
  if (LinkInFrame.count(R))
    B.CreateStore(F->getArg(0), B.CreateStructGEP(FrameTy, CurFrame, 0));
  if (FrameFini && parentFrameOnHeap(R)) // the frame holds its parent's
    B.CreateCall(frameRuntime("__pas_frame_addref"), {F->getArg(0)});
  if (ResultByPointer) { // the caller's temporary
    B.CreateStore(F->getArg(ArgNo++), varSlot(ResultVar));
    emitZeroInit(varAddress(ResultVar), ResultVar->getType());
  }
  if (R->hasSelf())
    B.CreateStore(F->getArg(ArgNo++), varSlot(R->getSelfVar()));
  // The variables holding COM interfaces: value parameters (one more
  // reference each), then local variables (nil at first).
  BorrowedAnsiParams.clear();
  if (canBorrowAnsiParams(R))
    for (size_t I = 0; I < R->getParams().size(); ++I) {
      VarDecl *P = R->getParams()[I];
      if ((I >= 32 || !(SliceMask & (1u << I))) && !isByRef(P) &&
          !P->isConstParam() && !P->isVarParam() && !P->isOutParam() &&
          P->getType()->isAnsiString() && !P->isCaptured())
        BorrowedAnsiParams.insert(P);
    }
  llvm::SmallVector<std::pair<llvm::Value *, Type *>, 4> Managed;
  for (size_t I = 0; I < R->getParams().size(); ++I) {
    VarDecl *P = R->getParams()[I];
    if (I < 32 && (SliceMask & (1u << I))) {
      llvm::Value *PtrArg = F->getArg(ArgNo++);
      llvm::Value *LenArg = F->getArg(ArgNo++);
      SliceParams[P] = {PtrArg, LenArg};
      continue;
    }
    llvm::Value *Arg = F->getArg(ArgNo++);
    llvm::Value *Slot = varSlot(P);
    if (isByRef(P))
      B.CreateStore(Arg, Slot);
    else if (auto *SS = llvm::dyn_cast<ShortStringType>(P->getType()))
      // value semantics: the characters in use, cut to fit
      B.CreateCall(RT.StrAssign, {Slot, B.getInt32(SS->getMaxLength()), Arg});
    else if (P->getType()->isStructured())
      copyAggregate(Slot, Arg, P->getType()); // value semantics
    else
      storeScalar(Slot, Arg, P->getType());
    if (auto *OA = llvm::dyn_cast<OpenArrayType>(P->getType());
        OA && !P->isVarParam() && !P->isConstParam())
      copyOpenArray(Slot, OA);
    if (!BorrowedAnsiParams.count(P) && !isByRef(P) && !P->isConstParam() &&
        !P->isVarParam() && isManaged(P->getType())) {
      addRefValue(Slot, P->getType());
      Managed.push_back({Slot, P->getType()});
    }
  }
  // Initialized variables (objfpc) get their initial values on each call.
  for (Decl *D : R->getBody()->getDecls())
    if (auto *V = llvm::dyn_cast<VarDecl>(D); V && V->isInitializedVar())
      B.CreateStore(initializerFor(V, memType(V->getType())), varAddress(V));
  for (Decl *D : R->getBody()->getDecls())
    if (auto *V = llvm::dyn_cast<VarDecl>(D);
        V && !V->isTypedConst() && !V->getAbsolute() &&
        !SliceLocals.count(V) && isManaged(V->getType()))
      Managed.push_back({varAddress(V), V->getType()});
  bool ManagedResult = ResultVar && isManaged(ResultVar->getType());

  initFileVars(R->getBody()->getDecls(), nullptr);
  if (JmpBufSlots.count(R))
    emitSetJmpDispatch(*R->getBody(),
                       B.CreateStructGEP(FrameTy, CurFrame, JmpBufSlots[R]));
  // An exception leaving the routine releases them too (and the result):
  // the body runs protected, in a block of its own. Whether it needs to
  // is only known once it is emitted (can anything it calls raise?), so
  // the block before it is finished afterwards.
  llvm::Value *ExcFrame = nullptr;
  llvm::BasicBlock *Raised = nullptr, *BeforeBody = nullptr, *Body = nullptr;
  if (!Managed.empty() || ManagedResult || R->usesManaged() || FrameFini) {
    BeforeBody = B.GetInsertBlock();
    Body = newBlock("managed.body");
    B.SetInsertPoint(Body);
  }
  RoutineTemps.clear();
  ExitBlock = newBlock("exit");
  // The owned variables' objects are freed however the body ends, the
  // last declared first: the body is the protected part of a try-finally.
  llvm::SmallVector<VarDecl *, 4> Owned;
  for (Decl *D : R->getBody()->getDecls())
    if (auto *V = llvm::dyn_cast<VarDecl>(D);
        V && V->isOwned() && !V->isInline()) // (those: with their sequence)
      Owned.push_back(V);
  if (!R->isAsync() && !R->isIterator())
    initRangeLocals(R);
  bool SavedTail = AtRoutineTail;
  AtRoutineTail = false;
  if (Owned.empty()) {
    AtRoutineTail = R->isFunction() && !R->isAsync() && !R->isIterator() &&
                    Cleanups.empty() && !hasNonLocalLabels(*R->getBody());
    visitStmt(R->getBody()->getBody());
    AtRoutineTail = false;
  } else {
    emitTryFinally([&] { visitStmt(R->getBody()->getBody()); },
                   [&](llvm::Value *State) { emitFreeOwned(Owned, State); });
  }
  AtRoutineTail = SavedTail;
  endRangeLocals();
  B.CreateBr(ExitBlock);
  if (BeforeBody) {
    llvm::DebugLoc Loc = B.getCurrentDebugLocation();
    B.SetInsertPoint(BeforeBody);
    setDebugLoc(R->getBodyLocation());
    // Only when the landing has something to release: with nothing, it
    // would only raise again, as the exception does without it (and the
    // frame costs a setjmp on every call).
    bool Releases = !Managed.empty() || ManagedResult ||
                    !RoutineTemps.empty() || FrameFini;
    if (Releases && mayRaise()) {
      if (!PinTemps) { // what the landing releases
        if (CurFrame)
          pinInMemory(CurFrame);
        for (llvm::Value *Slot : RoutineTemps)
          pinInMemory(Slot);
      }
      Raised = newBlock("managed.raise");
      ExcFrame = emitProtect(Raised, "", Body);
    } else {
      B.CreateBr(Body);
    }
    B.SetCurrentDebugLocation(Loc);
  }
  B.SetInsertPoint(ExitBlock);
  // (A frame on the heap: its variables when its last reference goes.)
  auto Finalize = [&] {
    if (!FrameFini)
      for (auto &[Addr, T] : Managed)
        finalizeValue(Addr, T);
    for (llvm::Value *Slot : RoutineTemps)
      clearTemp(Slot);
  };
  auto ReleaseFrame = [&] {
    if (FrameFini)
      B.CreateCall(frameRuntime("__pas_frame_release"), {CurFrame});
  };
  if (ExcFrame)
    B.CreateCall(excRuntime("__pas_exc_pop"), {ExcFrame});
  Finalize();
  for (Decl *D : R->getBody()->getDecls())
    if (auto *V = llvm::dyn_cast<VarDecl>(D);
        V && !V->getAbsolute() && !SliceLocals.count(V))
      emitFileLifetime(varAddress(V), V->getType(), /*Init=*/false);

  if (Raised) {
    llvm::BasicBlock *Ret = B.GetInsertBlock();
    B.SetInsertPoint(Raised);
    Finalize();
    if (ManagedResult)
      finalizeValue(varAddress(ResultVar), ResultVar->getType());
    ReleaseFrame();
    B.CreateCall(excRuntime("__pas_reraise"), {ExcFrame});
    B.CreateUnreachable();
    B.SetInsertPoint(Ret);
  }
  RoutineTemps.clear();

  if (R->isFunction() && !ResultByPointer) {
    llvm::Value *Result =
        loadScalar(varSlot(ResultVar), ResultVar->getType(), "result");
    ReleaseFrame();
    B.CreateRet(Result);
  } else {
    ReleaseFrame();
    B.CreateRetVoid();
  }
  if (!mayRaise())
    NoRaiseFns.insert(F);
  BorrowedAnsiParams.clear();
  SliceLocals.clear();
  SliceParams.clear();
}

CodeGenModule::CodeGenModule(llvm::Module &M, const SourceManager &SM,
              const LangOptions &LangOpts, const CodeGenOptions &CGOpts): M(M), DL(M.getDataLayout()), B(M.getContext()), RT(M), SM(SM),
      LangOpts(LangOpts), CGOpts(CGOpts) {
  if (CGOpts.FastMath) { // on the FP instructions IRBuilder creates
    llvm::FastMathFlags FMF;
    FMF.setFast();
    B.setFastMathFlags(FMF);
  }
  for (const llvm::Function &F : M) // RT's declarations
    RuntimeFns.insert(&F);
}


void CodeGenModule::emitProgram(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units){
  llvm::LLVMContext &C = M.getContext();
  auto *MainTy = llvm::FunctionType::get(
      B.getInt32Ty(), {B.getInt32Ty(), llvm::PointerType::getUnqual(C)},
      false);
  llvm::Function *Main = llvm::Function::Create(
      MainTy, llvm::Function::ExternalLinkage, "main", M);
  Main->getArg(0)->setName("argc");
  Main->getArg(1)->setName("argv");

  Block &Blk = *P.getBlock();
  bool Separate = CGOpts.SeparateUnits;
  if (CGOpts.DebugInfo)
    initDebugInfo(M.getSourceFileName());
  if (Separate)
    for (UnitDecl *U : Units)
      collectExports(*U, /*Imported=*/true);
  for (UnitDecl *U : Units) {
    emitGlobals(*U->getInterface());
    if (!Separate)
      emitGlobals(*U->getImplementation());
  }
  emitGlobals(Blk);
  if (hasNonLocalLabels(Blk)) { // needed by gotos in routine bodies
    MainJmpBuf = new llvm::GlobalVariable(
        M, jmpBufType(), false, llvm::GlobalValue::InternalLinkage,
        llvm::Constant::getNullValue(jmpBufType()), "pas.jmpbuf");
    MainJmpBuf->setAlignment(llvm::Align(16));
  }
  for (UnitDecl *U : Units) {
    declareRoutines(*U->getInterface());
    if (!Separate)
      declareRoutines(*U->getImplementation());
  }
  declareRoutines(Blk);
  finishStaticInits();
  defineVMTs();
  for (RoutineDecl *R : RoutinesToEmit)
    emitRoutineBody(R);
  emitSliceSpecs();
  for (RoutineDecl *R : AsyncsToEmit)
    emitAsyncParts(R);
  AsyncsToEmit.clear();
  emitCleanups();

  CurFn = Main;

  AddrAligns.clear();
  CurRoutine = nullptr;
  CurFrame = nullptr;
  CurPin = nullptr;
  bool NeedsPin = hasNonLocalLabels(Blk) || stmtHasTry(Blk.getBody());
  if (!Separate)
    for (UnitDecl *U : Units)
      if (hasNonLocalLabels(*U->getImplementation()) ||
          stmtHasTry(U->getImplementation()->getBody()))
        NeedsPin = true;
  PinTemps = NeedsPin;
  CurSP = DIB ? createSubprogram(Main, "main", DIUnitFile,
                                 lineOf(P.getLocation()), nullptr, {})
              : nullptr;
  B.SetCurrentDebugLocation(llvm::DebugLoc());
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Main));
  setDebugLoc(P.getLocation());
  B.CreateCall(RT.Init, {Main->getArg(0), Main->getArg(1)});
  if (LangOpts.Mode != LangMode::ISO)
    B.CreateStore(B.getInt32(1),
                  new llvm::GlobalVariable(M, B.getInt32Ty(), false,
                                           llvm::GlobalValue::ExternalLinkage,
                                           nullptr, "__pas_tp_mode"));
  // Program parameters other than input/output bind to argv[1], ...
  llvm::StringMap<unsigned> ParamIndex;
  for (const Identifier &Param : P.getParams())
    if (Param.Name != "input" && Param.Name != "output")
      ParamIndex.try_emplace(Param.Name, ParamIndex.size() + 1);
  if (Separate) {
    // Separately compiled units initialize themselves, in uses order.
    for (UnitDecl *U : Units)
      B.CreateCall(M.getOrInsertFunction(
          initName(*U), llvm::FunctionType::get(B.getVoidTy(), false)));
  } else {
    // Units: their files, then their initialization parts, in uses order.
    for (UnitDecl *U : Units) {
      initFileVars(U->getInterface()->getDecls(), nullptr);
      initFileVars(U->getImplementation()->getDecls(), nullptr);
    }
    for (UnitDecl *U : Units) {
      emitClassInits(U->getInterface()->getDecls(),
                     U->getImplementation()->getDecls());
      visitStmt(U->getImplementation()->getBody());
    }
  }
  initFileVars(Blk.getDecls(), &ParamIndex);
  if (MainJmpBuf)
    emitSetJmpDispatch(Blk, MainJmpBuf);
  emitClassInits(Blk.getDecls(), {});
  ExitBlock = newBlock("exit");
  visitStmt(Blk.getBody());
  B.CreateBr(ExitBlock);
  B.SetInsertPoint(ExitBlock);
  B.CreateCall(RT.Fini);
  B.CreateRet(B.getInt32(0));
  emitDynOps();
  markDefinedFunctionsNounwind();
  if (DIB)
    DIB->finalize();
  if (!CGOpts.KeepUnused)
    removeUnreachable();
}


/// Nothing unwinds through p3's frames: exceptions are _setjmp/_longjmp
/// based (item 10). LLVM cannot infer it for calls through procedural
/// values, virtual methods or separately compiled units.
void CodeGenModule::markDefinedFunctionsNounwind(){
  bool TargetAttrs = CGOpts.TargetCPU != "generic" ||
                      !CGOpts.TargetFeatures.empty();
  for (llvm::Function &F : M)
    if (!F.isDeclaration()) {
      F.addFnAttr(llvm::Attribute::NoUnwind);
      if (CGOpts.SizeLevel == 1)
        F.addFnAttr(llvm::Attribute::OptimizeForSize);
      else if (CGOpts.SizeLevel == 2)
        F.addFnAttr(llvm::Attribute::MinSize);
      if (TargetAttrs) {
        F.addFnAttr("target-cpu", CGOpts.TargetCPU);
        if (!CGOpts.TargetFeatures.empty())
          F.addFnAttr("target-features", CGOpts.TargetFeatures);
      }
    }
}


/// Removes the definitions with internal linkage (routines, constants,
/// variables) that nothing reachable refers to. Reachable: the
/// definitions with external linkage (main, a unit's initialization and
/// interface), and what the code of a reachable function or the
/// initializer of a reachable variable refers to. So the routines of a
/// used unit that the program does not call are not compiled to machine
/// code, nor are the methods of a class whose VMT nothing refers to. All
/// routines are still generated, in declaration order: what is known
/// about a routine emitted earlier (can it raise?) stays the same, and so
/// does the code of the routines that remain.
void CodeGenModule::removeUnreachable(){
  llvm::SmallPtrSet<const llvm::GlobalValue *, 32> Live;
  llvm::SmallPtrSet<const llvm::Constant *, 32> Seen;
  llvm::SmallVector<const llvm::GlobalValue *, 64> Work;
  llvm::SmallVector<const llvm::Value *, 16> Pending;
  auto Refer = [&](const llvm::Value *Root) {
    Pending.push_back(Root);
    while (!Pending.empty()) {
      const llvm::Value *V = Pending.pop_back_val();
      if (auto *G = llvm::dyn_cast<llvm::GlobalValue>(V)) {
        if (Live.insert(G).second)
          Work.push_back(G);
      } else if (auto *K = llvm::dyn_cast<llvm::Constant>(V);
                 K && !llvm::isa<llvm::ConstantData>(K) &&
                 Seen.insert(K).second) {
        for (const llvm::Use &U : K->operands())
          Pending.push_back(U.get());
      }
    }
  };
  auto Removable = [](const llvm::GlobalValue &G) {
    return G.hasLocalLinkage() && !G.isDeclaration();
  };
  for (const llvm::Function &F : M)
    if (!F.isDeclaration() && !Removable(F))
      Refer(&F);
  for (const llvm::GlobalVariable &G : M.globals())
    if (!G.isDeclaration() && !Removable(G))
      Refer(&G);
  while (!Work.empty()) {
    const llvm::GlobalValue *G = Work.pop_back_val();
    if (auto *F = llvm::dyn_cast<llvm::Function>(G)) {
      for (const llvm::BasicBlock &BB : *F)
        for (const llvm::Instruction &I : BB)
          for (const llvm::Use &U : I.operands())
            Refer(U.get());
    } else if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(G)) {
      if (GV->hasInitializer())
        Refer(GV->getInitializer());
    }
  }
  llvm::SmallVector<llvm::Function *, 64> DeadFns;
  llvm::SmallVector<llvm::GlobalVariable *, 64> DeadVars;
  for (llvm::Function &F : M)
    if (Removable(F) && !Live.count(&F))
      DeadFns.push_back(&F);
  for (llvm::GlobalVariable &G : M.globals())
    if (Removable(G) && !Live.count(&G))
      DeadVars.push_back(&G);
  // They may refer to each other: drop the references first.
  for (llvm::Function *F : DeadFns)
    F->deleteBody();
  for (llvm::GlobalVariable *G : DeadVars)
    G->setInitializer(nullptr);
  for (llvm::Function *F : DeadFns) {
    F->removeDeadConstantUsers();
    F->eraseFromParent();
  }
  for (llvm::GlobalVariable *G : DeadVars) {
    G->removeDeadConstantUsers();
    G->eraseFromParent();
  }
}


/// The start of a program or unit (Decls, then More: its declarations):
/// the class constructors of the classes and advanced records it
/// declares (nested ones too), in declaration order; then its class
/// destructors are registered with the runtime, which runs them at exit
/// (last registered first: the program's, then the units' in reverse
/// order, each one's in declaration order, as in Free Pascal).
void CodeGenModule::emitClassInits(llvm::ArrayRef<Decl *> Decls, llvm::ArrayRef<Decl *> More){
  llvm::SmallVector<RoutineDecl *, 4> Dtors;
  auto Visit = [&](llvm::ArrayRef<Decl *> Ds) {
    for (Decl *D : withNested(Ds)) {
      ClassType *C = definedClass(D);
      if (!C)
        C = advancedRecord(D);
      if (!C || ImportedDecls.count(D))
        continue;
      if (RoutineDecl *R = C->getClassConstructor(); R && Functions.count(R))
        B.CreateCall(Functions[R],
                     {llvm::ConstantPointerNull::get(B.getPtrTy())});
      if (RoutineDecl *R = C->getClassDestructor(); R && Functions.count(R))
        Dtors.push_back(R);
    }
  };
  Visit(Decls);
  Visit(More);
  for (RoutineDecl *R : llvm::reverse(Dtors))
    B.CreateCall(classRuntime("__pas_at_exit", B.getVoidTy(), {B.getPtrTy()}),
                 {Functions[R]});
}


/// A unit compiled on its own: its variables and routines, and its
/// initialization part as pas.<unit>$$init (files, then the statements).
void CodeGenModule::emitUnit(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps){
  llvm::LLVMContext &C = M.getContext();
  llvm::Function *Init = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), false),
      llvm::Function::ExternalLinkage, initName(U), M);
  if (CGOpts.DebugInfo)
    initDebugInfo(M.getSourceFileName());
  for (UnitDecl *D : Deps)
    collectExports(*D, /*Imported=*/true);
  collectExports(U, /*Imported=*/false);
  for (UnitDecl *D : Deps)
    emitGlobals(*D->getInterface());
  emitGlobals(*U.getInterface());
  emitGlobals(*U.getImplementation());
  for (UnitDecl *D : Deps)
    declareRoutines(*D->getInterface());
  declareRoutines(*U.getInterface());
  declareRoutines(*U.getImplementation());
  finishStaticInits();
  defineVMTs();
  for (RoutineDecl *R : RoutinesToEmit)
    emitRoutineBody(R);
  emitSliceSpecs();
  for (RoutineDecl *R : AsyncsToEmit)
    emitAsyncParts(R);
  AsyncsToEmit.clear();
  emitCleanups();

  CurFn = Init;

  AddrAligns.clear();
  CurRoutine = nullptr;
  CurFrame = nullptr;
  CurPin = nullptr;
  PinTemps = hasNonLocalLabels(*U.getImplementation()) ||
             stmtHasTry(U.getImplementation()->getBody());
  CurSP = DIB ? createSubprogram(Init, U.getName().Spelling, DIUnitFile,
                                 lineOf(U.getLocation()), nullptr, {})
              : nullptr;
  B.SetCurrentDebugLocation(llvm::DebugLoc());
  B.SetInsertPoint(llvm::BasicBlock::Create(C, "entry", Init));
  setDebugLoc(U.getLocation());
  initFileVars(U.getInterface()->getDecls(), nullptr);
  initFileVars(U.getImplementation()->getDecls(), nullptr);
  emitClassInits(U.getInterface()->getDecls(),
                 U.getImplementation()->getDecls());
  ExitBlock = newBlock("exit");
  visitStmt(U.getImplementation()->getBody());
  B.CreateBr(ExitBlock);
  B.SetInsertPoint(ExitBlock);
  B.CreateRetVoid();
  emitDynOps();
  markDefinedFunctionsNounwind();
  if (DIB)
    DIB->finalize();
  if (!CGOpts.KeepUnused)
    removeUnreachable();
}

std::unique_ptr<llvm::Module>
p3::generateModule(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units,
                    const SourceManager &SM,
                    const LangOptions &LangOpts, const CodeGenOptions &CGOpts,
                    llvm::LLVMContext &Ctx, llvm::TargetMachine &TM,
                    llvm::StringRef SourceFileName) {
  auto M = std::make_unique<llvm::Module>(SourceFileName, Ctx);
  M->setSourceFileName(SourceFileName);
  M->setTargetTriple(TM.getTargetTriple());
  M->setDataLayout(TM.createDataLayout());
  CodeGenModule(*M, SM, LangOpts, CGOpts).emitProgram(P, Units);
  return M;
}

std::unique_ptr<llvm::Module>
p3::generateUnitModule(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps,
                        const SourceManager &SM, const LangOptions &LangOpts,
                        const CodeGenOptions &CGOpts, llvm::LLVMContext &Ctx,
                        llvm::TargetMachine &TM, llvm::StringRef SourceFileName) {
  auto M = std::make_unique<llvm::Module>(SourceFileName, Ctx);
  M->setSourceFileName(SourceFileName);
  M->setTargetTriple(TM.getTargetTriple());
  M->setDataLayout(TM.createDataLayout());
  CodeGenModule(*M, SM, LangOpts, CGOpts).emitUnit(U, Deps);
  return M;
}
