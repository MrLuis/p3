// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- SemaGenerics.cpp - Semantic analysis -------------------------------------===//

#include "Checker.h"

using namespace p3;
using namespace p3::detail;


/// Resolves a routine's parameter and result types.
void Checker::resolveSignature(RoutineDecl *R){
  checkAttributes(R);
  for (VarDecl *P : R->getParams()) {
    P->setOwner(R);
    P->setType(resolveType(P->getTypeSpec()));
    checkParamDefault(P);
    if (P->getType() && P->getType()->isUntyped())
      warnUnsafe(P->getLocation(),
                 "untyped parameter '" + P->getName().Spelling + "'",
                 {P->getName().getRange()});
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
    // Turbo/FPC also return strings, records and arrays (without files).
    if (!ResultTy->isError() && !ResultTy->isValidResultType() &&
        !ResultTy->isShortString() && !isMethodPointer(ResultTy) &&
        !(!isISO() && (ResultTy->isRecord() || ResultTy->isArray()) &&
          !containsFile(ResultTy))) {
      Diags.error(R->getResultTypeSpec()->getBeginLoc(),
                  "function result type must be a simple or pointer type, not " +
                      quoted(ResultTy),
                  {R->getResultTypeSpec()->getRange()});
      ResultTy = errorType();
    } else if (!ResultTy->isError() && ResultTy->isRecord() &&
               containsInterface(ResultTy)) {
      Diags.error(R->getResultTypeSpec()->getBeginLoc(),
                  "a function cannot return a record containing COM "
                  "interfaces",
                  {R->getResultTypeSpec()->getRange()});
      ResultTy = errorType();
    }
  }
  // iterator T: an iterator routine, whose body is a procedure's that
  // yields T values.
  if (auto *IT = llvm::dyn_cast<IteratorType>(ResultTy)) {
    R->setIterator(IT);
    R->setFunction(false);
    if (R->isExternal())
      Diags.error(R->getName().Loc,
                  "an external routine cannot be an iterator",
                  {R->getName().getRange()});
    return;
  }
  // async T / async: an async routine, whose body is a function's
  // returning T or a procedure's.
  if (auto *TT = llvm::dyn_cast<TaskType>(ResultTy)) {
    R->setAsync(TT);
    if (R->isExternal())
      Diags.error(R->getName().Loc,
                  "an external routine cannot be async",
                  {R->getName().getRange()});
    if (!TT->getResult()) {
      R->setFunction(false);
      return;
    }
    ResultTy = TT->getResult();
    if (!ResultTy->isError() && !ResultTy->isValidResultType() &&
        !ResultTy->isShortString() &&
        !(ResultTy->isRecord() && !containsFile(ResultTy))) {
      Diags.error(R->getResultTypeSpec()->getBeginLoc(),
                  "the result of a task cannot be " + quoted(ResultTy),
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


/// An async routine's restrictions: its task may outlive its caller, so
/// its arguments are copied (no var, out or constref parameters, no open
/// or untyped ones), and it is not nested (but a spawn block, which its
/// scope waits for) nor a virtual method, constructor or destructor.
/// (The same for an iterator routine, whose iterator may outlive its
/// caller too.)
void Checker::checkAsyncRoutine(RoutineDecl *R){
  const Identifier &Id = R->getName();
  bool It = R->isIterator();
  std::string Kind = It ? "iterator" : "async";
  const char *Its = It ? "its iterator" : "its task";
  if (R->getParent() && !R->isSpawnBlock() && !R->isLambda())
    Diags.error(Id.Loc,
                Kind + " routine '" + Id.Spelling +
                    "' must be declared at program or unit level, or be a "
                    "method (" + Its + " may outlive the routine around it)",
                {Id.getRange()});
  if (R->isMethod()) {
    std::string Why;
    if (R->isConstructor() || R->isDestructor())
      Why = It ? "a constructor or a destructor cannot be an iterator"
               : "a constructor or a destructor cannot be async";
    else if (R->isVirtual() || R->isAbstract() || R->isOverride() ||
             R->isDispatched())
      Why = "an " + Kind + " method cannot be virtual (not supported yet)";
    else if (R->getClass() && R->getClass()->hasSelfByReference())
      Why = It ? "a record's or helper's method cannot be an iterator (its "
                 "Self is passed by reference)"
               : "a record's or helper's method cannot be async (its Self is "
                 "passed by reference)";
    if (!Why.empty())
      Diags.error(Id.Loc, Why, {Id.getRange()});
  }
  for (VarDecl *P : R->getParams()) {
    Type *T = P->getType();
    const char *PKind = P->isOutParam()        ? "an 'out'"
                       : P->isVarParam()      ? "a 'var'"
                       : P->isConstRefParam() ? "a 'constref'"
                       : T && (T->isOpenArray() || T->isArrayOfConst())
                           ? "an open array"
                       : T && T->isUntyped() ? "an untyped"
                                             : nullptr;
    if (PKind)
      Diags.error(P->getLocation(),
                  "parameter '" + P->getName().Spelling + "' of " + Kind +
                      " routine '" + Id.Spelling + "' cannot be " + PKind +
                      " parameter: " + Its + " may outlive the caller's "
                      "variable; pass a value",
                  {P->getName().getRange()});
  }
}


/// CompareOut false: `out` and `var` parameters count as the same, and
/// `constref` and `const` ones, as
/// when looking for routines that overloads cannot tell apart.
bool Checker::sameSignature(RoutineDecl *A, RoutineDecl *B, bool CompareOut){
  if (A->getParams().size() != B->getParams().size())
    return false;
  for (size_t I = 0; I < A->getParams().size(); ++I) {
    VarDecl *P = A->getParams()[I], *Q = B->getParams()[I];
    if (P->getVarKind() != Q->getVarKind() || P->getType() != Q->getType() ||
        (CompareOut && (P->isOutParam() != Q->isOutParam() ||
                        P->isConstRefParam() != Q->isConstRefParam())))
      return false;
  }
  return true;
}


/// The word for a by-reference parameter, for diagnostics.
const char *Checker::paramKeyword(VarDecl *P){
  return P->isOutParam() ? "out" : "var";
}


/// The scope key of a generic routine: its name and type parameter count
/// (F<1>), so it does not clash with routines named F.
llvm::StringRef Checker::genericKey(llvm::StringRef Name, size_t N){
  return Ctx.getStrings().intern((Name + "<" + llvm::Twine(N) + ">").str());
}


/// generic function F<T>...: declared (under its key), not checked.
void Checker::declareGeneric(RoutineDecl *R){
  const Identifier &Id = R->getName();
  if (R->getClassName().isValid() && !CurRoutine)
    return declareGenericMethodBody(R);
  if (CurRoutine) {
    Diags.error(Id.Loc,
                "generic routines must be declared at program or unit level",
                {Id.getRange()});
    return;
  }
  auto Params = R->getTypeParams();
  for (size_t I = 0; I < Params.size(); ++I)
    for (size_t J = 0; J < I; ++J)
      if (Params[I].Name == Params[J].Name)
        Diags.error(Params[I].Loc,
                    "duplicate type parameter '" + Params[I].Spelling + "'",
                    {Params[I].getRange()});
  llvm::StringRef Key = genericKey(Id.Name, Params.size());
  auto *Prev = llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookupOwn(Key));
  if (!Prev && CurUnitInterface)
    Prev = llvm::dyn_cast_or_null<RoutineDecl>(CurUnitInterface->lookupOwn(Key));
  R->setGenericScope(Cur);
  Used.insert(R);
  if (Prev && !Prev->getBody() && Prev->getGenericDefinition() == Prev &&
      R->getBody()) { // the definition of a forward or interface heading
    Prev->setGenericDefinition(R);
    Prev->setGenericScope(Cur);
    return;
  }
  if (Prev) {
    Diags.error(Id.Loc,
                "redefinition of generic " + llvm::Twine(R->getKindName()) +
                    " '" + Id.Spelling + "'",
                {Id.getRange()});
    Diags.note(Prev->getLocation(), "previous definition is here");
    return;
  }
  Cur->insert(Key, R);
}


/// generic function TFoo.M<T>(...) begin ... end: the definition of the
/// generic method M<N> of TFoo; its heading must match the declaration's
/// in its shape (type parameters, parameters' kinds and number, function
/// or procedure), the types being checked in each specialization.
void Checker::declareGenericMethodBody(RoutineDecl *R){
  const Identifier &CN = R->getClassName(), &Id = R->getName();
  ClassType *C = nullptr;
  if (!R->getOuterClassNames().empty()) {
    auto *TD = llvm::dyn_cast_or_null<TypeDecl>(
        lookupMemberPath(R->getOuterClassNames(), CN));
    C = TD ? llvm::dyn_cast<ClassType>(TD->getType()) : nullptr;
    if (!C && TD)
      C = memberTableOf(TD->getType()); // an advanced record
  } else {
    if (auto *TD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(CN.Name)))
      C = memberTableOf(TD->getType()); // an advanced record
    if (!C)
      C = lookupClass(CN);
  }
  if (!C)
    return;
  auto Params = R->getTypeParams();
  auto *M = llvm::dyn_cast_or_null<RoutineDecl>(
      C->findOwnMember(genericKey(Id.Name, Params.size())));
  if (!M) {
    Diags.error(Id.Loc,
                llvm::Twine(C->getRecord()  ? "record"
                            : C->isHelper() ? C->helperKindName()
                                            : "class") +
                    " '" + C->getName() + "' has no generic method '" +
                    Id.Spelling + "' with " + llvm::Twine(Params.size()) +
                    " type parameter" + (Params.size() == 1 ? "" : "s"),
                {Id.getRange()});
    return;
  }
  std::string QName = qualified(C, M->getName());
  if (C->getModule() != CurModule) {
    Diags.error(Id.Loc,
                "method '" + QName +
                    "' must be implemented where its class is declared",
                {Id.getRange()});
    return;
  }
  if (M->getGenericDefinition() != M) {
    Diags.error(Id.Loc, "redefinition of '" + QName + "'", {Id.getRange()});
    return;
  }
  bool Same = M->isFunction() == R->isFunction() &&
              M->isClassMethod() == R->isClassMethod() &&
              M->getParams().size() == R->getParams().size();
  for (size_t I = 0; Same && I < Params.size(); ++I)
    Same = Params[I].Name == M->getTypeParams()[I].Name;
  for (size_t I = 0; Same && I < R->getParams().size(); ++I) {
    VarDecl *A = M->getParams()[I], *B = R->getParams()[I];
    Same = A->getVarKind() == B->getVarKind() &&
           A->isOutParam() == B->isOutParam() &&
           A->isConstRefParam() == B->isConstRefParam() &&
           A->getName().Name == B->getName().Name;
  }
  if (!Same) {
    Diags.error(Id.Loc,
                "heading of '" + QName + "' does not match its declaration",
                {Id.getRange()});
    noteDeclaredHere(M);
    return;
  }
  R->setGenericScope(Cur);
  R->setClass(C);
  M->setGenericDefinition(R);
  M->setGenericScope(Cur);
  Used.insert(R);
}


/// The specialization of generic method G (of its class) for type
/// arguments Types: its implementation parsed again, named M<types>, a
/// method of the class (not a member) with G's directives; its heading
/// resolved now, its body checked later. Null after a diagnostic.
RoutineDecl *Checker::instantiateMethod(const Identifier &Name, RoutineDecl *G,
                               llvm::ArrayRef<Type *> Types){
  Used.insert(G);
  ClassType *C = G->getClass();
  RoutineDecl *Def = G->getGenericDefinition();
  if (Def == G) {
    Diags.error(Name.Loc,
                "generic method '" + qualified(C, G->getName()) +
                    "' is used before its implementation",
                {Name.getRange()});
    noteDeclaredHere(G);
    return nullptr;
  }
  auto Key = std::make_pair(G, std::vector<Type *>(Types.begin(), Types.end()));
  if (auto It = Instances.find(Key); It != Instances.end())
    return It->second;
  RoutineDecl *I = Reparse ? Reparse(Def) : nullptr;
  if (!I) {
    if (!Reparse)
      Diags.error(Name.Loc, "generic methods cannot be specialized here",
                  {Name.getRange()});
    return nullptr;
  }
  std::string N = (G->getName().Name + "<").str();
  std::string S = (G->getName().Spelling + "<").str();
  for (size_t K = 0; K < Types.size(); ++K) {
    if (K) {
      N += ",";
      S += ", ";
    }
    N += llvm::StringRef(Types[K]->getName()).lower();
    S += Types[K]->getName();
  }
  N += ">";
  S += ">";
  I->setName(Identifier{Ctx.getStrings().intern(N), Ctx.getStrings().intern(S),
                        Def->getName().Loc});
  Instances[Key] = I;
  // Its scope: the implementation's, where the type parameters name the
  // types.
  auto Sc = std::make_unique<Scope>(static_cast<Scope *>(Def->getGenericScope()));
  for (size_t K = 0; K < Types.size(); ++K) {
    const Identifier &P = Def->getTypeParams()[K];
    auto *TD = Ctx.create<TypeDecl>(P.Loc, P, Types[K]);
    Used.insert(TD);
    Sc->insert(P.Name, TD);
  }
  Scope *InstScope = Sc.get();
  InstanceScopes.push_back(std::move(Sc));
  Scope *SavedScope = Cur;
  RoutineDecl *SavedRoutine = CurRoutine;
  llvm::StringRef SavedPrefix = NestPrefix;
  Cur = InstScope;
  CurRoutine = nullptr;
  NestPrefix = {};
  unsigned Errors = Diags.getNumErrors();
  I->setParent(nullptr);
  I->setClass(C);
  I->setMethodKind(MethodKind::Method);
  if (G->isClassMethod())
    I->setClassMethod();
  if (G->isStatic())
    I->setStatic(true);
  I->setVisibility(G->getVisibility());
  resolveSignature(I);
  if (!I->isStatic())
    makeSelf(I, C);
  if (Diags.getNumErrors() > Errors)
    Diags.note(Name.Loc, "in the specialization '" + qualified(C, I->getName()) +
                             "' requested here");
  Cur = SavedScope;
  CurRoutine = SavedRoutine;
  NestPrefix = SavedPrefix;
  Used.insert(I);
  PendingInstances.push_back({I, InstScope, Name.Loc});
  ModuleInstances.push_back(I);
  return I;
}


/// The generic methods named Name of class C or its ancestors (the
/// nearest of each number of type parameters).
llvm::SmallVector<RoutineDecl *, 2> Checker::genericMethodsNamed(const ClassType *C,
                                                        llvm::StringRef Name){
  llvm::SmallVector<RoutineDecl *, 2> Found;
  for (size_t N = 1; N <= 16; ++N)
    if (auto *G = llvm::dyn_cast_or_null<RoutineDecl>(
            C->findMember(genericKey(Name, N))))
      Found.push_back(G);
  return Found;
}


/// Is TS, the type generic helper TD extends, a pattern that gives all
/// its type parameters (and not a bare parameter: a helper for every
/// type)?
bool Checker::genericHelperPatternOK(TypeDecl *TD, TypeSpec *TS){
  auto Params = TD->getTypeParams();
  llvm::SmallVector<bool, 2> Seen(Params.size(), false);
  std::function<bool(TypeSpec *)> Walk = [&](TypeSpec *X) -> bool {
    if (auto *N = llvm::dyn_cast<NamedTypeSpec>(X)) {
      if (N->getQualifiers().empty())
        for (size_t K = 0; K < Params.size(); ++K)
          if (Params[K].Name == N->getName().Name)
            Seen[K] = true;
      return true;
    }
    if (auto *DS = llvm::dyn_cast<DynArrayTypeSpec>(X))
      return Walk(DS->getElement());
    if (auto *AS = llvm::dyn_cast<AsyncTypeSpec>(X); AS && AS->isIterator())
      return Walk(AS->getInner());
    if (auto *SS = llvm::dyn_cast<SpecializeTypeSpec>(X)) {
      for (TypeSpec *A : SS->getArgs())
        if (!Walk(A))
          return false;
      return true;
    }
    return false;
  };
  if (auto *N = llvm::dyn_cast<NamedTypeSpec>(TS); N && N->getQualifiers().empty())
    for (const Identifier &P : Params)
      if (P.Name == N->getName().Name)
        return false; // (a helper for every type)
  return Walk(TS) && !llvm::is_contained(Seen, false);
}


/// Is Name the name of a generic (type or routine), with any number of
/// type parameters?
bool Checker::isGenericName(llvm::StringRef Name){
  for (size_t N = 1; N <= 16; ++N)
    if (Cur->lookup(genericKey(Name, N)))
      return true;
  return false;
}


/// generic TList<T> = ...: declared (under its key), not checked.
void Checker::declareGenericType(TypeDecl *TD){
  if (auto *HS = llvm::dyn_cast<HelperTypeSpec>(TD->getTypeSpec());
      HS && !CurRoutine) { // a generic helper: also in the chain
    if (!genericHelperPatternOK(TD, HS->getExtended()))
      Diags.error(HS->getExtended()->getBeginLoc(),
                  "a generic helper must extend a type built from all its "
                  "type parameters, such as 'array of T' or 'specialize "
                  "TGen<T>'",
                  {HS->getExtended()->getRange()});
    else {
      TD->setPrevGenericHelper(
          llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(genericHelpersKey())));
      Cur->set(genericHelpersKey(), TD);
    }
  }
  const Identifier &Id = TD->getName();
  TD->setType(errorType());
  if (CurRoutine) {
    Diags.error(Id.Loc,
                "generic types must be declared at program or unit level",
                {Id.getRange()});
    return;
  }
  auto Params = TD->getTypeParams();
  for (size_t I = 0; I < Params.size(); ++I)
    for (size_t J = 0; J < I; ++J)
      if (Params[I].Name == Params[J].Name)
        Diags.error(Params[I].Loc,
                    "duplicate type parameter '" + Params[I].Spelling + "'",
                    {Params[I].getRange()});
  llvm::StringRef Key = genericKey(Id.Name, Params.size());
  if (Decl *Prev = Cur->lookupOwn(Key)) {
    Diags.error(Id.Loc, "redefinition of generic '" + Id.Spelling + "'",
                {Id.getRange()});
    Diags.note(Prev->getLocation(), "previous definition is here");
    return;
  }
  TD->setGenericScope(Cur);
  Used.insert(TD);
  Cur->insert(Key, TD);
}


/// specialize Name<Args>, a type: the declaration of the type made of
/// the generic type Name for those type arguments, parsed again from the
/// generic's tokens on first use and declared like any type (named
/// Name<integer>), in the generic's scope plus the type parameters as
/// type aliases; its methods' implementations are parsed again too, and
/// their bodies checked later. Null after a diagnostic.
TypeDecl *Checker::specializeType(const Identifier &Name,
                         llvm::ArrayRef<TypeSpec *> Args){
  llvm::SmallVector<Type *, 2> Types;
  for (TypeSpec *TS : Args) {
    Type *T = resolveType(TS);
    if (T->isError())
      return nullptr;
    Types.push_back(T);
  }
  auto *G = llvm::dyn_cast_or_null<TypeDecl>(
      Cur->lookup(genericKey(Name.Name, Args.size())));
  if (!G || !G->isGeneric()) {
    Diags.error(Name.Loc,
                "'" + Name.Spelling + "' is not a generic type with " +
                    llvm::Twine(Args.size()) + " type parameter" +
                    (Args.size() == 1 ? "" : "s"),
                {Name.getRange()});
    return nullptr;
  }
  return specializeTypeFor(Name, G, Types);
}


/// The specialization of generic type G for Types (asked for at Name),
/// made on first use (see specializeType).
TypeDecl *Checker::specializeTypeFor(const Identifier &Name, TypeDecl *G,
                            llvm::ArrayRef<Type *> Types){
  Used.insert(G);
  auto Key = std::make_pair(G, std::vector<Type *>(Types.begin(), Types.end()));
  if (auto It = TypeInstances.find(Key); It != TypeInstances.end())
    return It->second;
  llvm::SmallVector<Decl *, 8> Methods;
  TypeDecl *I = ReparseType ? ReparseType(G, Methods) : nullptr;
  if (!I) {
    if (!ReparseType)
      Diags.error(Name.Loc, "generic types cannot be specialized here",
                  {Name.getRange()});
    return nullptr;
  }
  // TList<integer, string>; it is still found as TList, in its scope.
  std::string S = (G->getName().Spelling + "<").str();
  for (size_t K = 0; K < Types.size(); ++K)
    S += (K ? ", " : "") + Types[K]->getName();
  S += ">";
  I->setName(Identifier{G->getName().Name, Ctx.getStrings().intern(S),
                        G->getName().Loc});
  TypeInstances[Key] = I;
  auto Sc = std::make_unique<Scope>(static_cast<Scope *>(G->getGenericScope()));
  for (size_t K = 0; K < Types.size(); ++K) {
    const Identifier &P = G->getTypeParams()[K];
    auto *TD = Ctx.create<TypeDecl>(P.Loc, P, Types[K]);
    Used.insert(TD);
    Sc->insert(P.Name, TD);
  }
  Scope *InstScope = Sc.get();
  InstanceScopes.push_back(std::move(Sc));
  InstanceScopesOf[G].push_back(InstScope);
  Scope *SavedScope = Cur;
  RoutineDecl *SavedRoutine = CurRoutine;
  llvm::StringRef SavedPrefix = NestPrefix;
  std::vector<RoutineDecl *> *SavedDeferred = DeferredBodies;
  std::vector<RoutineDecl *> Bodies;
  Cur = InstScope;
  CurRoutine = nullptr;
  NestPrefix = {};
  DeferredBodies = &Bodies;
  unsigned Errors = Diags.getNumErrors();
  size_t PointerMark = PendingPointers.size();
  declareType(I);
  resolvePendingPointersFrom(PointerMark);
  for (Decl *M : Methods)
    if (auto *R = llvm::dyn_cast<RoutineDecl>(M))
      declareRoutine(R);
  checkMethodsImplemented(I);
  if (Diags.getNumErrors() > Errors)
    Diags.note(Name.Loc, "in the specialization '" + S + "' requested here");
  Cur = SavedScope;
  CurRoutine = SavedRoutine;
  NestPrefix = SavedPrefix;
  DeferredBodies = SavedDeferred;
  Used.insert(I);
  for (RoutineDecl *R : Bodies)
    PendingInstances.push_back({R, InstScope, Name.Loc});
  // The type and its methods' implementations are the module's.
  ModuleInstances.push_back(I);
  for (Decl *M : Methods)
    ModuleInstances.push_back(M);
  return I;
}


/// A forward or interface heading without its definition (yet).
bool Checker::isUndefinedHeading(RoutineDecl *R){
  return !R->getBody() &&
         !(R->isGeneric() && R->getGenericDefinition()->getBody());
}


/// specialize Name<Args>: the routine for those type arguments, made
/// from the generic's declaration on first use (its heading resolved now,
/// its body checked later); null after a diagnostic.
/// (W: the with context the generic was found through, a generic
/// method's object: Self in a method.)
RoutineDecl *Checker::specialize(const Identifier &Name, llvm::ArrayRef<TypeSpec *> Args,
                        WithContext **W){
  llvm::SmallVector<Type *, 2> Types;
  for (TypeSpec *TS : Args) {
    Type *T = resolveType(TS);
    if (T->isError())
      return nullptr;
    Types.push_back(T);
  }
  auto *G = llvm::dyn_cast_or_null<RoutineDecl>(
      Cur->lookup(genericKey(Name.Name, Args.size()), W));
  bool OtherGeneric = false; // F with another number of type parameters
  for (size_t N = 1; N <= 16 && !G; ++N)
    OtherGeneric |= Cur->lookup(genericKey(Name.Name, N)) != nullptr;
  if (G && G->isGenericMethod()) // specialize M<T>(args) in a method
    return instantiateMethod(Name, G, Types);
  if (!G) {
    if (Cur->lookup(Name.Name) || OtherGeneric)
      Diags.error(Name.Loc,
                  "'" + Name.Spelling + "' is not a generic routine with " +
                      llvm::Twine(Args.size()) + " type parameter" +
                      (Args.size() == 1 ? "" : "s"),
                  {Name.getRange()});
    else
      diagUndeclared(Name);
    return nullptr;
  }
  return instantiate(Name, G, Types);
}


/// The specialization of the generic routine G for type arguments Types
/// (see specialize).
RoutineDecl *Checker::instantiate(const Identifier &Name, RoutineDecl *G,
                         llvm::ArrayRef<Type *> Types){
  Used.insert(G);
  RoutineDecl *Def = G->getGenericDefinition();
  if (!Def->getBody()) {
    Diags.error(Name.Loc,
                "generic " + llvm::Twine(G->getKindName()) + " '" +
                    Name.Spelling + "' is used before its definition",
                {Name.getRange()});
    Diags.note(G->getLocation(), "declared here");
    return nullptr;
  }
  auto Key = std::make_pair(G, std::vector<Type *>(Types.begin(), Types.end()));
  if (auto It = Instances.find(Key); It != Instances.end())
    return It->second;
  RoutineDecl *I = Reparse ? Reparse(Def) : nullptr;
  if (!I) {
    if (!Reparse)
      Diags.error(Name.Loc, "generic routines cannot be specialized here",
                  {Name.getRange()});
    return nullptr;
  }
  // F<integer, string>
  std::string N = (Def->getName().Name + "<").str();
  std::string S = (Def->getName().Spelling + "<").str();
  for (size_t K = 0; K < Types.size(); ++K) {
    if (K) {
      N += ",";
      S += ", ";
    }
    N += llvm::StringRef(Types[K]->getName()).lower();
    S += Types[K]->getName();
  }
  N += ">";
  S += ">";
  I->setName(Identifier{Ctx.getStrings().intern(N), Ctx.getStrings().intern(S),
                        Def->getName().Loc});
  Instances[Key] = I;
  // Its scope: the generic's, where the type parameters name the types.
  auto Sc = std::make_unique<Scope>(static_cast<Scope *>(Def->getGenericScope()));
  for (size_t K = 0; K < Types.size(); ++K) {
    const Identifier &P = Def->getTypeParams()[K];
    auto *TD = Ctx.create<TypeDecl>(P.Loc, P, Types[K]);
    Used.insert(TD);
    Sc->insert(P.Name, TD);
  }
  Scope *InstScope = Sc.get();
  InstanceScopes.push_back(std::move(Sc));
  Scope *SavedScope = Cur;
  RoutineDecl *SavedRoutine = CurRoutine;
  llvm::StringRef SavedPrefix = NestPrefix;
  Cur = InstScope;
  CurRoutine = nullptr;
  NestPrefix = {};
  unsigned Errors = Diags.getNumErrors();
  I->setParent(nullptr);
  resolveSignature(I);
  if (Diags.getNumErrors() > Errors)
    Diags.note(Name.Loc, "in the specialization '" + S + "' requested here");
  Cur = SavedScope;
  CurRoutine = SavedRoutine;
  NestPrefix = SavedPrefix;
  Used.insert(I);
  PendingInstances.push_back({I, InstScope, Name.Loc});
  ModuleInstances.push_back(I);
  return I;
}


/// The index of the type parameter of Def that TS names, or -1.
int Checker::typeParamNamed(RoutineDecl *Def, TypeSpec *TS){
  auto *N = llvm::dyn_cast_or_null<NamedTypeSpec>(TS);
  if (!N || !N->getQualifiers().empty())
    return -1;
  auto Params = Def->getTypeParams();
  for (size_t K = 0; K < Params.size(); ++K)
    if (Params[K].Name == N->getName().Name)
      return static_cast<int>(K);
  return -1;
}


/// Does TS (written in generic Def) name one of Def's type parameters,
/// itself or in its type arguments?
bool Checker::mentionsTypeParam(RoutineDecl *Def, TypeSpec *TS){
  if (typeParamNamed(Def, TS) >= 0)
    return true;
  if (auto *SS = llvm::dyn_cast_or_null<SpecializeTypeSpec>(TS))
    return llvm::any_of(SS->getArgs(), [&](TypeSpec *A) {
      return mentionsTypeParam(Def, A);
    });
  if (auto *DS = llvm::dyn_cast_or_null<DynArrayTypeSpec>(TS))
    return mentionsTypeParam(Def, DS->getElement());
  if (auto *AS = llvm::dyn_cast_or_null<AsyncTypeSpec>(TS); AS && AS->isIterator())
    return mentionsTypeParam(Def, AS->getInner());
  return false;
}


/// Name(args), where Name is not declared but generic routines Name<...>
/// are: the specialization for the type arguments that the arguments
/// give (a parameter of type T or array of T gives T the type of its
/// argument; several arguments for one T, their common type), and for
/// those still unknown the type Want wanted of the result, if the result
/// is a T. Found: there is such a generic. Null without one, and after a
/// diagnostic.
RoutineDecl *Checker::inferSpecialization(const Identifier &Name,
                                 llvm::ArrayRef<Expr *> Args, Type *Want,
                                 bool &Found, WithContext **W){
  Found = false;
  llvm::SmallVector<RoutineDecl *, 2> Generics;
  for (size_t N = 1; N <= 16; ++N) {
    WithContext *GW = nullptr;
    if (auto *G = llvm::dyn_cast_or_null<RoutineDecl>(
            Cur->lookup(genericKey(Name.Name, N), &GW))) {
      if (Generics.empty() && W) // (M(args) in a method: Self.M)
        *W = GW;
      Generics.push_back(G);
    }
  }
  if (Generics.empty())
    return nullptr;
  Found = true;
  return inferFor(Name, Generics, Args, Want);
}


/// Of Generics (routines or methods named Name), the one whose
/// parameters take Args, specialized for the type arguments they (and
/// Want, the type wanted of the result) give; null after a diagnostic.
RoutineDecl *Checker::inferFor(const Identifier &Name,
                      llvm::SmallVectorImpl<RoutineDecl *> &Generics,
                      llvm::ArrayRef<Expr *> Args, Type *Want){
  if (Generics.size() > 1) { // those that take these arguments
    llvm::SmallVector<RoutineDecl *, 2> Fit;
    for (RoutineDecl *G : Generics) {
      auto Params = G->getGenericDefinition()->getParams();
      llvm::SmallVector<unsigned, 8> Map;
      if (hasNamedArgs(Args) ? mapArgs(Params, Args, Map, nullptr)
                             : acceptsArgCount(Params, Args.size()))
        Fit.push_back(G);
    }
    if (Fit.size() != 1) {
      Diags.error(Name.Loc,
                  "cannot tell which generic '" + Name.Spelling +
                      "' is called; give the types: specialize " +
                      Name.Spelling + "<...>",
                  {Name.getRange()});
      return nullptr;
    }
    Generics = Fit;
  }
  RoutineDecl *G = Generics[0];
  RoutineDecl *Def = G->getGenericDefinition();
  auto TypeParams = Def->getTypeParams();
  llvm::SmallVector<Type *, 2> Bound(TypeParams.size(), nullptr);
  Type *Str = Ctx.getAnsiStringType();
  bool OK = true;
  // T (number K) is also the type Ty of the value A.
  auto Give = [&](int K, Type *Ty, Expr *A) {
    if (Ty->isError()) {
      OK = false;
      return;
    }
    if (Ty->isStringLiteral() || isCharLiteral(A))
      Ty = Str;
    if (!isInferable(Ty))
      return; // (nil, []: says nothing)
    Type *Joined = Bound[K] ? commonType(Bound[K], Ty, Str) : Ty;
    if (!Joined) {
      Diags.error(A->getBeginLoc(),
                  "cannot infer the type parameter '" +
                      TypeParams[K].Spelling + "' of '" + Name.Spelling +
                      "': the arguments give both " + quoted(Bound[K]) +
                      " and " + quoted(Ty) + "; give the type: specialize " +
                      Name.Spelling + "<...>",
                  {A->getRange()});
      OK = false;
      return;
    }
    Bound[K] = Joined;
  };
  for (size_t I = 0; I < Args.size() && OK; ++I) {
    VarDecl *P = paramOfArg(Def->getParams(), Args, I);
    Expr *A = argValue(Args[I]);
    if (!P || llvm::isa<FormattedExpr>(A))
      continue;
    TypeSpec *TS = P->getTypeSpec();
    if (int K = typeParamNamed(Def, TS); K >= 0) {
      if (!isRoutineName(A))
        Give(K, check(A), A);
      continue;
    }
    TypeSpec *Elem = nullptr;
    if (auto *OS = llvm::dyn_cast_or_null<OpenArrayTypeSpec>(TS))
      Elem = OS->getElement();
    else if (auto *DS = llvm::dyn_cast_or_null<DynArrayTypeSpec>(TS))
      Elem = DS->getElement();
    if (int K = typeParamNamed(Def, Elem); K >= 0) {
      if (auto *SC = llvm::dyn_cast<SetCtorExpr>(A)) { // [a, b, c]
        for (const SetElement &El : SC->getElements())
          if (!El.Hi && OK)
            Give(K, check(El.Lo), El.Lo);
        continue;
      }
      Type *AT = check(A);
      if (AT->isError())
        OK = false;
      else if (auto *AA = llvm::dyn_cast<ArrayType>(AT))
        Give(K, AA->getElementType(), A);
      else if (auto *DA = llvm::dyn_cast<DynArrayType>(AT))
        Give(K, DA->getElementType(), A);
      else if (auto *OA = llvm::dyn_cast<OpenArrayType>(AT))
        Give(K, OA->getElementType(), A);
      continue;
    }
    // iterator T: T from the iterator's values
    if (auto *AS = llvm::dyn_cast_or_null<AsyncTypeSpec>(TS);
        AS && AS->isIterator())
      if (int K = typeParamNamed(Def, AS->getInner()); K >= 0) {
        Type *AT = check(A);
        if (AT->isError())
          OK = false;
        else if (auto *IT = llvm::dyn_cast<IteratorType>(AT))
          Give(K, IT->getElementType(), A);
        continue;
      }
    // specialize TGen<..., T, ...>: T from the specialization the
    // argument is (a lambda's are the lambda pass's, below)
    if (auto *SS = llvm::dyn_cast_or_null<SpecializeTypeSpec>(TS);
        SS && mentionsTypeParam(Def, SS) && !pendingLambda(A) &&
        !isRoutineName(A)) {
      Type *AT = check(A);
      if (AT->isError())
        OK = false;
      else
        inferFromSpecialization(Def, SS, AT, A, Give);
    }
  }
  if (!OK)
    return nullptr;
  // Lambdas passed for a parameter of a generic procedural type
  // (specialize TFunc<T, R>): the types written in them give type
  // arguments; those of their parameters not written come from the type
  // arguments known (T in), and a result not written from the body,
  // checked with them (R out).
  for (size_t I = 0; I < Args.size() && OK; ++I) {
    VarDecl *P = paramOfArg(Def->getParams(), Args, I);
    auto *A = llvm::dyn_cast<AddrOfExpr>(argValue(Args[I]));
    RoutineDecl *L = A ? A->getLambda() : nullptr;
    if (P && L && !L->isLambdaDone())
      inferThroughLambda(Def, P, A, L, Bound, OK, Give);
  }
  if (!OK)
    return nullptr;
  // The type wanted of the result, for a result of type T.
  if (int K = typeParamNamed(Def, Def->getResultTypeSpec());
      K >= 0 && !Bound[K] && Want && !Want->isError() && isInferable(Want))
    Bound[K] = Want;
  for (size_t K = 0; K < Bound.size(); ++K)
    if (!Bound[K]) {
      Diags.error(Name.Loc,
                  "cannot infer the type parameter '" +
                      TypeParams[K].Spelling + "' of '" + Name.Spelling +
                      "' in this call: no argument gives it; give the "
                      "type: specialize " + Name.Spelling + "<...>",
                  {Name.getRange()});
      noteDeclaredHere(G);
      return nullptr;
    }
  if (G->isGenericMethod())
    return instantiateMethod(Name, G, Bound);
  return instantiate(Name, G, Bound);
}


/// Checks the bodies of the specializations made so far (and those they
/// make), then adds the module's to its block.
void Checker::checkInstances(Block &ModuleBlock){
  for (size_t K = 0; K < PendingInstances.size() && !Diags.shouldStop(); ++K) {
    PendingInstance P = PendingInstances[K];
    Scope *SavedScope = Cur;
    Cur = P.S;
    unsigned Errors = Diags.getNumErrors();
    int SavedChecking = CheckingInstance;
    CheckingInstance = static_cast<int>(K);
    checkRoutineBody(P.R);
    CheckingInstance = SavedChecking;
    // (the specializations that led here, innermost first)
    for (int Q = static_cast<int>(K); Q >= 0 && Diags.getNumErrors() > Errors;
         Q = PendingInstances[Q].Parent) {
      const PendingInstance &I = PendingInstances[Q];
      Diags.note(I.Use,
                 "in the specialization '" +
                     (I.R->getClass()
                          ? qualified(I.R->getClass(), I.R->getName())
                          : I.R->getName().Spelling.str()) +
                     "' requested here");
    }
    Cur = SavedScope;
  }
  PendingInstances.clear();
  if (ModuleInstances.empty())
    return;
  llvm::SmallVector<Decl *, 16> All(ModuleBlock.getDecls().begin(),
                                    ModuleBlock.getDecls().end());
  All.append(ModuleInstances.begin(), ModuleInstances.end());
  ModuleBlock.setDecls(Ctx.copyArray<Decl *>(All));
  ModuleInstances.clear();
}


void Checker::declareRoutine(RoutineDecl *R){
  if (R->isLambda()) // completed where it is used (completeLambda)
    return;
  if (R->isSpawnBlock()) // completed where it is spawned (visitSpawnExpr)
    return;
  if (R->isTemplateMember()) { // of a generic type: see specializeType
    // Its specializations see what is declared up to here (the unit's
    // implementation, for a generic type of its interface).
    auto *G = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(
        genericKey(R->getClassName().Name, R->getTypeParams().size())));
    if (G && G->isGeneric() && !CurRoutine && G->getGenericScope() != Cur) {
      G->setGenericScope(Cur);
      for (Scope *S : InstanceScopesOf[G])
        S->setParent(Cur);
    }
    return;
  }
  R->setParent(CurRoutine);
  if (R->isGeneric())
    return declareGeneric(R);
  if (R->getClassName().isValid())
    return declareMethodBody(R);
  const Identifier &Id = R->getName();
  if (R->isClassMethod())
    Diags.error(Id.Loc,
                "only a method can be a class " + std::string(R->getKindName()) +
                    "; implement it as '" + R->getKindName() + " TClass." +
                    Id.Spelling.str() + "'",
                {Id.getRange()});

  // The definition of an earlier `forward` declaration?
  auto *Fwd = llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookupOwn(Id.Name));
  bool InInterface = false;
  if (!Fwd && CurUnitInterface && !CurRoutine) { // an interface heading
    Fwd = llvm::dyn_cast_or_null<RoutineDecl>(
        CurUnitInterface->lookupOwn(Id.Name));
    InInterface = Fwd != nullptr;
  }
  // objfpc: of overloaded routines, the one with this heading.
  if (Fwd && Fwd->isOverloaded() && R->getBody()) {
    for (VarDecl *P : R->getParams())
      P->setType(resolveType(P->getTypeSpec()));
    RoutineDecl *Match = nullptr;
    for (RoutineDecl *X = Fwd; X; X = X->getNextOverload())
      if (X->isForward() && !X->getBody() && sameSignature(X, R))
        Match = X;
    Fwd = Match;
    if (!Match && InInterface) {
      Diags.error(Id.Loc,
                  "overloads of '" + Id.Spelling +
                      "' must all be declared in the interface",
                  {Id.getRange()});
      return;
    }
  }
  if (Fwd && Fwd->isForward() && !Fwd->getBody() && R->getBody()) {
    if ((Fwd->isFunction() || Fwd->isAsync() || Fwd->isIterator()) !=
        R->isFunction()) {
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
      Type *Declared = Fwd->isAsync()      ? Fwd->getAsyncType()
                       : Fwd->isIterator() ? Fwd->getIteratorType()
                                           : Fwd->getResultType();
      if (!sameSignature(Fwd, R) || (RT && RT != Declared) ||
          !sameDefaults(Fwd, R)) {
        Diags.error(Id.Loc,
                    "heading of '" + Id.Spelling +
                        "' does not match its forward declaration",
                    {Id.getRange()});
        Diags.note(Fwd->getLocation(), "forward declaration is here");
      }
    }
    if (R->isCDecl() != Fwd->isCDecl()) {
      Diags.error(Id.Loc,
                  "calling convention of '" + Id.Spelling + "' is " +
                      callConvDesc(R->isCDecl()) +
                      " but its forward declaration's is " +
                      callConvDesc(Fwd->isCDecl()),
                  {Id.getRange()});
      Diags.note(Fwd->getLocation(), "forward declaration is here");
    }
    Fwd->setBody(R->getBody());
    Fwd->setResultName(R->hasResultName()); // (as where its body is)
    R->setCanonical(Fwd);
    if (R->isInline())
      Fwd->setInline();
    if (R->isCompiletime())
      Fwd->setCompiletime();
    checkRoutineBody(Fwd);
    return;
  }

  resolveSignature(R);
  if (InInterface && LangOpts.ObjKeywords) { // a different heading
    Diags.error(Id.Loc,
                "overloads of '" + Id.Spelling +
                    "' must all be declared in the interface",
                {Id.getRange()});
    return;
  }
  if (!declareRoutineName(R))
    return;
  if (R->isExternal())
    checkExternal(R);
  if (R->getBody())
    checkRoutineBody(R);
}


/// Declares R; in objfpc, another routine of its name in the same scope
/// makes it an overload (with other parameters). False if R was rejected
/// as such (its body is not checked then).
bool Checker::declareRoutineName(RoutineDecl *R){
  if (LangOpts.ObjKeywords)
    if (auto *First = llvm::dyn_cast_or_null<RoutineDecl>(
            Cur->lookupOwn(R->getName().Name)))
      return addOverload(First, R);
  declare(R);
  return true;
}


/// R (resolved) joins the overloads First...: its parameters must differ.
bool Checker::addOverload(RoutineDecl *First, RoutineDecl *R){
  for (RoutineDecl *X = First; X; X = X->getNextOverload())
    if (sameSignature(X, R, /*CompareOut=*/false) &&
        !(isConversionOperator(R) && X->getResultType() != R->getResultType())) {
      const Identifier &Id = R->getName();
      Diags.error(Id.Loc,
                  "redefinition of '" + Id.Spelling +
                      "' (overloads must differ in their parameters)",
                  {Id.getRange()});
      Diags.note(X->getLocation(), "previous definition is here");
      return false;
    }
  First->addOverload(R);
  return true;
}


/// A record's `:=` or `Explicit` operator: its overloads may differ in the
/// result type alone.
bool Checker::isConversionOperator(RoutineDecl *R){
  return R->isOperator() && (R->getName().Name == "operator:=" ||
                             R->getName().Name == "operatorexplicit");
}


//===--------------------------------------------------------------------===//
// Overload resolution (objfpc)
//===--------------------------------------------------------------------===//

/// The routines a call of First's name may mean: First's overloads; for a
/// method with the `overload` directive also those of its ancestors that
/// it does not override.
void Checker::collectOverloads(RoutineDecl *First,
                      llvm::SmallVectorImpl<RoutineDecl *> &Out){
  bool Directive = false;
  size_t Start = Out.size();
  for (RoutineDecl *X = First; X; X = X->getNextOverload()) {
    Out.push_back(X);
    Directive |= X->hasOverloadDirective();
  }
  if (!Directive || !First->isMethod() || !First->getClass())
    return;
  ClassType *Parent = First->getClass()->getParent();
  auto *Up = Parent ? llvm::dyn_cast_or_null<RoutineDecl>(
                          Parent->findMember(First->getName().Name))
                    : nullptr;
  if (!Up)
    return;
  llvm::SmallVector<RoutineDecl *, 4> Inherited;
  collectOverloads(Up, Inherited);
  for (RoutineDecl *Y : Inherited)
    if (llvm::none_of(llvm::ArrayRef<RoutineDecl *>(Out).drop_front(Start),
                      [&](RoutineDecl *X) { return sameSignature(X, Y); }))
      Out.push_back(Y);
}


/// How far an argument of type A is from a parameter of type P (lower
/// is better): an exact type, a subrange of it, a wider integer type
/// (integer before int64), a same-size or narrower one, real, an
/// ancestor class (the nearest first), anything else assignable.
unsigned Checker::conversionCost(Type *P, Type *A){
  if (P == A)
    return 0;
  Type *PB = P->getBase(), *AB = A->getBase();
  if (PB == AB)
    return 1;
  if (PB->isInteger() && AB->isInteger()) {
    unsigned PBits = llvm::cast<IntegerType>(PB)->getBits();
    unsigned ABits = llvm::cast<IntegerType>(AB)->getBits();
    if (PBits > ABits)
      return PBits == 64 ? 3 : 2;
    return PBits == ABits ? 4 : 5;
  }
  if (P->isReal() && AB->isInteger())
    return 6;
  if (P->isShortString() &&
      (A->isShortString() || A->isStringLiteral() || A->isAnsiString()))
    return 1;
  if (P->isShortString() && AB->isChar())
    return 2;
  if (P->isAnsiString() && (A->isShortString() || A->isStringLiteral()))
    return 2;
  if (P->isAnsiString() && AB->isChar())
    return 3;
  auto Distance = [](const ClassType *From, const ClassType *To) {
    unsigned D = 0;
    for (; From && From != To; From = From->getParent())
      ++D;
    return D;
  };
  if (auto *PC = llvm::dyn_cast<ClassType>(P))
    if (auto *AC = llvm::dyn_cast<ClassType>(A))
      return 1 + Distance(AC, PC);
  if (auto *PR = llvm::dyn_cast<ClassRefType>(P))
    if (auto *AR = llvm::dyn_cast<ClassRefType>(A))
      return 1 + Distance(AR->getClass(), PR->getClass());
  if (A->isNil())
    return 1;
  return 7;
}


/// Breaks ties between costs: widening an integer to 64 bits of the
/// other signedness (byte to int64 rather than qword, longint to qword
/// rather than int64), as Free Pascal prefers the same signedness.
unsigned Checker::signPenalty(Type *P, Type *A){
  auto *PI = llvm::dyn_cast<IntegerType>(P->getBase());
  auto *AI = llvm::dyn_cast<IntegerType>(A->getBase());
  return PI && AI && PI->getBits() == 64 && AI->getBits() < 64 &&
                 PI->isSigned() != AI->isSigned()
             ? 1
             : 0;
}


/// Is Arg the name of a routine (passed for a procedural parameter)?
bool Checker::isRoutineName(Expr *Arg){
  auto *N = llvm::dyn_cast<NameRefExpr>(Arg);
  return N && llvm::isa_and_nonnull<RoutineDecl>(Cur->lookup(N->getName().Name));
}


/// Can parameters Params take N arguments (the rest from default values)?
bool Checker::acceptsArgCount(llvm::ArrayRef<VarDecl *> Params, size_t N){
  if (N > Params.size())
    return false;
  for (size_t I = N; I < Params.size(); ++I)
    if (!Params[I]->getDefault())
      return false;
  return true;
}


/// The value of an argument: of Name := value, the value.
Expr *Checker::argValue(Expr *Arg){
  if (auto *NA = llvm::dyn_cast<NamedArgExpr>(Arg))
    return NA->getValue();
  return Arg;
}


bool Checker::hasNamedArgs(llvm::ArrayRef<Expr *> Args){
  for (Expr *A : Args)
    if (llvm::isa<NamedArgExpr>(A))
      return true;
  return false;
}


/// The parameter of Params that argument I of Args is for (by its
/// position, or by its name), or null.
VarDecl *Checker::paramOfArg(llvm::ArrayRef<VarDecl *> Params,
                           llvm::ArrayRef<Expr *> Args, size_t I){
  if (auto *NA = llvm::dyn_cast<NamedArgExpr>(Args[I])) {
    for (VarDecl *P : Params)
      if (P->getName().Name == NA->getName().Name)
        return P;
    return nullptr;
  }
  return I < Params.size() ? Params[I] : nullptr;
}


/// Where the arguments of a call with named ones go: Map[I] is the index
/// in Params of the parameter of argument I. Arguments by position come
/// first and are for the first parameters; each parameter is given once;
/// one that is not given has a default value. False if Args do not fit,
/// reported if Callee is given.
bool Checker::mapArgs(llvm::ArrayRef<VarDecl *> Params, llvm::ArrayRef<Expr *> Args,
             llvm::SmallVectorImpl<unsigned> &Map,
             const Identifier *Callee){
  llvm::SmallVector<Expr *, 8> Given(Params.size(), nullptr);
  bool Named = false;
  for (size_t I = 0; I < Args.size(); ++I) {
    Expr *A = Args[I];
    auto *NA = llvm::dyn_cast<NamedArgExpr>(A);
    size_t At = I;
    if (NA) {
      Named = true;
      for (At = 0; At < Params.size(); ++At)
        if (Params[At]->getName().Name == NA->getName().Name)
          break;
      if (At == Params.size()) {
        if (Callee)
          Diags.error(NA->getName().Loc,
                      "'" + Callee->Spelling + "' has no parameter named '" +
                          NA->getName().Spelling + "'",
                      {NA->getName().getRange()});
        return false;
      }
    } else if (Named) {
      if (Callee)
        Diags.error(A->getBeginLoc(),
                    "an argument without a name cannot follow a named one",
                    {A->getRange()});
      return false;
    } else if (At >= Params.size()) {
      if (Callee)
        Diags.error(A->getBeginLoc(),
                    "too many arguments in call to '" + Callee->Spelling +
                        "': it has " + llvm::Twine(Params.size()) +
                        (Params.size() == 1 ? " parameter" : " parameters"),
                    {A->getRange()});
      return false;
    }
    if (Given[At]) {
      if (Callee) {
        Diags.error(A->getBeginLoc(),
                    "parameter '" + Params[At]->getName().Spelling +
                        "' is given twice",
                    {A->getRange()});
        Diags.note(Given[At]->getBeginLoc(), "its other argument is here");
      }
      return false;
    }
    Given[At] = A;
    Map.push_back(static_cast<unsigned>(At));
  }
  for (size_t I = 0; I < Params.size(); ++I)
    if (!Given[I] && !Params[I]->getDefault()) {
      if (Callee)
        Diags.error(Callee->Loc,
                    "no argument for parameter '" +
                        Params[I]->getName().Spelling + "' in call to '" +
                        Callee->Spelling + "'",
                    {Callee->getRange()});
      return false;
    }
  return true;
}


/// The cost of calling R with Args (types ArgTys; null: a routine's
/// name), or -1 if they do not fit. Penalty receives the sign penalties
/// (which break ties).
int Checker::callCost(RoutineDecl *R, llvm::ArrayRef<Expr *> Args,
             llvm::ArrayRef<Type *> ArgTys, unsigned &Penalty){
  Penalty = 0;
  auto Params = R->getParams();
  llvm::SmallVector<unsigned, 8> Map;
  if (hasNamedArgs(Args)) {
    if (!mapArgs(Params, Args, Map, nullptr))
      return -1;
  } else if (!acceptsArgCount(Params, Args.size())) {
    return -1;
  }
  int Cost = 0;
  for (size_t I = 0; I < Args.size(); ++I) {
    VarDecl *P = Params[Map.empty() ? I : Map[I]];
    Expr *Arg = argValue(Args[I]);
    Type *PT = P->getType();
    Type *AT = ArgTys[I];
    if (!PT || PT->isError())
      return -1;
    if (auto *SC = llvm::dyn_cast<SetCtorExpr>(Arg);
        SC && AT == ArrayOfConstTy) {
      int C = setCtorCost(SC, P);
      if (C < 0)
        return -1;
      Cost += C;
      continue;
    }
    if (auto *OA = llvm::dyn_cast<OpenArrayType>(PT)) {
      if (!AT || !fitsOpenArray(Arg, AT, OA))
        return -1;
      Cost += AT == OA ? 0 : 1;
      continue;
    }
    if (RoutineDecl *L = pendingLambda(Arg)) { // its shape
      int C = lambdaCost(L, PT);
      if (C < 0)
        return -1;
      Cost += C;
      continue;
    }
    if (!AT) { // a routine's name: for a procedural parameter
      if (!llvm::isa<RoutineType>(PT))
        return -1;
      continue;
    }
    if (PT->isUntyped()) { // any variable, after the typed fits
      if (!isVariableReference(Arg))
        return -1;
      Cost += 100;
      continue;
    }
    if (AT->isUntyped())
      return -1;
    if (P->isVarParam()) {
      if (AT != PT || !isVariableReference(Arg))
        return -1;
      continue;
    }
    if (!isAssignable(PT, AT) && !isPCharLiteral(PT, Arg)) {
      if (!findConversion(AT, PT, false))
        return -1;
      Cost += 1000; // a record's := operator
      continue;
    }
    Cost += static_cast<int>(conversionCost(PT, AT));
    Penalty += signPenalty(PT, AT);
  }
  return Cost;
}


/// The routine of First's name that Args fit best: First itself unless
/// it is overloaded. Null after a diagnostic (no fit, or a tie).
bool Checker::hasConstArrayParam(llvm::ArrayRef<RoutineDecl *> Cands,
                               llvm::ArrayRef<Expr *> Args, size_t I){
  for (RoutineDecl *R : Cands)
    if (VarDecl *P = paramOfArg(R->getParams(), Args, I);
        P && P->getType() &&
        (P->getType()->isArrayOfConst() || P->getType()->isOpenArray()))
      return true;
  return false;
}


bool Checker::hasProceduralParam(llvm::ArrayRef<RoutineDecl *> Cands,
                               llvm::ArrayRef<Expr *> Args, size_t I){
  for (RoutineDecl *R : Cands)
    if (VarDecl *P = paramOfArg(R->getParams(), Args, I);
        P && P->getType() && llvm::isa<RoutineType>(P->getType()))
      return true;
  return false;
}


/// The parameters of R that a call with N arguments leaves to their
/// default values.
unsigned Checker::defaultsUsed(RoutineDecl *R, size_t N){
  size_t Count = R->getParams().size();
  return Count > N ? static_cast<unsigned>(Count - N) : 0;
}


/// Does an expression of type A fit an open array of Elem without being
/// an array constructor: the same open array, an array of exactly Elem,
/// or a string constant for chars?
bool Checker::fitsOpenArray(Expr *Arg, Type *A, OpenArrayType *OA){
  Type *Elem = OA->getElementType();
  if (A == OA)
    return true;
  if (auto *AT = llvm::dyn_cast<ArrayType>(A)) // (not a bitpacked layout)
    return AT->getElementType() == Elem && !AT->hasBitElements();
  if (auto *DA = llvm::dyn_cast<DynArrayType>(A))
    return DA->getElementType() == Elem;
  return Elem->isChar() &&
         (A->isStringLiteral() || (A->getBase()->isChar() && fold(Arg)));
}


/// The cost of passing [...] for a parameter of type PT (-1: it cannot
/// be): as an array of const first, then a set, then an open array, as in
/// Free Pascal.
int Checker::setCtorCost(SetCtorExpr *E, VarDecl *P){
  Type *PT = P->getType();
  if (PT->isArrayOfConst())
    return 0;
  int Cost = 0;
  if (auto *OA = llvm::dyn_cast<OpenArrayType>(PT)) {
    if (P->isVarParam())
      return -1;
    Cost = 2;
    for (const SetElement &El : E->getElements()) {
      if (El.Hi)
        return -1;
      Type *T = check(El.Lo);
      if (T->isError() ||
          (!isAssignable(OA->getElementType(), T) &&
           !isPCharLiteral(OA->getElementType(), El.Lo)))
        return -1;
      Cost += conversionCost(OA->getElementType(), T);
    }
    return Cost;
  }
  auto *ST = llvm::dyn_cast<SetType>(PT);
  if (!ST || P->isVarParam())
    return -1;
  for (const SetElement &El : E->getElements())
    for (Expr *X : {El.Lo, El.Hi}) {
      if (!X)
        continue;
      Type *T = check(X);
      if (T->isError() || !T->isOrdinal() ||
          !haveSameOrdinalHost(ST->getElementType(), T))
        return -1;
    }
  return 1;
}


void Checker::checkOpenArrayArg(Expr *Arg, VarDecl *P, OpenArrayType *OA){
  const Identifier &PName = P->getName();
  Type *Elem = OA->getElementType();
  if (auto *SC = llvm::dyn_cast<SetCtorExpr>(Arg)) {
    if (P->isVarParam()) {
      Diags.error(Arg->getBeginLoc(),
                  "argument for '" + llvm::Twine(paramKeyword(P)) + "' parameter '" + PName.Spelling +
                      "' must be a variable",
                  {Arg->getRange()});
      return;
    }
    for (const SetElement &El : SC->getElements()) {
      if (El.Hi) {
        Diags.error(El.Hi->getBeginLoc(),
                    "a range cannot be an element of an open array",
                    {El.Hi->getRange()});
        continue;
      }
      Type *T = check(El.Lo);
      if (T->isError())
        continue;
      if (!isAssignable(Elem, T) && !isPCharLiteral(Elem, El.Lo) &&
          !convertImplicitly(El.Lo, Elem))
        Diags.error(El.Lo->getBeginLoc(),
                    "a value of type " + quoted(T) +
                        " cannot be an element of an open array of " +
                        quoted(Elem),
                    {El.Lo->getRange()});
    }
    SC->setType(OA);
    return;
  }
  bool Slice = isSliceCall(Arg);
  AllowSlice = Slice;
  Type *AT = check(Arg);
  AllowSlice = false;
  if (AT->isError())
    return;
  if (!fitsOpenArray(Arg, AT, OA)) {
    Diags.error(Arg->getBeginLoc(),
                "cannot pass a value of type " + quoted(AT) + " to parameter '" +
                    PName.Spelling + "' of type " + quoted(OA),
                {Arg->getRange()});
    return;
  }
  if (P->isVarParam()) {
    Expr *Target = Slice ? llvm::cast<CallExpr>(Arg)->getArgs()[0] : Arg;
    if (isBitField(Target))
      diagBitFieldAddress(Target);
    else if (!isVariableReference(Target))
      Diags.error(Arg->getBeginLoc(),
                  "argument for '" + llvm::Twine(paramKeyword(P)) + "' parameter '" + PName.Spelling +
                      "' must be a variable",
                  {Arg->getRange()});
    else
      checkModifiable(Target);
  }
}


/// [a, b, ...] passed for an array of const: any values it can hold.
void Checker::checkConstArray(SetCtorExpr *E, Type *PT){
  for (const SetElement &El : E->getElements()) {
    if (El.Hi) {
      Diags.error(El.Hi->getBeginLoc(),
                  "a range cannot be an element of an array of const",
                  {El.Hi->getRange()});
      continue;
    }
    Type *T = check(El.Lo);
    if (T->isError())
      continue;
    if (T->getBase()->isWideChar() ||
        !(T->isIntegerLike() || T->isReal() || T->getBase()->isChar() ||
          T->getBase()->isBoolean() || T->isStringish() || T->isPointer() ||
          T->isNil() || T->isClass() || T->isClassRef()))
      Diags.error(El.Lo->getBeginLoc(),
                  "a value of type " + quoted(T) +
                      " cannot be an element of an array of const",
                  {El.Lo->getRange()});
  }
  E->setType(PT);
}


/// E is a lambda not completed yet: its routine.
RoutineDecl *Checker::pendingLambda(Expr *E){
  auto *A = llvm::dyn_cast<AddrOfExpr>(E);
  RoutineDecl *L = A ? A->getLambda() : nullptr;
  if (auto *C = llvm::dyn_cast<CallExpr>(E)) // one that copies
    L = C->getCopyLambda();
  return L && !L->isLambdaDone() ? L : nullptr;
}


/// The types written in lambda L (of its parameters and result), resolved
/// once while overloads are chosen (kept as their types until
/// completeLambda sets them). False after a diagnostic.
bool Checker::resolveLambdaSpecs(RoutineDecl *L){
  for (VarDecl *P : L->getParams())
    if (P->getTypeSpec() && !P->getType()) {
      Type *T = resolveType(P->getTypeSpec());
      if (T->isError())
        return false;
      P->setType(T);
    }
  if (L->isFunction() && L->getResultTypeSpec() && !L->getResultType()) {
    Type *T = resolveType(L->getResultTypeSpec());
    if (T->isError())
      return false;
    L->setResultType(T);
  }
  return true;
}


/// The cost of passing lambda L for a parameter of type PT, by its
/// shape (-1: it does not fit): a procedural type of the same kind
/// (function or procedure), with as many parameters, of the types and
/// kinds L writes, and the result type it writes. A parameter written
/// with a type but without a kind takes the kind wanted, but fits a value
/// parameter best (cost 1 for each other); one without either fits any
/// parameter alike (so overloads that differ there are ambiguous).
int Checker::lambdaCost(RoutineDecl *L, Type *PT){
  auto *RT = llvm::dyn_cast<RoutineType>(PT);
  if (!RT || RT->isFunction() != L->isFunction() ||
      RT->getParams().size() != L->getParams().size() ||
      (L->isCopied() && !RT->isReference())) // (copies: a closure)
    return -1;
  int Cost = 0;
  for (size_t I = 0; I < RT->getParams().size(); ++I) {
    VarDecl *P = L->getParams()[I], *WP = RT->getParams()[I];
    if (P->getTypeSpec() && P->getType() != WP->getType())
      return -1;
    bool WantsValue = WP->getVarKind() == VarDecl::ValueParam &&
                      !WP->isOutParam() && !WP->isConstRefParam();
    if (P->getVarKind() == VarDecl::ValueParam)
      Cost += WantsValue || !P->getTypeSpec() ? 0 : 1;
    else if (P->getVarKind() != WP->getVarKind() ||
             P->isOutParam() != WP->isOutParam() ||
             P->isConstRefParam() != WP->isConstRefParam())
      return -1;
  }
  if (L->isFunction() && L->getResultTypeSpec() &&
      L->getResultType() != RT->getResult())
    return -1;
  return Cost;
}


RoutineDecl *Checker::resolveOverload(RoutineDecl *First, llvm::ArrayRef<Expr *> Args,
                             const Identifier &Callee, SourceRange Range){
  llvm::SmallVector<RoutineDecl *, 4> Cands;
  collectOverloads(First, Cands);
  if (Cands.size() < 2)
    return First;
  llvm::SmallVector<Type *, 4> ArgTys;
  for (Expr *Written : Args) {
    Expr *A = argValue(Written);
    if (llvm::isa<FormattedExpr>(A))
      return First; // reported with the call's arguments
    // A lambda: chosen by its shape (lambdaFits), completed afterwards
    // with the parameter's type.
    if (RoutineDecl *L = pendingLambda(A)) {
      if (!resolveLambdaSpecs(L))
        return nullptr;
      ArgTys.push_back(nullptr);
      continue;
    }
    // [...] for an open array need not be a valid set.
    if (llvm::isa<SetCtorExpr>(A) &&
        hasConstArrayParam(Cands, Args, ArgTys.size())) {
      ArgTys.push_back(ArrayOfConstTy);
      continue;
    }
    AllowSlice = isSliceCall(A);
    // A routine's name: for a procedural parameter if a candidate has
    // one here, else a call (a function without parameters).
    Type *T = isRoutineName(A) &&
                      hasProceduralParam(Cands, Args, ArgTys.size())
                  ? nullptr
                  : checkAllowingUntyped(A);
    AllowSlice = false;
    if (T && T->isError())
      return nullptr;
    ArgTys.push_back(T);
  }
  // The best conversions of the arguments written decide; then the
  // fewest parameters left to their default values (a routine that takes
  // just these arguments is preferred); a tie is an error.
  RoutineDecl *Best = nullptr;
  int BestCost = -1;
  unsigned BestPenalty = 0, BestDefaults = 0;
  bool Tie = false;
  for (RoutineDecl *R : Cands) {
    unsigned Penalty;
    int Cost = callCost(R, Args, ArgTys, Penalty);
    if (Cost < 0)
      continue;
    unsigned Defaults = defaultsUsed(R, Args.size());
    if (!Best || Cost < BestCost ||
        (Cost == BestCost && Penalty < BestPenalty) ||
        (Cost == BestCost && Penalty == BestPenalty &&
         Defaults < BestDefaults)) {
      Best = R;
      BestCost = Cost;
      BestPenalty = Penalty;
      BestDefaults = Defaults;
      Tie = false;
    } else if (Cost == BestCost && Penalty == BestPenalty &&
               Defaults == BestDefaults) {
      Tie = true;
    }
  }
  if (Best && !Tie)
    return Best;
  std::string Types;
  for (size_t I = 0; I < ArgTys.size(); ++I) {
    Types += I ? ", " : "";
    if (auto *NA = llvm::dyn_cast<NamedArgExpr>(Args[I]))
      Types += NA->getName().Spelling.str() + " := ";
    Types += ArgTys[I]                        ? ArgTys[I]->getName()
             : pendingLambda(argValue(Args[I])) ? lambdaShape(
                                                      pendingLambda(argValue(Args[I])))
                                                : std::string("routine");
  }
  Diags.error(Callee.Loc,
              (Best ? "ambiguous call to overloaded '"
                    : "no overload of '") +
                  Callee.Spelling +
                  (Best ? "' with arguments (" : "' takes arguments (") +
                  Types + ")",
              {Range});
  for (RoutineDecl *R : Cands) {
    unsigned Penalty;
    if (!Best || (callCost(R, Args, ArgTys, Penalty) == BestCost &&
                  Penalty == BestPenalty &&
                  defaultsUsed(R, Args.size()) == BestDefaults))
      Diags.note(R->getLocation(), "candidate: " + routineHeading(R));
  }
  return nullptr;
}


/// A lambda's shape for diagnostics: "function(integer; ?): ?" (? for
/// types left out).
std::string Checker::lambdaShape(RoutineDecl *L){
  std::string S = L->isFunction() ? "lambda function(" : "lambda procedure(";
  auto Params = L->getParams();
  for (size_t I = 0; I < Params.size(); ++I) {
    Type *T = Params[I]->getTypeSpec() ? Params[I]->getType() : nullptr;
    S += std::string(I ? "; " : "") +
         (Params[I]->isOutParam()        ? "out "
          : Params[I]->isVarParam()      ? "var "
          : Params[I]->isConstRefParam() ? "constref "
          : Params[I]->isConstParam()    ? "const "
                                         : "") +
         (T ? T->getName() : std::string("?"));
  }
  S += ")";
  if (L->isFunction()) {
    Type *T = L->getResultTypeSpec() ? L->getResultType() : nullptr;
    S += ": " + (T ? T->getName() : std::string("?"));
  }
  return S;
}


/// "f(integer; var string): real" for diagnostics.
std::string Checker::routineHeading(RoutineDecl *R){
  std::string S = R->getName().Spelling.str() + "(";
  auto Params = R->getParams();
  for (size_t I = 0; I < Params.size(); ++I)
    S += std::string(I ? "; " : "") +
         (Params[I]->isOutParam()     ? "out "
          : Params[I]->isVarParam()   ? "var "
          : Params[I]->isConstRefParam() ? "constref "
          : Params[I]->isConstParam() ? "const "
                                      : "") +
         (Params[I]->getType() ? Params[I]->getType()->getName()
                               : std::string("?"));
  S += ")";
  if (R->isFunction() && R->getResultType())
    S += ": " + R->getResultType()->getName();
  return S;
}


/// The string constant E, the library or symbol name (What) of an
/// external routine; nullopt after a diagnostic.
std::optional<llvm::StringRef> Checker::externalString(Expr *E, const char *What,
                                              const char *Of){
  Type *T = check(E);
  if (T->isError())
    return std::nullopt;
  std::optional<Constant> C = fold(E);
  if (C && C->Value.K == ConstValue::String)
    return C->Value.Str;
  if (C && C->Value.K == ConstValue::Ordinal && T->getBase()->isChar()) {
    char Ch = static_cast<char>(C->Value.Int);
    return llvm::StringRef(Ctx.copyArray<char>(llvm::ArrayRef<char>(&Ch, 1)).data(), 1);
  }
  Diags.error(E->getBeginLoc(),
              llvm::Twine("the ") + What + " of an external " + Of +
                  " must be a string constant",
              {E->getRange()});
  return std::nullopt;
}


/// A type with a C equivalent, for external routines.
bool Checker::isCType(Type *T){
  return T->isError() || T->isReal() || T->isPointer() ||
         T->isToken() || // (the runtime's token, not counted by the call)
         (T->isOrdinal() && !T->getBase()->isBoolean());
}


/// A procedural type C code can call: cdecl, with parameters and result
/// of C types.
bool Checker::isCCallback(RoutineType *RT){
  if (!RT->isCDecl() || (RT->getResult() && !isCType(RT->getResult())))
    return false;
  for (VarDecl *P : RT->getParams())
    if (P->getType() && !P->isVarParam() && !isCType(P->getType()))
      return false;
  return true;
}


/// External (C) routines: declared at the outermost level, with
/// parameters and result of types that have a C equivalent; the library
/// and symbol names: string constants.
void Checker::checkExternal(RoutineDecl *R){
  warnUnsafe(R->getName().Loc,
             "external routine '" + R->getName().Spelling + "'",
             {R->getName().getRange()});
  if (CurRoutine)
    Diags.error(R->getName().Loc,
                "external routine '" + R->getName().Spelling +
                    "' must be declared at program or unit level",
                {R->getName().getRange()});
  if (Expr *L = R->getLibraryExpr())
    if (auto S = externalString(L, "library name"))
      R->setLibraryName(*S);
  if (Expr *N = R->getNameExpr())
    if (auto S = externalString(N, "symbol name"))
      R->setExternalName(*S);
  if (R->isVarArgs() && !R->isCDecl())
    Diags.error(R->getName().Loc,
                "'varargs' routine '" + R->getName().Spelling +
                    "' must be 'cdecl'",
                {R->getName().getRange()});
  for (VarDecl *P : R->getParams()) {
    // (an untyped parameter: a pointer, `void *` in C)
    if (!P->getType() || P->isVarParam() || P->getType()->isUntyped())
      continue;
    if (auto *RT = llvm::dyn_cast<RoutineType>(P->getType())) {
      if (!isCCallback(RT))
        Diags.error(P->getLocation(),
                    "parameter '" + P->getName().Spelling +
                        "' of external routine '" + R->getName().Spelling +
                        "' has type " + quoted(P->getType()) +
                        "; a callback needs a 'cdecl' procedural type with "
                        "integer, real, char, pointer or var parameters",
                    {P->getName().getRange()});
      continue;
    }
    if (!isCType(P->getType()))
      Diags.error(P->getLocation(),
                  "parameter '" + P->getName().Spelling + "' of external routine '" +
                      R->getName().Spelling + "' has type " +
                      quoted(P->getType()) +
                      "; use integer, real, char, pointer, procedural or "
                      "var parameters",
                  {P->getName().getRange()});
  }
  if (R->isFunction() && !isCType(R->getResultType()))
    Diags.error(R->getName().Loc,
                "external function '" + R->getName().Spelling +
                    "' cannot return " + quoted(R->getResultType()),
                {R->getName().getRange()});
}


/// A string constant passed where a pchar is expected (Turbo/FPC): the
/// compiler supplies a NUL-terminated copy.
bool Checker::isPCharLiteral(Type *Target, Expr *Value){
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


void Checker::checkRoutineBody(RoutineDecl *R){
  if (DeferredBodies) { // of a specialization being declared: later
    DeferredBodies->push_back(R);
    return;
  }
  if (R->isAsync() || R->isIterator())
    checkAsyncRoutine(R);
  // The method of a nested class or record also sees the constants and
  // types of the classes around it.
  llvm::SmallVector<std::unique_ptr<Scope>, 2> OuterScopes;
  Scope *Base = Cur;
  if (ClassType *RC = R->getClass()) {
    llvm::SmallVector<ClassType *, 2> Outers;
    for (ClassType *O = RC->getOuter(); O; O = O->getOuter())
      Outers.push_back(O);
    for (ClassType *O : llvm::reverse(Outers)) {
      OuterScopes.push_back(std::make_unique<Scope>(Scope::nestedIn(Base, O)));
      Base = OuterScopes.back().get();
    }
  }
  // A method sees the members of its class through Self.
  std::unique_ptr<Scope> RecordScope, ClassScope, FieldsScope;
  // (a method-pointer lambda outside methods: a Self no one sees)
  bool SelfSeen = R->hasSelf() && R->getClass();
  if (SelfSeen) {
    auto *SelfRef = Ctx.create<NameRefExpr>(R->getSelfVar()->getName());
    SelfRef->setDecl(R->getSelfVar());
    SelfRef->setType(R->getSelfVar()->getType());
    auto *W = Ctx.create<WithContext>(SelfRef, /*IsSelf=*/true);
    W->Routine = R;
    auto *RT = llvm::dyn_cast_or_null<RecordType>(
        R->getClass()->getHelperFor());
    if (R->getClass()->isHelper())
      W->Helper = R->getClass();
    // A helper's method for an advanced record also sees the record's
    // methods and properties (the helper's own members first).
    if (ClassType *MT = memberTableOf(RT)) {
      RecordScope = std::make_unique<Scope>(Base, MT, W);
      Base = RecordScope.get();
    }
    ClassScope = std::make_unique<Scope>(Base, R->getClass(), W);
    // A record helper's method also sees the record's fields.
    if (RT)
      FieldsScope = std::make_unique<Scope>(ClassScope.get(), RT, W);
  } else if (R->isStatic()) { // a record's static method: its statics
    ClassScope = std::make_unique<Scope>(Base, R->getClass(), nullptr);
  }
  Scope RoutineScope(FieldsScope  ? FieldsScope.get()
                     : ClassScope ? ClassScope.get()
                                  : Base);
  (RecordScope  ? RecordScope.get()
   : ClassScope ? ClassScope.get()
                : &RoutineScope)
      ->setRoutineBoundary();
  Scope *SavedScope = Cur;
  RoutineDecl *SavedRoutine = CurRoutine;
  Cur = &RoutineScope;
  CurRoutine = R;

  if (SelfSeen)
    Cur->insert("self", R->getSelfVar());
  for (VarDecl *P : R->getParams())
    if (Decl *Prev = Cur->insert(P->getName().Name, P)) {
      Diags.error(P->getLocation(),
                  "duplicate parameter name '" + P->getName().Spelling + "'",
                  {P->getName().getRange()});
      if (Prev->isPredeclared())
        Diags.note(P->getLocation(), "'Self' is the object a method is "
                                     "called on");
      else
        Diags.note(Prev->getLocation(), "previous parameter is here");
    }
  // objfpc: `Result` names the function result.
  if (R->hasResultName() && R->isFunction() && R->getResultVar())
    Cur->insert("result", R->getResultVar());

  checkBlock(*R->getBody());

  if (R->isFunction() && !ResultAssigned.count(R) && R->getResultType() &&
      !R->getResultType()->isError())
    Diags.warning(R->getName().Loc,
                  "function '" + R->getName().Spelling +
                      "' never assigns its result",
                  {R->getName().getRange()});

  Cur = SavedScope;
  CurRoutine = SavedRoutine;
}


// --- compiletime functions ------------------------------------------------
// A compiletime function (and the routines nested in it) runs while
// compiling: no I/O, pointers, objects, procedural values or files, no
// global variables (typed constants can be read), and it calls only
// compiletime functions, its nested routines and the builtins without
// side effects.

/// The compiletime function the current routine is (or is nested in).
RoutineDecl *Checker::compiletimeRoutine() const{
  for (RoutineDecl *R = CurRoutine; R; R = R->getParent())
    if (R->isCompiletime())
      return R;
  return nullptr;
}


/// Is R the current routine, or one around it?
bool Checker::enclosesCurrent(const RoutineDecl *R) const{
  for (RoutineDecl *E = CurRoutine; E; E = E->getParent())
    if (E == R)
      return true;
  return false;
}


/// F, if E is a call of compiletime function F (F or F(args)).
RoutineDecl *Checker::compiletimeCallee(Expr *E){
  Identifier Id;
  if (auto *C = llvm::dyn_cast_or_null<CallExpr>(E))
    Id = C->getCallee();
  else if (auto *N = llvm::dyn_cast_or_null<NameRefExpr>(E))
    Id = N->getName();
  else
    return nullptr;
  auto *R = llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookup(Id.Name));
  for (RoutineDecl *X = R; X; X = X->getNextOverload())
    if (X->isCompiletime())
      return X;
  return nullptr;
}


bool Checker::compiletimeBuiltinAllowed(BuiltinKind K){
  switch (K) {
  case BuiltinKind::Write: case BuiltinKind::Writeln:
  case BuiltinKind::Read: case BuiltinKind::Readln:
  case BuiltinKind::Reset: case BuiltinKind::Rewrite:
  case BuiltinKind::Get: case BuiltinKind::Put: case BuiltinKind::Page:
  case BuiltinKind::Eof: case BuiltinKind::Eoln:
  case BuiltinKind::Assign: case BuiltinKind::AssignFile:
  case BuiltinKind::Close: case BuiltinKind::CloseFile:
  case BuiltinKind::Flush: case BuiltinKind::BlockRead:
  case BuiltinKind::BlockWrite: case BuiltinKind::Seek:
  case BuiltinKind::FilePos: case BuiltinKind::FileSize:
  case BuiltinKind::GetFileHandle: case BuiltinKind::GetMem:
  case BuiltinKind::FreeMem: case BuiltinKind::ReallocMem:
  case BuiltinKind::Halt: case BuiltinKind::ParamCount:
  case BuiltinKind::ParamStr: case BuiltinKind::ExceptAddr:
  case BuiltinKind::New: case BuiltinKind::Dispose:
  case BuiltinKind::Wait: case BuiltinKind::WhenAll:
  case BuiltinKind::WhenAny:
    return false;
  default:
    return true;
  }
}


void Checker::compiletimeError(RoutineDecl *CT, SourceLocation L, SourceRange R,
                      const llvm::Twine &What){
  Diags.error(L,
              "compiletime function '" + CT->getName().Spelling +
                  "' cannot " + What,
              {R});
}


/// A use of D (a name, a call) in compiletime function CT.
void Checker::checkCompiletimeUse(RoutineDecl *CT, Decl *D, SourceLocation L,
                         SourceRange Range, bool Write){
  if (!D)
    return;
  if (auto *B = llvm::dyn_cast<BuiltinDecl>(D)) {
    if (!compiletimeBuiltinAllowed(B->getBuiltinKind()))
      compiletimeError(CT, L, Range,
                       "call '" + B->getName().Spelling +
                           "' (no input, output or memory management)");
    return;
  }
  if (auto *V = llvm::dyn_cast<VarDecl>(D)) {
    if (V->isPredeclared())
      return compiletimeError(CT, L, Range,
                              "use '" + V->getName().Spelling + "'");
    if (V->isTypedConst() && !V->getOwner()) {
      if (Write)
        compiletimeError(CT, L, Range,
                         "assign to typed constant '" +
                             V->getName().Spelling + "'");
      return;
    }
    if (!V->getOwner() || !enclosesCurrent(V->getOwner()))
      compiletimeError(CT, L, Range,
                       "use variable '" + V->getName().Spelling +
                           "' (only its own parameters and variables)");
    return;
  }
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) {
    if (R->isMethod() || R->getClass())
      return compiletimeError(CT, L, Range,
                              "call method '" + R->getName().Spelling + "'");
    RoutineDecl *C = R->getCanonical() ? R->getCanonical() : R;
    if (C->isCompiletime() || R->isCompiletime())
      return;
    for (RoutineDecl *P = C->getParent(); P; P = P->getParent())
      if (P == CT)
        return; // nested in it
    compiletimeError(CT, L, Range,
                     "call '" + R->getName().Spelling +
                         "', which is not compiletime");
  }
}


/// E (of type T) in compiletime function CT.
void Checker::checkCompiletimeExpr(RoutineDecl *CT, Expr *E, Type *T){
  if (T && !T->isError()) {
    const char *What = nullptr;
    switch (T->getKind()) {
    case Type::TK_Pointer: case Type::TK_Nil: What = "pointers"; break;
    case Type::TK_Class: case Type::TK_ClassRef: case Type::TK_Interface:
      What = "objects";
      break;
    case Type::TK_File: What = "files"; break;
    case Type::TK_Routine: What = "procedural values"; break;
    case Type::TK_Task: case Type::TK_Token: What = "tasks"; break;
    default: break;
    }
    if (What) {
      compiletimeError(CT, E->getBeginLoc(), E->getRange(),
                       llvm::Twine("use ") + What);
      return;
    }
  }
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
    checkCompiletimeUse(CT, N->getDecl(), N->getName().Loc,
                        N->getName().getRange());
  else if (auto *C = llvm::dyn_cast<CallExpr>(E))
    checkCompiletimeUse(CT, C->getDecl(), C->getCallee().Loc,
                        C->getCallee().getRange());
  else if (llvm::isa<IndirectCallExpr>(E) || llvm::isa<InheritedExpr>(E))
    compiletimeError(CT, E->getBeginLoc(), E->getRange(),
                     "call methods or procedural values");
}


/// ISO 7185 6.6.3.6: a procedure/function passed as an argument must have
/// congruent parameter lists (same kinds, identical types) and result.
bool Checker::congruent(llvm::ArrayRef<VarDecl *> A, Type *AResult,
                      llvm::ArrayRef<VarDecl *> B, Type *BResult){
  if (A.size() != B.size() || (AResult == nullptr) != (BResult == nullptr) ||
      (AResult && AResult != BResult))
    return false;
  for (size_t I = 0; I < A.size(); ++I) {
    if (A[I]->getVarKind() != B[I]->getVarKind() ||
        A[I]->isOutParam() != B[I]->isOutParam() ||
        A[I]->isConstRefParam() != B[I]->isConstRefParam())
      return false;
    Type *TA = A[I]->getType(), *TB = B[I]->getType();
    auto *RA = llvm::dyn_cast_or_null<RoutineType>(TA);
    auto *RB = llvm::dyn_cast_or_null<RoutineType>(TB);
    if (RA && RB) {
      if (RA->isCDecl() != RB->isCDecl() ||
          RA->isOfObject() != RB->isOfObject() ||
          RA->isReference() != RB->isReference() ||
          !congruent(RA->getParams(), RA->getResult(), RB->getParams(),
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
void Checker::checkRoutineArg(Expr *Arg, VarDecl *P, RoutineType *Formal){
  auto *LA = llvm::dyn_cast<AddrOfExpr>(Arg);
  if (LA) { // a lambda: its types
    completeLambda(LA, Formal);
    if (LA->getLambda() && LA->getLambda()->isLambdaBad())
      return;
  }
  // @obj.M, or a method pointer; a closure (any value of the type)
  if (Formal->isOfObject() || Formal->isReference()) {
    Type *AT = checkValueFor(Formal, Arg);
    if (AT->isError())
      return;
    if (!isAssignable(Formal, AT))
      Diags.error(Arg->getBeginLoc(),
                  "cannot pass a value of type " + quoted(AT) + " to parameter '" +
                      P->getName().Spelling + "' of type " + quoted(Formal),
                  {Arg->getRange()});
    else if (P->isVarParam() && isBitField(Arg))
      diagBitFieldAddress(Arg);
    else if (P->isVarParam() && !isVariableReference(Arg))
      Diags.error(Arg->getBeginLoc(),
                  "argument for '" + llvm::Twine(paramKeyword(P)) + "' parameter '" + P->getName().Spelling +
                      "' must be a variable",
                  {Arg->getRange()});
    else if (P->isVarParam())
      checkModifiable(Arg);
    return;
  }
  const char *Kind = Formal->isFunction() ? "function" : "procedure";
  // Turbo/FPC: @f, as objfpc requires (the operand: a routine's name).
  auto *A = isISO() ? nullptr : llvm::dyn_cast<AddrOfExpr>(Arg);
  if (A)
    A->setType(P->getType());
  if (A && llvm::isa<FieldExpr>(A->getOperand())) { // @TFoo.M (static)
    const Identifier &Name = llvm::cast<FieldExpr>(A->getOperand())->getField();
    RoutineType *SavedTarget = AddrOfTarget;
    AddrOfTarget = Formal;
    A->setType(nullptr);
    Type *T = check(A);
    AddrOfTarget = SavedTarget;
    A->setType(P->getType());
    if (T->isError())
      return;
    RoutineDecl *R = A->getCode();
    if (!R) {
      Diags.error(Arg->getBeginLoc(),
                  llvm::Twine("argument for ") + Kind + " parameter '" +
                      P->getName().Spelling + "' must be a " + Kind +
                      " or a static class method",
                  {Arg->getRange()});
      return;
    }
    if (R->isCDecl() != Formal->isCDecl() ||
        !congruent(Formal->getParams(), Formal->getResult(), R->getParams(),
                   R->isFunction() ? R->getResultType() : nullptr))
      Diags.error(Arg->getBeginLoc(),
                  "'" + Name.Spelling +
                      "' does not match the heading of parameter '" +
                      P->getName().Spelling + "' (" + quoted(Formal) + ")",
                  {Arg->getRange()});
    return;
  }
  auto *N = llvm::dyn_cast<NameRefExpr>(A ? A->getOperand() : Arg);
  Decl *D = N ? Cur->lookup(N->getName().Name) : nullptr;
  llvm::ArrayRef<VarDecl *> Params;
  Type *Result = nullptr;
  bool Found = false, CDecl = false;
  if (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D); R && !R->isMethod()) {
    // objfpc: of overloaded routines, the one whose heading fits
    for (RoutineDecl *X = R; R->isOverloaded() && X; X = X->getNextOverload())
      if (X->isCDecl() == Formal->isCDecl() &&
          congruent(Formal->getParams(), Formal->getResult(), X->getParams(),
                    X->isFunction() ? X->getResultType() : nullptr)) {
        D = R = X;
        break;
      }
    Params = R->getParams();
    Result = R->isFunction() ? R->getResultType() : nullptr;
    CDecl = R->isCDecl();
    Found = true;
  } else if (auto *V = llvm::dyn_cast_or_null<VarDecl>(D)) {
    if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(V->getType())) {
      Params = RT->getParams();
      Result = RT->getResult();
      CDecl = RT->isCDecl();
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
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) {
    std::string Why = notProceduralValue(R, !Formal->isCDecl());
    if (!Why.empty()) {
      Diags.error(Arg->getBeginLoc(),
                  "'" + N->getName().Spelling + "': " + Why,
                  {Arg->getRange()});
      return;
    }
  }
  if (CDecl != Formal->isCDecl() ||
      !congruent(Formal->getParams(), Formal->getResult(), Params, Result))
    Diags.error(Arg->getBeginLoc(),
                (LA && LA->getLambda()
                     ? std::string("this lambda")
                     : "'" + N->getName().Spelling.str() + "'") +
                    " does not match the heading of parameter '" +
                    P->getName().Spelling + "' (" + quoted(Formal) + ")",
                {Arg->getRange()});
}


/// An argument (number I, from 0) beyond the parameters of a 'varargs'
/// external routine: a value C can take (ordinals are passed as int at
/// least, reals as double, string constants as C strings).
void Checker::checkVarArg(Expr *Arg, size_t I, const Identifier &Callee){
  if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
    Diags.error(F->getWidth()->getBeginLoc(),
                "field widths are only allowed in 'write' and 'writeln'",
                {F->getWidth()->getRange()});
    return;
  }
  Type *T = check(Arg);
  auto *N = llvm::dyn_cast<NameRefExpr>(Arg);
  bool StringConst = T->isStringLiteral() &&
                     (llvm::isa<StringLiteralExpr>(Arg) ||
                      (N && llvm::isa_and_nonnull<ConstDecl>(N->getDecl())));
  if (T->isError() || T->isOrdinal() || T->isReal() || T->isPointer() ||
      T->isClass() || T->isNil() || StringConst)
    return;
  Diags.error(Arg->getBeginLoc(),
              "argument " + llvm::Twine(I + 1) + " of '" + Callee.Spelling +
                  "' has type " + quoted(T) +
                  "; a 'varargs' argument must be an integer, real, char, "
                  "boolean, pointer or string constant",
              {Arg->getRange()});
}


/// Why routine R cannot be a procedural value (empty: it can). Nested
/// routines can be passed to ISO procedural parameters (AllowNested).
std::string Checker::notProceduralValue(RoutineDecl *R, bool AllowNested){
  if (R->getParent() && !AllowNested && !R->isLambda())
    return "only routines declared at program or unit level can be "
           "procedural values";
  if (!R->isExternal())
    return "";
  if (!R->isCDecl())
    return "an external routine must be 'cdecl' to be a procedural value";
  if (R->isVarArgs())
    return "a 'varargs' routine cannot be a procedural value";
  for (VarDecl *P : R->getParams())
    if (llvm::isa_and_nonnull<RoutineType>(P->getType()))
      return "an external routine with procedural parameters cannot be a "
             "procedural value";
  return "";
}


/// Checks the arguments of a call to a user routine.
void Checker::checkCallArgs(RoutineDecl *R, const Identifier &Callee,
                   llvm::ArrayRef<Expr *> Args, SourceRange CallRange){
  checkCallArgs(R->getParams(), R, Callee, Args, CallRange);
  if (R->isAsync() || R->isIterator()) // (copies, which it keeps)
    for (Expr *A : Args)
      checkIsolatedCopy(A, "passed to a task");
}


void Checker::checkCallArgs(llvm::ArrayRef<VarDecl *> Params, Decl *CalleeDecl,
                   const Identifier &Callee, llvm::ArrayRef<Expr *> Args,
                   SourceRange CallRange){
  Decl *R = CalleeDecl;
  if (ArgsFailed) { // named arguments that do not fit: reported
    ArgsFailed = false;
    for (Expr *A : Args)
      if (!llvm::isa<FormattedExpr>(argValue(A)))
        check(argValue(A));
    return;
  }
  // A call whose result is discarded still holds it for a while.
  if (auto *RD = llvm::dyn_cast_or_null<RoutineDecl>(CalleeDecl);
      RD && RD->isFunction() && RD->getResultType() &&
      isManaged(RD->getResultType()))
    markUsesManaged();
  if (auto *V = llvm::dyn_cast_or_null<VarDecl>(CalleeDecl))
    if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(V->getType());
        RT && RT->getResult() && isManaged(RT->getResult()))
      markUsesManaged();
  auto *RD = llvm::dyn_cast_or_null<RoutineDecl>(CalleeDecl);
  if (RD && RD->isExternal())
    warnUnsafe(Callee.Loc,
               "call to external routine '" + Callee.Spelling + "' outside 'unsafe'",
               {Callee.getRange()});
  bool VarArgs = RD && RD->isVarArgs();
  if (!acceptsArgCount(Params, Args.size()) &&
      !(VarArgs && Args.size() > Params.size())) {
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
    if (I >= Params.size()) { // varargs
      checkVarArg(Arg, I, Callee);
      continue;
    }
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
    if (auto *OA = llvm::dyn_cast_or_null<OpenArrayType>(P->getType())) {
      if (RD && RD->isCDecl())
        Diags.error(Arg->getBeginLoc(),
                    "an open array cannot be passed to a 'cdecl' routine",
                    {Arg->getRange()});
      else
        checkOpenArrayArg(Arg, P, OA);
      continue;
    }
    if (P->getType() && P->getType()->isArrayOfConst()) {
      if (RD && RD->isCDecl()) {
        Diags.error(Arg->getBeginLoc(),
                    "an array of const cannot be passed to a 'cdecl' routine",
                    {Arg->getRange()});
        continue;
      }
      if (auto *SC = llvm::dyn_cast<SetCtorExpr>(Arg)) {
        checkConstArray(SC, P->getType());
        continue;
      }
      Type *AT = check(Arg);
      if (!AT->isError() && !AT->isArrayOfConst())
        Diags.error(Arg->getBeginLoc(),
                    "cannot pass a value of type " + quoted(AT) +
                        " to parameter '" + P->getName().Spelling +
                        "' of type 'array of const'",
                    {Arg->getRange()});
      continue;
    }
    if (P->getType() && P->getType()->isUntyped()) {
      // Turbo/FPC: any variable, passed by reference
      if (checkAllowingUntyped(Arg)->isError())
        continue;
      if (!isVariableReference(Arg))
        Diags.error(Arg->getBeginLoc(),
                    "argument for untyped parameter '" +
                        P->getName().Spelling + "' must be a variable",
                    {Arg->getRange()});
      else if (P->isVarParam())
        checkModifiable(Arg);
      markAliasedBase(Arg);
      continue;
    }
    Type *PT = P->getType();
    Type *AT = PT && (llvm::isa<AddrOfExpr>(Arg) ||
                      llvm::isa<TupleExpr>(Arg) ||
                      (PT->isDynArray() && llvm::isa<SetCtorExpr>(Arg)))
                   ? checkValueFor(PT, Arg)
                   : check(Arg);
    if (AT->isError() || !PT || PT->isError())
      continue;
    const Identifier &PName = P->getName();

    if (P->isVarParam()) {
      if (isBitField(Arg)) {
        diagBitFieldAddress(Arg);
        continue;
      }
      if (!isVariableReference(Arg)) {
        Diags.error(Arg->getBeginLoc(),
                    "argument for '" + llvm::Twine(paramKeyword(P)) + "' parameter '" + PName.Spelling +
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
                    "argument of type " + quoted(AT) + " for '" +
                        llvm::Twine(paramKeyword(P)) + "' parameter '" +
                        PName.Spelling + "' must have exactly "
                    "type " + quoted(PT),
                    {Arg->getRange()});
      else {
        checkNotForControlVar(Arg);
        nullVarArg(Arg, P);
      }
      continue;
    }
    checkWideToChar(PT, Arg);
    if (!isAssignable(PT, AT) && !isPCharLiteral(PT, Arg) &&
        !convertImplicitly(Arg, PT))
      Diags.error(Arg->getBeginLoc(),
                  "cannot pass a value of type " + quoted(AT) +
                      " to parameter '" + PName.Spelling + "' of type " +
                      quoted(PT),
                  {Arg->getRange()});
    else {
      if (P->isConstRefParam() && AT == PT && !Arg->getConversion() &&
          isConstRefAddressable(Arg))
        Arg->setConstRefAddress(); // its address (else a temporary's)
      if (isNullTrackedType(PT) && declNonNull(P) &&
          !(PT->isPointer() && isUnsafe(Arg->getBeginLoc())))
        requireNotNil(Arg, "but parameter '" + PName.Spelling.str() +
                               "' is of the non-null type " + quoted(PT));
    }
  }
}


/// @obj.M, @TFoo.M, or @M in a method: a method pointer. Null if the
/// operand is not a method.
Type *Checker::checkMethodAddr(AddrOfExpr *E){
  bool WantsCode = AddrOfWantsCode;
  AddrOfWantsCode = false;
  if (!LangOpts.ObjKeywords)
    return nullptr;
  Expr *Op = E->getOperand();
  RoutineDecl *R = nullptr;
  Identifier Name;
  bool ViaClass = false;
  auto *F = llvm::dyn_cast<FieldExpr>(Op);
  auto *N = llvm::dyn_cast<NameRefExpr>(Op);
  if (F) {
    if (!isMethodCallee(F))
      return nullptr;
    ClassType *Named = classNamedBy(F->getBase());
    ViaClass = Named != nullptr;
    const ClassType *Owner = nullptr;
    auto *M = Named ? llvm::dyn_cast_or_null<RoutineDecl>(
                          Named->findMember(F->getField().Name, &Owner))
                    : nullptr;
    if (M && (!M->isClassMethod() || WantsCode)) { // @TFoo.M: its code
      const Identifier &Field = F->getField();
      const char *Why = nullptr;
      if (M->isConstructor() || M->isDestructor())
        Why = "constructors and destructors have no code address here";
      else if (M->getBuiltin() != TObjectMethod::None)
        Why = "the methods of TObject have no code address";
      else if (M->isAbstract())
        Why = "abstract methods have no code";
      if (Why) {
        Diags.error(Field.Loc, "'@" + Field.Spelling + "': " + Why,
                    {Field.getRange()});
        return errorType();
      }
      checkMemberAccess(M, Owner, Field);
      F->setMember(M);
      Used.insert(M);
      warnHints(M, Field);
      E->setCode(M);
      return Ctx.getUntypedPointerType();
    }
    Decl *D = resolveMember(F);
    if (!D)
      return errorType();
    R = llvm::dyn_cast<RoutineDecl>(D);
    Name = F->getField();
    if (R && F->getBase()->getType() && F->getBase()->getType()->isInterface()) {
      Diags.error(Name.Loc, "'@" + Name.Spelling +
                                "': interface methods cannot be method pointers",
                  {Name.getRange()});
      return errorType();
    }
  } else if (N) {
    WithContext *W = nullptr;
    R = llvm::dyn_cast_or_null<RoutineDecl>(Cur->lookup(N->getName().Name, &W));
    if (R && R->isMethod()) {
      N->setWith(W);
      Name = N->getName();
      checkScopeAccess(R, W, Name);
    } else {
      R = nullptr;
    }
  }
  if (!R)
    return nullptr;
  if (R->isOverloaded()) {
    RoutineDecl *Pick = nullptr;
    for (RoutineDecl *X = R; AddrOfTarget && X; X = X->getNextOverload())
      if (!Pick && AddrOfTarget->isOfObject() &&
          congruent(AddrOfTarget->getParams(), AddrOfTarget->getResult(),
                    X->getParams(), X->isFunction() ? X->getResultType() : nullptr))
        Pick = X;
    if (!Pick) {
      Diags.error(Name.Loc,
                  "'@" + Name.Spelling + "': '" + Name.Spelling +
                      "' is overloaded; assign it to a method pointer of the "
                      "heading wanted",
                  {Name.getRange()});
      return errorType();
    }
    R = Pick;
  }
  const char *Why = nullptr;
  if (R->isConstructor() || R->isDestructor())
    Why = "constructors and destructors cannot be method pointers";
  else if (R->getBuiltin() != TObjectMethod::None)
    Why = "the methods of TObject cannot be method pointers";
  if (Why) {
    Diags.error(Name.Loc, "'@" + Name.Spelling + "': " + Why, {Name.getRange()});
    return errorType();
  }
  if (R->isStatic()) { // a routine without Self: a procedural value
    if (F)
      F->setMember(R);
    else
      N->setDecl(R);
    Used.insert(R);
    warnHints(R, Name);
    E->setCode(R);
    return Ctx.create<RoutineType>(R->getParams(),
                                   R->isFunction() ? R->getResultType() : nullptr,
                                   R->isCDecl(), false,
                                   AddrOfTarget && AddrOfTarget->isReference());
  }
  if (F)
    F->setMember(R);
  else
    N->setDecl(R);
  Used.insert(R);
  warnHints(R, Name);
  E->setMethod(R);
  return Ctx.create<RoutineType>(R->getParams(),
                                 R->isFunction() ? R->getResultType() : nullptr,
                                 R->isCDecl(), /*OfObject=*/true);
}


/// The lambda A (function(x) => e), used where a value of type Want is
/// wanted (null: none): its hidden routine is declared here, nested in
/// the current routine and in the current scope. Parameter types left
/// out are Want's; the result type left out is Want's, or else the type
/// of the body (set by visitAssignStmt). Then A is @routine.
void Checker::completeLambda(AddrOfExpr *A, RoutineType *Want){
  RoutineDecl *R = A->getLambda();
  if (!R || R->isLambdaDone())
    return;
  R->setLambdaDone();
  if (Want && Want->isReference())
    R->setClosure();
  if (Want && Want->isOfObject() && !makeMethodLambda(R, A))
    return;
  auto Params = R->getParams();
  if (Want && Want->getParams().size() != Params.size()) {
    Diags.error(A->getBeginLoc(),
                "this lambda has " + llvm::Twine(Params.size()) +
                    (Params.size() == 1 ? " parameter" : " parameters") +
                    ", but " + quoted(Want) + " is wanted here",
                {A->getRange()});
    R->setLambdaBad();
  }
  for (size_t I = 0; I < Params.size(); ++I) {
    VarDecl *P = Params[I];
    P->setOwner(R);
    Type *T;
    if (P->getTypeSpec()) {
      T = resolveType(P->getTypeSpec());
    } else if (Want && I < Want->getParams().size() &&
               Want->getParams()[I]->getType()) {
      VarDecl *WP = Want->getParams()[I];
      T = WP->getType();
      if (P->getVarKind() == VarDecl::ValueParam) { // (x): as wanted
        P->setVarKind(WP->getVarKind());
        if (WP->isOutParam())
          P->setOutParam();
        if (WP->isConstRefParam())
          P->setConstRefParam();
      }
    } else {
      R->setLambdaBad();
      if (!Want)
        Diags.error(P->getLocation(),
                    "cannot infer the type of parameter '" +
                        P->getName().Spelling +
                        "' in this lambda: there is no expected type; add "
                        "a type, for example function(" +
                        P->getName().Spelling + ": Integer) => ...",
                    {P->getName().getRange()});
      T = errorType();
    }
    P->setType(T);
  }
  if (R->isFunction()) {
    Type *ResultTy = nullptr; // (not known yet: the body's)
    if (R->getResultTypeSpec())
      ResultTy = resolveType(R->getResultTypeSpec());
    else if (Want && Want->getResult())
      ResultTy = Want->getResult();
    R->setResultType(ResultTy);
    auto *Result = Ctx.create<VarDecl>(R->getName(), R->getResultTypeSpec(),
                                       VarDecl::Result);
    Result->setType(ResultTy);
    Result->setOwner(R);
    R->setResultVar(Result);
  }
  declareAndCheckLambda(R);
  if (R->isFunction() && !R->getResultType()) {
    // (a block body that never sets Result: an expression body always
    // gives a type, or an error)
    Diags.error(A->getBeginLoc(),
                "cannot tell the result type of this lambda: its body does "
                "not set Result; give it one: function(...): T => ...",
                {A->getRange()});
    R->setResultType(errorType());
    R->getResultVar()->setType(errorType());
  }
}


/// Lambda R (or a copy factory), its types known: declared here, nested
/// in the current routine, and its body checked now, in the middle of a
/// statement.
void Checker::declareAndCheckLambda(RoutineDecl *R){
  R->setParent(CurRoutine);
  Used.insert(R);
  // In the innermost scope of declarations (not that of a with statement
  // or of a class's members, where it would not be found).
  Scope *Saved = Cur;
  while (Cur->isMemberScope() && Cur->getParent())
    Cur = Cur->getParent();
  declare(R);
  Cur = Saved;
  unsigned SavedLoop = LoopDepth;
  const void *SavedSeq = DirectSequence;
  std::unique_ptr<Scope> *SavedSeqScope = SeqScope;
  Expr *SavedAssign = CurAssignTarget, *SavedOwned = OwnedTarget;
  Expr *SavedIndex = IndexBase, *SavedWanted = WantedCall;
  RoutineType *SavedAddr = AddrOfTarget;
  bool SavedKeep = KeepCharLiterals;
  LoopDepth = 0;
  DirectSequence = nullptr;
  SeqScope = nullptr;
  CurAssignTarget = OwnedTarget = IndexBase = WantedCall = nullptr;
  AddrOfTarget = nullptr;
  KeepCharLiterals = false;
  checkRoutineBody(R);
  LoopDepth = SavedLoop;
  DirectSequence = SavedSeq;
  SeqScope = SavedSeqScope;
  CurAssignTarget = SavedAssign;
  OwnedTarget = SavedOwned;
  IndexBase = SavedIndex;
  WantedCall = SavedWanted;
  AddrOfTarget = SavedAddr;
  KeepCharLiterals = SavedKeep;
}


/// The copy factory F of a lambda that copies what it uses, called by E
/// where Want is wanted: a closure must be. Its parameters, the copies,
/// take the types of the variables they copy (read-only); its result is
/// Want, its body makes the lambda (completed for Want). False after a
/// diagnostic.
bool Checker::completeCopyFactory(RoutineDecl *F, CallExpr *E, Type *Want){
  F->setLambdaDone();
  auto *RT = llvm::dyn_cast_or_null<RoutineType>(Want);
  if (!RT || !RT->isReference()) {
    Diags.error(E->getBeginLoc(),
                "a lambda that copies what it uses must be a closure: a "
                "'reference to' type must be wanted here",
                {E->getRange()});
    F->setLambdaBad();
    return false;
  }
  if (F->isCopyAll()) { // the names that are variables of routines
    llvm::SmallVector<VarDecl *, 4> Kept;
    llvm::SmallVector<Expr *, 4> KeptArgs;
    for (size_t I = 0; I < F->getParams().size(); ++I) {
      WithContext *W = nullptr;
      auto *V = llvm::dyn_cast_or_null<VarDecl>(
          Cur->lookup(F->getParams()[I]->getName().Name, &W));
      Type *T = V ? V->getType() : nullptr;
      if (V && !W && V->getOwner() && !V->isTypedConst() &&
          !V->isClassVar() && T && !T->isOpenArray() &&
          !T->isArrayOfConst() && !T->isUntyped() && !containsFile(T)) {
        Kept.push_back(F->getParams()[I]);
        KeptArgs.push_back(E->getArgs()[I]);
      }
    }
    F->setParams(Ctx.copyArray<VarDecl *>(Kept));
    E->setArgs(Ctx.copyArray<Expr *>(KeptArgs));
  }
  bool OK = true;
  for (VarDecl *P : F->getParams()) {
    const Identifier &Id = P->getName();
    Decl *D = Cur->lookup(Id.Name);
    Type *T = nullptr;
    if (auto *V = llvm::dyn_cast_or_null<VarDecl>(D))
      T = V->getType();
    else if (auto *Fd = llvm::dyn_cast_or_null<FieldDecl>(D))
      T = Fd->getType(); // (an object's field, through Self or a with)
    const char *Why = nullptr;
    if (!T)
      Why = "a variable, a parameter or a field";
    else if (T->isOpenArray() || T->isArrayOfConst() || T->isUntyped())
      Why = "of a type that can be copied, not an open or untyped parameter";
    else if (containsFile(T))
      Why = "of a type that can be copied, not a file";
    if (Why) {
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' in the list of what a lambda copies "
                  "must be " + Why,
                  {Id.getRange()});
      OK = false;
      T = errorType();
    }
    P->setType(T);
    P->setOwner(F);
    P->setReadOnly();
  }
  if (!OK) {
    F->setLambdaBad();
    return false;
  }
  F->setResultType(RT);
  auto *Result = Ctx.create<VarDecl>(F->getName(), nullptr, VarDecl::Result);
  Result->setType(RT);
  Result->setOwner(F);
  F->setResultVar(Result);
  declareAndCheckLambda(F);
  return true;
}


/// Lambda R (at A) wanted as a method pointer: it gets a Self of its own,
/// of the class of the method it is written in (the nearest routine
/// around it with a Self, which must be an object), and that method's
/// Self is its object. Reported (and R bad) if there is none.
bool Checker::makeMethodLambda(RoutineDecl *R, AddrOfExpr *A){
  RoutineDecl *M = CurRoutine;
  while (M && !M->hasSelf())
    M = M->getParent();
  VarDecl *OuterSelf = M ? M->getSelfVar() : nullptr;
  if (!M) { // outside methods: no object (nil), no members
    auto *Self = Ctx.create<VarDecl>(memberName("Self"), nullptr,
                                     VarDecl::ValueParam);
    Self->setType(Ctx.getUntypedPointerType());
    Self->setOwner(R);
    R->setSelfVar(Self);
    R->setMethodLambda(nullptr);
    return true;
  }
  if (!OuterSelf || !OuterSelf->getType() || !OuterSelf->getType()->isClass()) {
    Diags.error(A->getBeginLoc(),
                "a lambda can be a method pointer ('of object') only in "
                "an object's method, whose Self is its object, or outside "
                "methods (its object is nil)",
                {A->getRange()});
    R->setLambdaBad();
    return false;
  }
  auto *C = llvm::cast<ClassType>(OuterSelf->getType());
  R->setClass(C);
  makeSelf(R, C);
  R->setMethodLambda(OuterSelf);
  // Its object: M's Self, where the lambda is made.
  if (M != CurRoutine) {
    OuterSelf->setCaptured();
    Identifier Id = memberName("Self");
    Id.Loc = A->getBeginLoc();
    noteLambdaUse(OuterSelf, nullptr, Id);
  }
  return true;
}


/// A use, in a lambda, of D (named Id, found through W): if D belongs to
/// a routine around the lambda (a variable, a member through Self or a
/// with statement, a nested routine), the lambdas up to that routine
/// must not outlive it (RoutineDecl::getCapture).
///
/// A closure (a lambda made a `reference to` value) may outlive them:
/// their frames it uses are then kept on the heap (setHeapFrame), and
/// it must not use what does not live in a frame (closureCannotUse).
void Checker::noteLambdaUse(Decl *D, WithContext *W, const Identifier &Id){
  noteTaskUse(D, W, Id);
  if (!CurRoutine || !CurRoutine->isLambda() || !D)
    return;
  RoutineDecl *Owner = nullptr, *Nested = nullptr;
  VarDecl *Var = nullptr;
  if (W) {
    if (W->Routine == CurRoutine)
      return;
    if (!W->IsSelf) {
      // Its address is the enclosing routine's to compute, not the
      // lambda's.
      Diags.error(Id.Loc,
                  "this lambda uses '" + Id.Spelling +
                      "' through a 'with' statement outside it; name the "
                      "record instead",
                  {Id.getRange()});
      return;
    }
    Owner = W->Routine;
    Var = Owner ? Owner->getSelfVar() : nullptr;
  } else if (auto *V = llvm::dyn_cast<VarDecl>(D)) {
    if (!V->getOwner() || V->getOwner() == CurRoutine || V->isTypedConst() ||
        V->isClassVar())
      return;
    Owner = V->getOwner();
    Var = V;
  } else if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) {
    if (!R->getParent() || R == CurRoutine)
      return;
    Owner = R->getParent();
    Nested = R;
  } else {
    return;
  }
  for (RoutineDecl *L = CurRoutine; L && L != Owner; L = L->getParent()) {
    if (!L->isLambda())
      continue;
    if (L->isMethodLambda()) { // called on its object, without a frame
      Diags.error(Id.Loc,
                  "this method-pointer lambda uses '" + Id.Spelling +
                      "' of the routine around it; it can use only its "
                      "object's members, its parameters and what is "
                      "declared at program or unit level",
                  {Id.getRange()});
      return;
    }
    if (!L->getCapture().isValid())
      L->setCapture(Id);
    if (!L->isClosure())
      continue;
    if (Nested && !Nested->isLambda()) {
      Diags.error(Id.Loc,
                  "this closure uses '" + Id.Spelling +
                      "', a routine nested in '" + Owner->getName().Spelling +
                      "'; a closure can only use routines declared at "
                      "program or unit level",
                  {Id.getRange()});
      return;
    }
    if (std::string Why = Var ? closureCannotUse(Var) : ""; !Why.empty()) {
      Diags.error(Id.Loc,
                  "this closure uses '" + Id.Spelling + "', " + Why +
                      ", which may not exist when the closure runs; copy it "
                      "to a local variable first",
                  {Id.getRange()});
      return;
    }
    for (RoutineDecl *F = L->getParent(); F; F = F->getParent()) {
      F->setHeapFrame();
      if (F == Owner)
        break;
    }
  }
}


/// The innermost spawn block around the code being checked, if R (a
/// routine; null: the program) is outside it: the task that would share
/// R's variables. Null otherwise.
RoutineDecl *Checker::spawnOutside(RoutineDecl *R) const{
  for (RoutineDecl *X = CurRoutine; X; X = X->getParent()) {
    if (X == R)
      return nullptr;
    if (X->isSpawnBlock())
      return X;
  }
  return nullptr;
}


/// D (used through W) in a spawn block: an isolated variable of another
/// task is an error; under {$modeswitch racechecks}, so is anything
/// mutable that is not shared: a variable of the routines around it or
/// of the program, Self's members (unless its class is thread-safe), a
/// routine nested in another (which may use its variables).
void Checker::noteTaskUse(Decl *D, WithContext *W, const Identifier &Id){
  if (!D || !CurRoutine)
    return;
  if (W) {
    if (!W->Routine)
      return;
    RoutineDecl *S = spawnOutside(W->Routine);
    if (!S || !S->isRaceChecked())
      return;
    if (W->IsSelf) {
      ClassType *C = W->Routine->getClass();
      auto *F = llvm::dyn_cast<FieldDecl>(D);
      if ((C && isThreadSafe(C)) || (F && F->isShared()))
        return;
      Diags.error(Id.Loc,
                  "this spawned task uses '" + Id.Spelling +
                      "' of Self, which other tasks may use at the same "
                      "time; make it a shared field, or derive the class "
                      "from TSharedObject",
                  {Id.getRange()});
    } else {
      Diags.error(Id.Loc,
                  "this spawned task uses '" + Id.Spelling +
                      "' through a 'with' statement outside it, which "
                      "other tasks may change at the same time",
                  {Id.getRange()});
    }
    return;
  }
  if (auto *V = llvm::dyn_cast<VarDecl>(D)) {
    RoutineDecl *S = spawnOutside(V->getOwner());
    if (!S)
      return;
    if (V->isIsolated()) {
      Diags.error(Id.Loc,
                  "isolated variable '" + Id.Spelling +
                      "' belongs to one task; a spawned task cannot use it "
                      "(hand it to an async routine with 'move " +
                      Id.Spelling + "')",
                  {Id.getRange()});
      noteDeclaredHere(V);
      return;
    }
    if (!S->isRaceChecked() || V->isShared() || V->getType()->isError())
      return;
    bool IsSelf = V->getOwner() && V == V->getOwner()->getSelfVar();
    bool ReadOnly = IsSelf || V->isReadOnly() ||
                    (V->isConstParam() && !V->isConstRefParam());
    if (ReadOnly && !sharesMutableData(V->getType()))
      return;
    Diags.error(Id.Loc,
                "this spawned task uses '" + Id.Spelling +
                    "', which is not shared: another task may change it at "
                    "the same time; declare it shared (of a thread-safe "
                    "type), or pass its value to an async routine",
                {Id.getRange()});
    noteDeclaredHere(V);
    return;
  }
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) {
    if (!R->getParent() || R->isLambda() || R->isSpawnBlock())
      return;
    RoutineDecl *S = spawnOutside(R->getParent());
    if (S && S->isRaceChecked())
      Diags.error(Id.Loc,
                  "this spawned task calls '" + Id.Spelling +
                      "', a routine nested in '" +
                      R->getParent()->getName().Spelling +
                      "', which may use that routine's variables while "
                      "other tasks change them",
                  {Id.getRange()});
  }
}


/// E an isolated variable (by name): it cannot be copied, only moved.
void Checker::checkIsolatedCopy(Expr *E, const char *Where){
  while (auto *P = llvm::dyn_cast<ParenExpr>(E))
    E = P->getInner();
  auto *N = llvm::dyn_cast<NameRefExpr>(E);
  auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(N->getDecl()) : nullptr;
  if (!V || !V->isIsolated())
    return;
  Diags.error(N->getName().Loc,
              "isolated variable '" + N->getName().Spelling +
                  "' cannot be copied (" + Where + "): its data belongs to "
                  "one task; write 'move " + N->getName().Spelling +
                  "' to hand it over",
              {N->getRange()});
}


/// Why a closure cannot use V, a variable of a routine around it (empty:
/// it can): it is not kept in that routine's frame, or is not counted
/// there.
std::string Checker::closureCannotUse(VarDecl *V){
  std::string Of = V->getOwner()->isLambda()
                       ? std::string("the lambda around it")
                       : "'" + V->getOwner()->getName().Spelling.str() + "'";
  if (V->getVarKind() == VarDecl::Result)
    return "the result of " + Of;
  Type *T = V->getType();
  if (V == V->getOwner()->getSelfVar())
    return T && (T->isClass() || T->isClassRef())
               ? ""
               : "through the Self of " + Of + " (a record, passed by reference)";
  bool Param = V->getVarKind() != VarDecl::Variable;
  if (!Param || !T)
    return "";
  if (T->isOpenArray() || T->isArrayOfConst() || T->isUntyped())
    return "an open or untyped parameter of " + Of;
  if (V->isOutParam())
    return "an 'out' parameter of " + Of;
  if (V->isVarParam())
    return "a 'var' parameter of " + Of;
  if ((V->isConstParam() || V->isConstRefParam()) &&
      (T->isStructured() || isManaged(T) || V->isConstRefParam()))
    return std::string("a '") + (V->isConstRefParam() ? "constref" : "const") +
           "' parameter of " + Of;
  return "";
}


/// A lambda that uses what belongs to the routines around it must not
/// outlive them: reported if Value is one and is stored (Local: in a
/// local variable of the current routine, which is allowed).
void Checker::checkLambdaEscape(Expr *Value, bool Local){
  auto *A = llvm::dyn_cast<AddrOfExpr>(Value);
  RoutineDecl *R = A ? A->getLambda() : nullptr;
  if (!R || !R->getCapture().isValid() || Local || R->isClosure())
    return;
  Diags.error(Value->getBeginLoc(),
              "this lambda uses '" + R->getCapture().Spelling +
                  "' of the routine it is written in, so it must not "
                  "outlive it: it can be passed as an argument or kept in "
                  "a local variable, not stored here",
              {Value->getRange()});
}

