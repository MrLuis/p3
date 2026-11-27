// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- SemaClasses.cpp - Semantic analysis -------------------------------------===//

#include "Checker.h"

using namespace p3;
using namespace p3::detail;


/// class TObject: constructor Create; destructor Destroy; virtual;
/// procedure Free; function ClassName: string; (bodies in codegen and the
/// runtime). Destroy is virtual method 0.
void Checker::declareTObject(){
  TObjectTy = Ctx.create<ClassType>();
  TObjectTy->setDeclName("TObject");
  auto Method = [&](llvm::StringRef Name, MethodKind MK, Type *Result,
                    TObjectMethod B) {
    auto *R = Ctx.create<RoutineDecl>(SourceLocation(), memberName(Name),
                                      Result != nullptr,
                                      llvm::ArrayRef<VarDecl *>(), false,
                                      nullptr, nullptr, false);
    R->setMethodKind(MK);
    R->setClass(TObjectTy);
    R->setBuiltin(B);
    if (Result)
      R->setResultType(Result);
    return R;
  };
  RoutineDecl *Destroy = Method("Destroy", MethodKind::Destructor, nullptr,
                                TObjectMethod::Destroy);
  Destroy->setDirectives(/*Virtual=*/true, false, false);
  Destroy->setVMTIndex(0);
  // TClass = class of TObject; class functions take Self = a class.
  ClassRefType *TClass = classRefOf(TObjectTy);
  auto ClassFn = [&](llvm::StringRef Name, Type *Result, TObjectMethod B) {
    RoutineDecl *R = Method(Name, MethodKind::Method, Result, B);
    R->setClassMethod();
    return R;
  };
  auto *AClass = Ctx.create<VarDecl>(memberName("AClass"), nullptr,
                                     VarDecl::ValueParam);
  AClass->setType(TClass);
  VarDecl *InheritsParams[] = {AClass};
  auto *InheritsFrom = Ctx.create<RoutineDecl>(
      SourceLocation(), memberName("InheritsFrom"), true,
      Ctx.copyArray<VarDecl *>(InheritsParams), true, nullptr, nullptr, false);
  InheritsFrom->setMethodKind(MethodKind::Method);
  InheritsFrom->setClass(TObjectTy);
  InheritsFrom->setBuiltin(TObjectMethod::InheritsFrom);
  InheritsFrom->setResultType(Ctx.getBooleanType());
  InheritsFrom->setClassMethod();
  Decl *Members[] = {
      Method("Create", MethodKind::Constructor, nullptr, TObjectMethod::Create),
      Destroy,
      Method("Free", MethodKind::Method, nullptr, TObjectMethod::Free),
      ClassFn("ClassName", Ctx.getShortStringType(), TObjectMethod::ClassName),
      Method("ClassType", MethodKind::Method, TClass, TObjectMethod::ClassType),
      ClassFn("ClassParent", TClass, TObjectMethod::ClassParent),
      InheritsFrom,
      ClassFn("InstanceSize", Ctx.getInt64Type(), TObjectMethod::InstanceSize)};
  RoutineDecl *VTable[] = {Destroy};
  TObjectTy->define(nullptr, Ctx.copyArray<Decl *>(Members), {},
                    Ctx.copyArray<RoutineDecl *>(VTable), nullptr);
  Standard.insert("tobject", Ctx.create<TypeDecl>(SourceLocation(),
                                                  memberName("TObject"),
                                                  TObjectTy));
  auto *TClassNamed = Ctx.create<ClassRefType>(TObjectTy);
  TClassNamed->setDeclName("TClass");
  Standard.insert("tclass", Ctx.create<TypeDecl>(SourceLocation(),
                                                 memberName("TClass"),
                                                 TClassNamed));
}


/// TVarRec, the elements of an array of const, and the vt* constants:
///   record VType: int64; case of ... VInteger, VBoolean, VChar, ... end
void Checker::declareVarRec(){
  Type *Int64Ty = Ctx.getInt64Type();
  auto Field = [&](llvm::StringRef Name, Type *T) {
    auto *F = Ctx.create<FieldDecl>(memberName(Name), nullptr);
    F->setType(T);
    return F;
  };
  Type *Untyped = Ctx.getUntypedPointerType();
  auto Ptr = [&](Type *T) { return Ctx.create<PointerType>(T); };
  std::pair<llvm::StringRef, Type *> Variants[] = {
      {"VInteger", Ctx.getIntegerType()},
      {"VBoolean", Ctx.getBooleanType()},
      {"VChar", Ctx.getCharType()},
      {"VExtended", Ptr(Ctx.getRealType())},
      {"VString", Ptr(Ctx.getShortStringType())},
      {"VPointer", Untyped},
      {"VPChar", PCharTy},
      {"VAnsiString", Untyped},
      {"VInt64", Ptr(Int64Ty)},
  };
  llvm::SmallVector<std::pair<llvm::StringRef, Type *>, 12> AllVariants(
      std::begin(Variants), std::end(Variants));
  if (LangOpts.ObjKeywords) { // classes exist
    AllVariants.push_back({"VObject", TObjectTy});
    AllVariants.push_back({"VClass", classRefOf(TObjectTy)});
  }
  llvm::SmallVector<FieldDecl *, 12> All;
  FieldDecl *VType = Field("VType", Int64Ty);
  All.push_back(VType);
  llvm::SmallVector<Variant *, 12> Vs;
  for (auto &[Name, T] : AllVariants) {
    FieldDecl *F = Field(Name, T);
    All.push_back(F);
    FieldDecl *One[] = {F};
    auto *FL = Ctx.create<FieldList>();
    FL->Fixed = Ctx.copyArray<FieldDecl *>(One);
    auto *V = Ctx.create<Variant>();
    V->Fields = FL;
    Vs.push_back(V);
  }
  auto *VP = Ctx.create<VariantPart>();
  VP->Variants = Ctx.copyArray<Variant *>(Vs);
  FieldDecl *Fixed[] = {VType};
  auto *FL = Ctx.create<FieldList>();
  FL->Fixed = Ctx.copyArray<FieldDecl *>(Fixed);
  FL->Variant = VP;
  auto *Rec = Ctx.create<RecordType>(FL, Ctx.copyArray<FieldDecl *>(All), false);
  Rec->setDeclName("TVarRec");
  Standard.insert("tvarrec", Ctx.create<TypeDecl>(SourceLocation(),
                                                  memberName("TVarRec"), Rec));
  ArrayOfConstTy = Ctx.create<ArrayOfConstType>(Rec);
  std::pair<llvm::StringRef, int64_t> Kinds[] = {
      {"vtinteger", 0}, {"vtboolean", 1}, {"vtchar", 2}, {"vtextended", 3},
      {"vtstring", 4}, {"vtpointer", 5}, {"vtpchar", 6}, {"vtobject", 7},
      {"vtclass", 8}, {"vtansistring", 11}, {"vtint64", 16},
  };
  for (auto &[Name, V] : Kinds)
    Standard.insert(Name, Ctx.create<ConstDecl>(SourceLocation(),
                                                predeclaredName(Name),
                                                Ctx.getIntegerType(),
                                                ConstValue::ordinal(V)));
}


/// TGUID = packed record case integer of
///   1: (Data1: longword; Data2, Data3: word; Data4: array[0..7] of byte);
///   2: (D1: longword; D2, D3: word; D4: array[0..7] of byte);
///   3: (time_low: longword; time_mid, time_hi_and_version: word;
///       clock_seq_hi_and_reserved, clock_seq_low: byte;
///       node: array[0..5] of byte);
/// end; and PGUID = ^TGUID, as in Free Pascal's system unit (tp, objfpc).
void Checker::declareTGUID(){
  auto Bytes = [&](int64_t Hi) {
    return Ctx.create<ArrayType>(
        Ctx.create<SubrangeType>(Ctx.getIntegerType(), 0, Hi),
        Ctx.getByteType(), false);
  };
  Type *LongWord = Ctx.getCardinalType(), *Word = Ctx.getWordType(),
       *Byte = Ctx.getByteType();
  std::pair<llvm::StringRef, Type *> V1[] = {
      {"Data1", LongWord}, {"Data2", Word}, {"Data3", Word},
      {"Data4", Bytes(7)}};
  std::pair<llvm::StringRef, Type *> V2[] = {
      {"D1", LongWord}, {"D2", Word}, {"D3", Word}, {"D4", Bytes(7)}};
  std::pair<llvm::StringRef, Type *> V3[] = {
      {"time_low", LongWord},
      {"time_mid", Word},
      {"time_hi_and_version", Word},
      {"clock_seq_hi_and_reserved", Byte},
      {"clock_seq_low", Byte},
      {"node", Bytes(5)}};
  llvm::SmallVector<FieldDecl *, 14> All;
  llvm::SmallVector<Variant *, 3> Vs;
  for (llvm::ArrayRef<std::pair<llvm::StringRef, Type *>> Fs :
       {llvm::ArrayRef(V1), llvm::ArrayRef(V2), llvm::ArrayRef(V3)}) {
    llvm::SmallVector<FieldDecl *, 6> Fixed;
    for (auto &[Name, T] : Fs) {
      auto *F = Ctx.create<FieldDecl>(memberName(Name), nullptr);
      F->setType(T);
      Fixed.push_back(F);
      All.push_back(F);
    }
    auto *FL = Ctx.create<FieldList>();
    FL->Fixed = Ctx.copyArray<FieldDecl *>(Fixed);
    auto *V = Ctx.create<Variant>();
    V->Fields = FL;
    Vs.push_back(V);
  }
  auto *VP = Ctx.create<VariantPart>();
  VP->Variants = Ctx.copyArray<Variant *>(Vs);
  auto *FL = Ctx.create<FieldList>();
  FL->Variant = VP;
  auto *Rec = Ctx.create<RecordType>(FL, Ctx.copyArray<FieldDecl *>(All),
                                     /*Packed=*/false);
  Rec->setDeclName("TGUID");
  TGuidTy = Rec;
  Standard.insert("tguid", Ctx.create<TypeDecl>(SourceLocation(),
                                                memberName("TGUID"), Rec));
  auto *PGuid = Ctx.create<PointerType>(Rec);
  PGuid->setDeclName("PGUID");
  Standard.insert("pguid", Ctx.create<TypeDecl>(SourceLocation(),
                                                memberName("PGUID"), PGuid));
}


/// TMethod = record Code, Data: pointer; end (objfpc).
void Checker::declareTMethod(){
  llvm::SmallVector<FieldDecl *, 2> All;
  for (llvm::StringRef Name : {"Code", "Data"}) {
    auto *F = Ctx.create<FieldDecl>(memberName(Name), nullptr);
    F->setType(Ctx.getUntypedPointerType());
    All.push_back(F);
  }
  auto *FL = Ctx.create<FieldList>();
  FL->Fixed = Ctx.copyArray<FieldDecl *>(All);
  auto *Rec = Ctx.create<RecordType>(FL, FL->Fixed, false);
  Rec->setDeclName("TMethod");
  Standard.insert("tmethod", Ctx.create<TypeDecl>(SourceLocation(),
                                                  memberName("TMethod"), Rec));
  TMethodTy = Rec;
}


/// class Exception (Free Pascal's SysUtils): private FMessage: string;
/// constructor Create(const msg: string); property Message: string read
/// FMessage write FMessage. Its VMT is in the runtime, which prints the
/// message of an unhandled exception.
void Checker::declareException(){
  ExceptionTy = Ctx.create<ClassType>();
  ExceptionTy->setDeclName("Exception");
  auto *Msg = Ctx.create<FieldDecl>(memberName("FMessage"), nullptr);
  Msg->setType(Ctx.getShortStringType());
  Msg->setVisibility(Visibility::Private);
  auto *Param = Ctx.create<VarDecl>(memberName("msg"), nullptr,
                                    VarDecl::ConstParam);
  Param->setType(Ctx.getShortStringType());
  VarDecl *Params[] = {Param};
  auto *Create = Ctx.create<RoutineDecl>(
      SourceLocation(), memberName("Create"), false,
      Ctx.copyArray<VarDecl *>(Params), true, nullptr, nullptr, false);
  Create->setMethodKind(MethodKind::Constructor);
  Create->setClass(ExceptionTy);
  Create->setBuiltin(TObjectMethod::ExceptionCreate);
  auto *Message = Ctx.create<PropertyDecl>(memberName("Message"), nullptr,
                                           memberName("FMessage"),
                                           memberName("FMessage"));
  Message->setType(Ctx.getShortStringType());
  Message->setAccessors(Msg, Msg);
  auto *FmtMsg = Ctx.create<VarDecl>(memberName("msg"), nullptr,
                                     VarDecl::ConstParam);
  FmtMsg->setType(Ctx.getShortStringType());
  auto *FmtArgs = Ctx.create<VarDecl>(memberName("args"), nullptr,
                                      VarDecl::ConstParam);
  FmtArgs->setType(ArrayOfConstTy);
  VarDecl *FmtParams[] = {FmtMsg, FmtArgs};
  auto *CreateFmt = Ctx.create<RoutineDecl>(
      SourceLocation(), memberName("CreateFmt"), false,
      Ctx.copyArray<VarDecl *>(FmtParams), true, nullptr, nullptr, false);
  CreateFmt->setMethodKind(MethodKind::Constructor);
  CreateFmt->setClass(ExceptionTy);
  CreateFmt->setBuiltin(TObjectMethod::ExceptionCreateFmt);
  Decl *Members[] = {Msg, Create, CreateFmt, Message};
  llvm::SmallVector<FieldDecl *, 2> Fields(TObjectTy->getAllFields());
  Fields.push_back(Msg);
  ExceptionTy->define(TObjectTy, Ctx.copyArray<Decl *>(Members),
                      Ctx.copyArray<FieldDecl *>(Fields),
                      TObjectTy->getVTable(), nullptr);
  Standard.insert("exception", Ctx.create<TypeDecl>(SourceLocation(),
                                                    memberName("Exception"),
                                                    ExceptionTy));
  // The exception for integer div and mod by zero, as in SysUtils.
  ClassType *External = declareExceptionClass("EExternal", ExceptionTy);
  ClassType *IntError = declareExceptionClass("EIntError", External);
  declareExceptionClass("EDivByZero", IntError);
  declareExceptionClass("EIntOverflow", IntError); // (objfpc overflow)
  // Tasks: a cancelled task's wait raises EOperationCancelled; tokens.
  declareExceptionClass("EOperationCancelled", ExceptionTy);
  Standard.insert("tcanceltoken",
                  Ctx.create<TypeDecl>(SourceLocation(), memberName("TCancelToken"),
                                       Ctx.getTokenType()));
  declareExceptionClass("EConvertError", ExceptionTy);
}


/// IUnknown = interface end (COM; also named IInterface: Free Pascal's
/// QueryInterface, _AddRef and _Release are not declared, the compiled
/// code counts references itself) and
///   TInterfacedObject = class(TObject, IUnknown)
///   protected FRefCount: longint;
///   public property RefCount: longint read FRefCount; end;
/// Their iid and VMT are in the runtime.
void Checker::declareInterfacedObject(){
  IUnknownTy = Ctx.create<InterfaceType>();
  IUnknownTy->setDeclName("IUnknown");
  IUnknownTy->setCOM(true);
  IUnknownTy->define(nullptr, {}, {}, nullptr);
  Standard.insert("iunknown", Ctx.create<TypeDecl>(SourceLocation(),
                                                   memberName("IUnknown"),
                                                   IUnknownTy));
  Standard.insert("iinterface", Ctx.create<TypeDecl>(SourceLocation(),
                                                     memberName("IInterface"),
                                                     IUnknownTy));

  TInterfacedObjectTy = Ctx.create<ClassType>();
  TInterfacedObjectTy->setDeclName("TInterfacedObject");
  auto *Count = Ctx.create<FieldDecl>(memberName("FRefCount"), nullptr);
  Count->setType(Ctx.getIntegerType());
  Count->setVisibility(Visibility::Protected);
  auto *RefCount = Ctx.create<PropertyDecl>(memberName("RefCount"), nullptr,
                                            memberName("FRefCount"),
                                            Identifier());
  RefCount->setType(Ctx.getIntegerType());
  RefCount->setAccessors(Count, nullptr);
  Decl *Members[] = {Count, RefCount};
  FieldDecl *Fields[] = {Count};
  TInterfacedObjectTy->define(TObjectTy, Ctx.copyArray<Decl *>(Members),
                              Ctx.copyArray<FieldDecl *>(Fields),
                              TObjectTy->getVTable(), nullptr);
  InterfaceImpl Impls[] = {{IUnknownTy, {}}};
  TInterfacedObjectTy->setInterfaces(Ctx.copyArray<InterfaceImpl>(Impls));
  Standard.insert("tinterfacedobject",
                  Ctx.create<TypeDecl>(SourceLocation(),
                                       memberName("TInterfacedObject"),
                                       TInterfacedObjectTy));
}


/// TSharedObject = class(TObject) end: a class derived from it says it is
/// thread-safe (its methods synchronize what they share), so `shared`
/// variables can hold its objects. Its VMT is in the runtime.
void Checker::declareSharedObject(){
  TSharedObjectTy = Ctx.create<ClassType>();
  TSharedObjectTy->setDeclName("TSharedObject");
  TSharedObjectTy->define(TObjectTy, {}, TObjectTy->getAllFields(),
                          TObjectTy->getVTable(), nullptr);
  Identifier Id = memberName("TSharedObject");
  Standard.insert(Id.Name, Ctx.create<TypeDecl>(SourceLocation(), Id,
                                                TSharedObjectTy));
}


/// A predeclared `Name = class(Parent);` (its VMT is in the runtime).
ClassType *Checker::declareExceptionClass(llvm::StringRef Name, ClassType *Parent){
  auto *C = Ctx.create<ClassType>();
  C->setDeclName(Name);
  C->define(Parent, {}, Parent->getAllFields(), Parent->getVTable(), nullptr);
  Identifier Id = memberName(Name);
  Standard.insert(Id.Name, Ctx.create<TypeDecl>(SourceLocation(), Id, C));
  return C;
}


/// The name of the type declared as Id: TOuter.TInner for a nested one.
llvm::StringRef Checker::typeDeclName(const Identifier &Id){
  if (NestPrefix.empty())
    return Id.Spelling;
  return Ctx.getStrings().intern((NestPrefix + "." + Id.Spelling).str());
}


//===--------------------------------------------------------------------===//
// Type helpers (objfpc, {$modeswitch typehelpers})
//===--------------------------------------------------------------------===//

/// The scope key of the helper active for T.
llvm::StringRef Checker::helperKey(Type *T){
  return Ctx.getStrings().intern(
      "#helper:" + llvm::utohexstr(reinterpret_cast<uintptr_t>(T)));
}


/// The type helper active for values of type T (the last declared in
/// scope; a string constant: the ansistring or shortstring one), or null.
ClassType *Checker::helperFor(Type *T){
  if (!T || T->isError() || !LangOpts.ObjKeywords)
    return nullptr;
  if (T->isStringLiteral()) {
    if (ClassType *H = helperFor(Ctx.getAnsiStringType()))
      return H;
    return helperFor(Ctx.getShortStringType());
  }
  auto *TD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(helperKey(T)));
  if (TD)
    return llvm::dyn_cast_or_null<ClassType>(TD->getType());
  return genericHelperFor(T);
}


/// The scope key of the last generic type helper declared (see
/// TypeDecl::getPrevGenericHelper).
llvm::StringRef Checker::genericHelpersKey(){
  return Ctx.getStrings().intern("#generichelpers");
}


/// The generic type helpers in scope, the last declared first, whose
/// extended type T matches (generic TH<T> = type helper for array of T:
/// any dynamic array): the first one's specialization for the type
/// arguments the match gives, or null.
ClassType *Checker::genericHelperFor(Type *T){
  if (T->isClass() || T->isClassRef() || T->isInterface())
    return nullptr;
  for (auto *G = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(genericHelpersKey()));
       G; G = G->getPrevGenericHelper()) {
    auto *HS = llvm::cast<HelperTypeSpec>(G->getTypeSpec());
    llvm::SmallVector<Type *, 2> Bound(G->getTypeParams().size(), nullptr);
    if (!matchTypePattern(G, HS->getExtended(), T, Bound))
      continue;
    if (llvm::is_contained(Bound, nullptr))
      continue;
    TypeDecl *I = specializeTypeFor(G->getName(), G, Bound);
    return I ? llvm::dyn_cast_or_null<ClassType>(I->getType()) : nullptr;
  }
  return nullptr;
}


/// Does type T match TS, a type written with the type parameters of
/// generic G (a parameter, `array of P`, `specialize TGen<P...>`, or a
/// type without them)? The types the parameters stand for go to Bound.
bool Checker::matchTypePattern(TypeDecl *G, TypeSpec *TS, Type *T,
                      llvm::SmallVectorImpl<Type *> &Bound){
  auto Params = G->getTypeParams();
  if (auto *N = llvm::dyn_cast<NamedTypeSpec>(TS); N && N->getQualifiers().empty())
    for (size_t K = 0; K < Params.size(); ++K)
      if (Params[K].Name == N->getName().Name) {
        if (Bound[K])
          return Bound[K] == T;
        Bound[K] = T;
        return true;
      }
  if (auto *DS = llvm::dyn_cast<DynArrayTypeSpec>(TS)) {
    auto *DT = llvm::dyn_cast<DynArrayType>(T);
    return DT && matchTypePattern(G, DS->getElement(), DT->getElementType(), Bound);
  }
  if (auto *AS = llvm::dyn_cast<AsyncTypeSpec>(TS); AS && AS->isIterator()) {
    auto *IT = llvm::dyn_cast<IteratorType>(T);
    return IT && matchTypePattern(G, AS->getInner(), IT->getElementType(), Bound);
  }
  if (auto *SS = llvm::dyn_cast<SpecializeTypeSpec>(TS)) {
    auto *GS = static_cast<Scope *>(G->getGenericScope());
    auto *Of = llvm::dyn_cast_or_null<TypeDecl>(
        GS->lookup(genericKey(SS->getName().Name, SS->getArgs().size())));
    if (!Of)
      return false;
    for (auto &[Key, I] : TypeInstances)
      if (Key.first == Of && I->getType() == T) {
        for (size_t K = 0; K < SS->getArgs().size(); ++K)
          if (!matchTypePattern(G, SS->getArgs()[K], Key.second[K], Bound))
            return false;
        return true;
      }
    return false;
  }
  // a type without the parameters: that type
  if (!llvm::isa<NamedTypeSpec>(TS))
    return false;
  Scope *Saved = Cur;
  Cur = static_cast<Scope *>(G->getGenericScope());
  Type *Named = resolveType(TS);
  Cur = Saved;
  return !Named->isError() && Named == T;
}


/// The helper of the type Base names (AnsiString.Join), or null.
ClassType *Checker::helperNamedBy(Expr *Base){
  TypeDecl *TD = LangOpts.ObjKeywords ? typeNamedBy(Base) : nullptr;
  Type *T = TD ? TD->getType() : nullptr;
  if (!T || T->isClass() || memberTableOf(T))
    return nullptr;
  ClassType *H = helperFor(T);
  if (!H)
    return nullptr;
  if (auto *N = llvm::dyn_cast<NameRefExpr>(Base))
    N->setDecl(TD);
  Base->setType(T);
  return H;
}


/// The type helper D is a member of (a property: by its getter), or null.
ClassType *Checker::helperOfMember(Decl *D){
  if (auto *P = llvm::dyn_cast_or_null<PropertyDecl>(D))
    D = P->getRead();
  auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D);
  return R && R->getClass() && R->getClass()->isHelper() ? R->getClass()
                                                         : nullptr;
}


/// TH = type helper [(Parent)] for T ... end: a member table (a ClassType
/// that is not a type of values) whose methods' Self is a T, by
/// reference; from here on the helper active for T. A record helper is
/// the same, for a record type T.
void Checker::declareHelper(TypeDecl *TD, HelperTypeSpec *HS){
  const Identifier &Id = TD->getName();
  auto *C = Ctx.create<ClassType>();
  C->setDeclName(typeDeclName(Id));
  C->setModule(CurModule);
  if (HS->isRecordHelper())
    C->setRecordHelper();
  llvm::StringRef Kind = C->helperKindName();
  TD->setType(C);
  declare(TD);
  auto Empty = [&] { // (its methods' bodies are not checked)
    C->define(nullptr, {}, {}, {}, CurModule);
    TD->setType(errorType());
  };
  if (CurRoutine) {
    Diags.error(Id.Loc,
                Kind + " '" + Id.Spelling +
                    "' must be declared at program or unit level",
                {Id.getRange()});
    return Empty();
  }
  Type *T = resolveType(HS->getExtended());
  if (T->isError())
    return Empty();
  if (T->isClass() || T->isClassRef() || T->isInterface()) {
    Diags.error(HS->getExtended()->getBeginLoc(),
                "type helpers for classes and interfaces are not supported "
                "yet",
                {HS->getExtended()->getRange()});
    return Empty();
  }
  if (HS->isRecordHelper() && !T->isRecord()) {
    Diags.error(HS->getExtended()->getBeginLoc(),
                "a record helper can only extend a record type, not " +
                    quoted(T) + " (use a type helper)",
                {HS->getExtended()->getRange()});
    return Empty();
  }
  C->setHelperFor(T);
  ClassType *Parent = nullptr;
  if (const Identifier &P = HS->getParent(); P.isValid()) {
    auto *PD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(P.Name));
    auto *PC = PD ? llvm::dyn_cast_or_null<ClassType>(PD->getType()) : nullptr;
    if (!PC || !PC->isHelper()) // (a record helper can extend a type
                                // helper, and the reverse)
      Diags.error(P.Loc,
                  "'" + P.Spelling + "' is not a " +
                      (C->isRecordHelper() ? "helper" : "type helper"),
                  {P.getRange()});
    else if (PC->getHelperFor() != T)
      Diags.error(P.Loc,
                  "'" + P.Spelling + "' is a helper for " +
                      quoted(PC->getHelperFor()) + ", not for " + quoted(T),
                  {P.getRange()});
    else
      Parent = PC;
  }
  Scope MemberScope(Cur);
  Scope *SavedScope = Cur;
  Cur = &MemberScope;
  llvm::SmallVector<Decl *, 16> Members;
  llvm::StringMap<Decl *> Own;
  for (Decl *M : HS->getMembers()) {
    const Identifier &MN = M->getName();
    if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && R->isGenericMethod()) {
      if (Decl *Prev = Own.lookup(R->getMemberKey())) {
        Diags.error(MN.Loc,
                    "redefinition of generic method '" + MN.Spelling + "'",
                    {MN.getRange()});
        Diags.note(Prev->getLocation(), "previous definition is here");
        continue;
      }
      defineGenericMethod(C, R);
      if (R->isClassMethod())
        R->setStatic(); // no Self
      Own[R->getMemberKey()] = R;
      Members.push_back(R);
      continue;
    }
    RoutineDecl *OverloadOf = nullptr;
    if (Decl *Prev = Own.lookup(MN.Name)) {
      OverloadOf = llvm::dyn_cast<RoutineDecl>(Prev);
      if (!OverloadOf || !llvm::isa<RoutineDecl>(M)) {
        Diags.error(MN.Loc, "redefinition of '" + MN.Spelling + "'",
                    {MN.getRange()});
        Diags.note(Prev->getLocation(), "previous definition is here");
        continue;
      }
    } else {
      Own[MN.Name] = M;
    }
    Members.push_back(M);
    if (auto *CD = llvm::dyn_cast<ConstDecl>(M)) {
      declareConst(CD);
      continue;
    }
    if (auto *V = llvm::dyn_cast<VarDecl>(M)) { // class var
      declareClassVar(C, V);
      continue;
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(M)) {
      R->setClass(C);
      R->setParent(nullptr);
      resolveSignature(R);
      if (R->isVirtual() || R->isOverride() || R->isAbstract())
        Diags.error(MN.Loc,
                    "the methods of a " + Kind +
                        " cannot be 'virtual', 'override' or 'abstract'",
                    {MN.getRange()});
      if (R->isClassMethod())
        R->setStatic(); // no Self
      else
        makeSelf(R, C);
      if (OverloadOf && !addOverload(OverloadOf, R)) {
        R->setClass(nullptr); // rejected: not a member
        Members.pop_back();
      }
      continue;
    }
    auto *P = llvm::cast<PropertyDecl>(M);
    if (P->isDefault())
      Diags.error(P->getLocation(),
                  "a " + Kind + " cannot have a default property",
                  {P->getName().getRange()});
    definePropertyAccessors(C, P, Own, Parent);
  }
  Cur = SavedScope;
  C->define(Parent, Ctx.copyArray<Decl *>(Members), {}, {}, CurModule);
  Cur->set(helperKey(T), TD);
}


//===--------------------------------------------------------------------===//
// Advanced records (objfpc, {$modeswitch advancedrecords})
//===--------------------------------------------------------------------===//

/// TR = record fields; methods; operators; properties end; -- the record
/// type (its fields), then its member table (see RecordType).
void Checker::declareAdvancedRecord(TypeDecl *TD, RecordTypeSpec *RS){
  // Its nested constants and types first: its fields may use them.
  llvm::StringRef Name = typeDeclName(TD->getName());
  Scope MemberScope(Cur);
  Scope *SavedScope = Cur;
  llvm::StringRef SavedPrefix = NestPrefix;
  Cur = &MemberScope;
  NestPrefix = Name;
  for (Decl *M : RS->getMembers()) {
    if (auto *CD = llvm::dyn_cast<ConstDecl>(M))
      declareConst(CD);
    else if (auto *NT = llvm::dyn_cast<TypeDecl>(M))
      declareNestedType(NT);
  }
  RecordTypeSpec *Saved = NamedAdvancedRecord;
  NamedAdvancedRecord = RS;
  Type *T = resolveType(RS, Name);
  NamedAdvancedRecord = Saved;
  Cur = SavedScope;
  TD->setType(T);
  declare(TD); // its methods' headings may name it
  Cur = &MemberScope;
  if (auto *RT = llvm::dyn_cast<RecordType>(T))
    defineRecordMembers(RT, RS);
  Cur = SavedScope;
  NestPrefix = SavedPrefix;
}


bool Checker::isForwardClassDecl(Decl *D){
  auto *TD = llvm::dyn_cast_or_null<TypeDecl>(D);
  auto *CS = TD ? llvm::dyn_cast_or_null<ClassTypeSpec>(TD->getTypeSpec())
                : nullptr;
  return CS && CS->isForward();
}


/// A type declared in a class or advanced record (in Cur, its member
/// scope).
void Checker::declareNestedType(TypeDecl *TD){
  if (auto *HS = llvm::dyn_cast<HelperTypeSpec>(TD->getTypeSpec())) {
    Diags.error(TD->getName().Loc,
                llvm::Twine(HS->isRecordHelper() ? "record" : "type") +
                    " helpers cannot be declared in a class or record",
                {TD->getName().getRange()});
    TD->setType(errorType());
    declare(TD);
    return;
  }
  if (llvm::isa<InterfaceTypeSpec>(TD->getTypeSpec())) {
    Diags.error(TD->getName().Loc,
                "interfaces cannot be declared in a class or record yet",
                {TD->getName().getRange()});
    TD->setType(errorType());
    declare(TD);
    return;
  }
  declareType(TD);
}


/// Members of Outer declared with TD: a nested class or record knows its
/// outer one; an enumeration's values are members too.
void Checker::addNestedMembers(ClassType *Outer, TypeDecl *TD,
                      llvm::SmallVectorImpl<Decl *> &Members,
                      llvm::StringMap<Decl *> &Own){
  Type *T = TD->getType();
  if (auto *NC = llvm::dyn_cast_or_null<ClassType>(T))
    NC->setOuter(Outer);
  else if (ClassType *MT = memberTableOf(T))
    MT->setOuter(Outer);
  auto *E = llvm::dyn_cast_or_null<EnumType>(T);
  auto *ES = llvm::dyn_cast<EnumTypeSpec>(TD->getTypeSpec());
  if (!E || !ES || ES->isScoped()) // (a scoped one's: TOuter.TEnum.V)
    return;
  for (ConstDecl *V : E->getValues()) {
    V->setVisibility(TD->getVisibility());
    if (Own.try_emplace(V->getName().Name, V).second)
      Members.push_back(V);
  }
}


/// The member Name of the class or advanced record that Quals name
/// (TOuter, TOuter.TInner, ...); null after a diagnostic.
Decl *Checker::lookupMemberPath(llvm::ArrayRef<Identifier> Quals,
                       const Identifier &Name){
  ClassType *C = nullptr;
  for (size_t I = 0; I <= Quals.size(); ++I) {
    const Identifier &Id = I < Quals.size() ? Quals[I] : Name;
    Decl *D;
    if (!C) {
      D = Cur->lookup(Id.Name);
      if (!D) {
        diagUndeclared(Id);
        return nullptr;
      }
      Used.insert(D);
    } else {
      const ClassType *Owner = nullptr;
      D = C->findMember(Id.Name, &Owner);
      if (!D) {
        Diags.error(Id.Loc,
                    "no member named '" + Id.Spelling + "' in " +
                        (C->getRecord() ? "record " : "class ") +
                        quoted(C) + didYouMeanMember(C, Id.Name),
                    {Id.getRange()});
        return nullptr;
      }
      checkMemberAccess(D, Owner, Id);
    }
    if (I == Quals.size())
      return D;
    auto *TD = llvm::dyn_cast<TypeDecl>(D);
    Type *T = TD ? TD->getType() : nullptr;
    C = llvm::dyn_cast_or_null<ClassType>(T);
    if (!C)
      C = memberTableOf(T);
    if (!C) {
      if (!T || !T->isError())
        Diags.error(Id.Loc,
                    "'" + Id.Spelling +
                        "' is not a class or record with members",
                    {Id.getRange()});
      return nullptr;
    }
  }
  return nullptr;
}


void Checker::defineRecordMembers(RecordType *RT, RecordTypeSpec *RS){
  auto *C = Ctx.create<ClassType>();
  C->setDeclName(RT->getDeclName());
  C->setRecord(RT);
  RT->setMemberTable(C);
  llvm::SmallVector<Decl *, 16> Members;
  llvm::StringMap<Decl *> Own;
  for (FieldDecl *F : RT->getAllFields()) {
    Members.push_back(F);
    Own.try_emplace(F->getName().Name, F);
  }
  for (Decl *M : RS->getMembers()) {
    const Identifier &MN = M->getName();
    if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && R->isClassInit()) {
      defineClassInit(C, R); // not a member
      continue;
    }
    if (llvm::isa<ConstDecl>(M) || llvm::isa<TypeDecl>(M)) {
      // (declared with the record: see declareAdvancedRecord)
      if (Decl *Prev = Own.lookup(MN.Name)) {
        Diags.error(MN.Loc, "redefinition of '" + MN.Spelling + "'",
                    {MN.getRange()});
        Diags.note(Prev->getLocation(), "previous definition is here");
        continue;
      }
      Own[MN.Name] = M;
      Members.push_back(M);
      if (auto *NT = llvm::dyn_cast<TypeDecl>(M))
        addNestedMembers(C, NT, Members, Own);
      continue;
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && R->isGenericMethod()) {
      if (Decl *Prev = Own.lookup(R->getMemberKey())) {
        Diags.error(MN.Loc,
                    "redefinition of generic method '" + MN.Spelling + "'",
                    {MN.getRange()});
        Diags.note(Prev->getLocation(), "previous definition is here");
        continue;
      }
      defineGenericMethod(C, R);
      if (R->isClassMethod())
        R->setStatic(); // no Self
      Own[R->getMemberKey()] = R;
      Members.push_back(R);
      continue;
    }
    RoutineDecl *OverloadOf = nullptr;
    if (Decl *Prev = Own.lookup(MN.Name)) {
      OverloadOf = llvm::dyn_cast<RoutineDecl>(Prev);
      if (!OverloadOf || !llvm::isa<RoutineDecl>(M)) {
        Diags.error(MN.Loc, "redefinition of '" + MN.Spelling + "'",
                    {MN.getRange()});
        Diags.note(Prev->getLocation(), "previous definition is here");
        continue;
      }
    } else {
      Own[MN.Name] = M;
    }
    Members.push_back(M);
    if (auto *V = llvm::dyn_cast<VarDecl>(M)) { // class var
      declareClassVar(C, V);
      continue;
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(M)) {
      defineRecordMethod(C, RT, R);
      if (OverloadOf && !addOverload(OverloadOf, R)) {
        R->setClass(nullptr); // rejected: not a member
        Members.pop_back();
      }
      continue;
    }
    definePropertyAccessors(C, llvm::cast<PropertyDecl>(M), Own, nullptr);
  }
  C->define(nullptr, Ctx.copyArray<Decl *>(Members), RT->getAllFields(), {},
            CurModule);
}


/// A class variable of class, advanced record or helper C (class var):
/// static storage, one for C and its descendants.
void Checker::declareClassVar(ClassType *C, VarDecl *V){
  const Identifier &MN = V->getName();
  V->setType(resolveType(V->getTypeSpec()));
  V->setVarClass(C);
  if (containsFile(V->getType()))
    Diags.error(MN.Loc, "a class variable cannot be a file", {MN.getRange()});
  if (Expr *Init = V->getInit()) { // a typed constant
    Used.insert(V);
    ConstValue Folded;
    ConstInitWhat = "typed constant value";
    if (!V->getType()->isError() &&
        checkConstInit(Init, V->getType(), Folded))
      V->setInitValue(Folded);
  }
}


/// A method, class method (static) or class operator of record RT.
void Checker::defineRecordMethod(ClassType *C, RecordType *RT, RoutineDecl *R){
  const Identifier &Id = R->getName();
  R->setClass(C);
  R->setParent(nullptr);
  resolveSignature(R);
  if (R->isVirtual() || R->isOverride() || R->isAbstract())
    Diags.error(Id.Loc,
                "the methods of a record cannot be 'virtual', 'override' "
                "or 'abstract'",
                {Id.getRange()});
  if (R->isClassMethod())
    R->setStatic(); // no Self
  else
    makeSelf(R, C);
  if (R->isConstructor() && R->getParams().empty())
    Diags.error(Id.Loc, "a record's constructor needs parameters",
                {Id.getRange()});
  if (R->isOperator())
    checkOperatorHeading(R, RT);
}


/// A class operator's parameters: two (binary), one (unary - + not, and
/// the conversions := and Explicit); no var parameters; the record among
/// the parameter or result types.
void Checker::checkOperatorHeading(RoutineDecl *R, RecordType *RT){
  const Identifier &Id = R->getName();
  llvm::StringRef Sym = Id.Name.drop_front(strlen("operator"));
  size_t N = R->getParams().size();
  bool Unary = Sym == "not" || Sym == ":=" || Sym == "explicit";
  bool Either = Sym == "-" || Sym == "+";
  if (Unary ? N != 1 : Either ? N != 1 && N != 2 : N != 2) {
    Diags.error(Id.Loc,
                "'" + Id.Spelling + "' takes " +
                    (Unary ? "one parameter"
                     : Either ? "one or two parameters"
                              : "two parameters"),
                {Id.getRange()});
    return;
  }
  bool HasRecord = R->getResultType() == RT;
  for (VarDecl *P : R->getParams()) {
    if (P->isVarParam())
      Diags.error(P->getLocation(),
                  "the parameters of an operator cannot be 'var'",
                  {P->getName().getRange()});
    HasRecord |= P->getType() == RT;
  }
  if (!HasRecord)
    Diags.error(Id.Loc,
                "an operator of " + quoted(RT) +
                    " needs a parameter or result of that type",
                {Id.getRange()});
}


ClassType *Checker::memberTableOf(Type *T){
  auto *RT = llvm::dyn_cast_or_null<RecordType>(T);
  return RT ? RT->getMemberTable() : nullptr;
}


/// A user-defined conversion of a value of type From to To: a `:=`
/// operator (Explicit: an `Explicit` one first) of either record whose
/// parameter takes From (exactly, else by assignment) and whose result is
/// To. Null if there is none.
RoutineDecl *Checker::findConversion(Type *From, Type *To, bool Explicit){
  if (!LangOpts.ObjKeywords || !From || !To)
    return nullptr;
  for (llvm::StringRef Name :
       {llvm::StringRef("operatorexplicit"), llvm::StringRef("operator:=")}) {
    if (Name == "operatorexplicit" && !Explicit)
      continue;
    RoutineDecl *Best = nullptr;
    bool BestExact = false;
    for (Type *T : {From, To}) {
      ClassType *MT = memberTableOf(T);
      if (!MT || (T == To && To == From))
        continue;
      for (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(MT->findOwnMember(Name));
           R; R = R->getNextOverload()) {
        Type *PT = R->getParams().size() == 1 ? R->getParams()[0]->getType()
                                              : nullptr;
        if (!R->isOperator() || !PT || R->getResultType() != To)
          continue;
        bool Exact = PT == From;
        if (!Exact && !isAssignable(PT, From))
          continue;
        if (!Best || (Exact && !BestExact)) {
          Best = R;
          BestExact = Exact;
        }
      }
    }
    if (Best)
      return Best;
  }
  return nullptr;
}


/// Converts Value to To by a user-defined `:=` operator if there is one.
bool Checker::convertImplicitly(Expr *Value, Type *To){
  RoutineDecl *Op = findConversion(Value->getType(), To, false);
  if (!Op)
    return false;
  Value->setConversion(Op, To);
  return true;
}


/// The class operator Name of the records among Args' types that fits
/// Args best (exact types, then assignable ones, then ones converted by a
/// `:=` operator), with the conversions it needs annotated; null if none
/// fits. Found: whether either record has an operator Name of that arity.
RoutineDecl *Checker::resolveOperator(llvm::StringRef Name, llvm::ArrayRef<Expr *> Args,
                             bool &Found){
  llvm::SmallVector<ClassType *, 2> Tables;
  for (Expr *A : Args)
    if (ClassType *MT = memberTableOf(A->getType()))
      if (!llvm::is_contained(Tables, MT))
        Tables.push_back(MT);
  RoutineDecl *Best = nullptr;
  int BestCost = 0;
  for (ClassType *MT : Tables)
    for (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(MT->findOwnMember(Name));
         R; R = R->getNextOverload()) {
      if (!R->isOperator() || R->getParams().size() != Args.size())
        continue;
      Found = true;
      int Cost = 0;
      bool OK = true;
      for (size_t I = 0; I < Args.size() && OK; ++I) {
        Type *PT = R->getParams()[I]->getType(), *AT = Args[I]->getType();
        if (!PT || PT->isError())
          OK = false;
        else if (PT == AT)
          continue;
        else if (isAssignable(PT, AT))
          Cost += 1;
        else if (findConversion(AT, PT, false))
          Cost += 100;
        else
          OK = false;
      }
      if (OK && (!Best || Cost < BestCost)) {
        Best = R;
        BestCost = Cost;
      }
    }
  if (Best)
    for (size_t I = 0; I < Args.size(); ++I) {
      Type *PT = Best->getParams()[I]->getType(), *AT = Args[I]->getType();
      if (PT != AT && !isAssignable(PT, AT))
        convertImplicitly(Args[I], PT);
    }
  return Best;
}


const char *Checker::operatorSymbol(BinaryOp Op){
  switch (Op) {
  case BinaryOp::Add:     return "+";
  case BinaryOp::Sub:     return "-";
  case BinaryOp::Mul:     return "*";
  case BinaryOp::RealDiv: return "/";
  case BinaryOp::IntDiv:  return "div";
  case BinaryOp::Mod:     return "mod";
  case BinaryOp::And:     return "and";
  case BinaryOp::Or:      return "or";
  case BinaryOp::Xor:     return "xor";
  case BinaryOp::Shl:     return "shl";
  case BinaryOp::Shr:     return "shr";
  case BinaryOp::Eq:      return "=";
  case BinaryOp::Ne:      return "<>";
  case BinaryOp::Lt:      return "<";
  case BinaryOp::Gt:      return ">";
  case BinaryOp::Le:      return "<=";
  case BinaryOp::Ge:      return ">=";
  case BinaryOp::In:      return "in";
  default:                return nullptr;
  }
}


/// L op R where an operand is an advanced record: its class operator
/// (`<>` may use `=`, negated).
Type *Checker::checkRecordOperator(BinaryExpr *E){
  const char *Sym = operatorSymbol(E->getOp());
  Expr *Args[] = {E->getLHS(), E->getRHS()};
  Type *L = Args[0]->getType(), *R = Args[1]->getType();
  bool Found = false;
  RoutineDecl *Op =
      Sym ? resolveOperator(std::string("operator") + Sym, Args, Found)
          : nullptr;
  bool Negate = false;
  if (!Op && E->getOp() == BinaryOp::Ne) {
    Op = resolveOperator("operator=", Args, Found);
    Negate = Op != nullptr;
  }
  if (!Op) {
    Diags.error(E->getOpLoc(),
                llvm::Twine("operator '") + getOpSpelling(E->getOp()) +
                    "' is not overloaded for " + quoted(L) + " and " +
                    quoted(R),
                {Args[0]->getRange(), Args[1]->getRange()});
    return errorType();
  }
  E->setOperator(Op, Negate);
  if (Negate)
    return Ctx.getBooleanType();
  return Op->getResultType();
}


/// op X where X is an advanced record: its class operator.
Type *Checker::checkRecordOperator(UnaryExpr *E){
  const char *Sym = E->getOp() == UnaryOp::Not     ? "not"
                    : E->getOp() == UnaryOp::Minus ? "-"
                                                   : "+";
  Expr *Args[] = {E->getOperand()};
  bool Found = false;
  RoutineDecl *Op = resolveOperator(std::string("operator") + Sym, Args, Found);
  if (!Op) {
    Diags.error(E->getOpLoc(),
                llvm::Twine("operator '") + getOpSpelling(E->getOp()) +
                    "' is not overloaded for " + quoted(Args[0]->getType()),
                {Args[0]->getRange()});
    return errorType();
  }
  E->setOperator(Op);
  return Op->getResultType();
}


//===--------------------------------------------------------------------===//
// Classes (objfpc)
//===--------------------------------------------------------------------===//

std::string Checker::qualified(ClassType *C, const Identifier &Member){
  return C->getName() + "." + Member.Spelling.str();
}


/// TFoo = class ... end;  /  TFoo = class;
void Checker::declareClass(TypeDecl *TD, ClassTypeSpec *CS){
  const Identifier &Id = TD->getName();
  if (CurRoutine) {
    Diags.error(Id.Loc,
                "class '" + Id.Spelling +
                    "' must be declared at program or unit level",
                {Id.getRange()});
    TD->setType(errorType());
    CS->setResolved(errorType());
    declare(TD);
    return;
  }
  // The definition of a class declared forward in this type section?
  if (!CS->isForward())
    for (auto It = PendingClasses.begin(); It != PendingClasses.end(); ++It)
      if (It->first->getName().Name == Id.Name &&
          Cur->lookupOwn(Id.Name) == It->first) {
        ClassType *C = It->second;
        PendingClasses.erase(It);
        TD->setType(C);
        CS->setResolved(C);
        defineClass(C, CS);
        return;
      }
  auto *C = Ctx.create<ClassType>();
  C->setDeclName(typeDeclName(Id));
  C->setModule(CurModule);
  TD->setType(C);
  CS->setResolved(C);
  declare(TD); // before the members, which may refer to the class
  if (CS->isForward()) {
    PendingClasses.push_back({TD, C});
    return;
  }
  defineClass(C, CS);
}


/// A forward class must be defined in the same type section.
void Checker::checkPendingClasses(){
  for (auto &[TD, C] : PendingClasses) {
    Diags.error(TD->getName().Loc,
                "class '" + TD->getName().Spelling +
                    "' is declared forward but never defined",
                {TD->getName().getRange()});
    C->define(TObjectTy, {}, TObjectTy->getAllFields(),
              TObjectTy->getVTable(), CurModule);
  }
  PendingClasses.clear();
  for (auto &[TD, I] : PendingInterfaces) {
    Diags.error(TD->getName().Loc,
                "interface '" + TD->getName().Spelling +
                    "' is declared forward but never defined",
                {TD->getName().getRange()});
    I->define(nullptr, {}, {}, CurModule);
  }
  PendingInterfaces.clear();
}


//===--------------------------------------------------------------------===//
// Interfaces (objfpc, CORBA style)
//===--------------------------------------------------------------------===//

/// IFoo = interface ... end;  /  IFoo = interface;
void Checker::declareInterface(TypeDecl *TD, InterfaceTypeSpec *IS){
  const Identifier &Id = TD->getName();
  if (CurRoutine) {
    Diags.error(Id.Loc,
                "interface '" + Id.Spelling +
                    "' must be declared at program or unit level",
                {Id.getRange()});
    TD->setType(errorType());
    IS->setResolved(errorType());
    declare(TD);
    return;
  }
  if (!IS->isForward())
    for (auto It = PendingInterfaces.begin(); It != PendingInterfaces.end(); ++It)
      if (It->first->getName().Name == Id.Name &&
          Cur->lookupOwn(Id.Name) == It->first) {
        InterfaceType *I = It->second;
        PendingInterfaces.erase(It);
        TD->setType(I);
        IS->setResolved(I);
        defineInterface(I, IS);
        return;
      }
  auto *I = Ctx.create<InterfaceType>();
  I->setDeclName(Id.Spelling);
  I->setCOM(!IS->isCorba());
  TD->setType(I);
  IS->setResolved(I);
  declare(TD);
  if (IS->isForward()) {
    PendingInterfaces.push_back({TD, I});
    return;
  }
  defineInterface(I, IS);
}


/// The interface named Id, or null after a diagnostic.
/// specialize IGen<...> in a class's or interface's heading: the
/// interface it is, or null after a diagnostic.
InterfaceType *Checker::specializedInterface(const Identifier &Id, TypeSpec *Spec){
  Type *T = resolveType(Spec);
  if (T->isError())
    return nullptr;
  auto *I = llvm::dyn_cast<InterfaceType>(T);
  if (!I) {
    Diags.error(Spec->getBeginLoc(), quoted(T) + " is not an interface type",
                {Spec->getRange()});
    return nullptr;
  }
  if (!I->isDefined()) {
    Diags.error(Id.Loc,
                "interface '" + I->getName() + "' is not defined yet",
                {Spec->getRange()});
    return nullptr;
  }
  return I;
}


InterfaceType *Checker::lookupInterface(const Identifier &Id){
  Decl *D = Cur->lookup(Id.Name);
  if (!D) {
    Diags.error(Id.Loc,
                "unknown interface name '" + Id.Spelling + "'" +
                    didYouMean(Id.Name),
                {Id.getRange()});
    return nullptr;
  }
  auto *TD = llvm::dyn_cast<TypeDecl>(D);
  if (TD && TD->getType() && TD->getType()->isError())
    return nullptr;
  auto *I = TD ? llvm::dyn_cast_or_null<InterfaceType>(TD->getType()) : nullptr;
  if (!I) {
    Diags.error(Id.Loc, "'" + Id.Spelling + "' is not an interface type",
                {Id.getRange()});
    noteDeclaredHere(D);
    return nullptr;
  }
  if (!I->isDefined()) {
    Diags.error(Id.Loc,
                "interface '" + Id.Spelling +
                    "' is not defined yet (only declared forward)",
                {Id.getRange()});
    return nullptr;
  }
  return I;
}


/// The method Name of I or its nearest ancestor declaring it (the first
/// of its overloads).
RoutineDecl *Checker::findInterfaceMethod(InterfaceType *I, llvm::StringRef Name){
  for (; I; I = I->getParent())
    for (RoutineDecl *R : I->getMethods())
      if (R->getName().Name == Name)
        return R;
  return nullptr;
}


void Checker::defineInterface(InterfaceType *I, InterfaceTypeSpec *IS){
  InterfaceType *Parent = nullptr;
  I->setCOM(!IS->isCorba());
  if (IS->getParent().isValid()) {
    const Identifier &PId = IS->getParent();
    if (IS->getParentSpec()) // interface(specialize IBase<T>)
      Parent = specializedInterface(PId, IS->getParentSpec());
    else if (PId.Name == I->getDeclName().lower())
      Diags.error(PId.Loc,
                  "interface '" + I->getName() + "' cannot inherit from itself",
                  {PId.getRange()});
    else
      Parent = lookupInterface(PId);
    if (Parent && Parent->isCOM() != I->isCOM()) {
      Diags.error(PId.Loc,
                  llvm::Twine("a ") + (I->isCOM() ? "COM" : "CORBA") +
                      " interface cannot inherit from the " +
                      (Parent->isCOM() ? "COM" : "CORBA") + " interface '" +
                      Parent->getName() + "'",
                  {PId.getRange()});
      Parent = nullptr;
    }
  }
  // A COM interface without a parent descends from IUnknown.
  if (!Parent && I->isCOM())
    Parent = IUnknownTy;
  llvm::SmallVector<RoutineDecl *, 8> Own, All;
  if (Parent)
    All.append(Parent->getAllMethods().begin(), Parent->getAllMethods().end());
  llvm::StringMap<RoutineDecl *> Seen;
  llvm::SmallVector<PropertyDecl *, 4> Props;
  for (Decl *M : IS->getMembers()) {
    if (auto *Prop = llvm::dyn_cast<PropertyDecl>(M)) {
      Props.push_back(Prop); // (after the methods: they name them)
      continue;
    }
    auto *R = llvm::cast<RoutineDecl>(M);
    const Identifier &Id = R->getName();
    R->setInterface(I);
    R->setParent(nullptr);
    resolveSignature(R);
    if (R->isVirtual() || R->isOverride() || R->isAbstract()) {
      Diags.error(Id.Loc,
                  "the methods of an interface cannot be 'virtual', "
                  "'override' or 'abstract'",
                  {Id.getRange()});
      continue;
    }
    if (RoutineDecl *Inh = Parent ? findInterfaceMethod(Parent, Id.Name) : nullptr) {
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' is already declared in interface '" +
                      Inh->getInterface()->getName() + "'",
                  {Id.getRange()});
      noteDeclaredHere(Inh);
      continue;
    }
    if (RoutineDecl *Prev = Seen.lookup(Id.Name)) {
      if (!addOverload(Prev, R))
        continue;
    } else {
      Seen[Id.Name] = R;
    }
    R->setVMTIndex(static_cast<int>(All.size()));
    All.push_back(R);
    Own.push_back(R);
  }
  I->define(Parent, Ctx.copyArray<RoutineDecl *>(Own),
            Ctx.copyArray<RoutineDecl *>(All), CurModule);
  llvm::SmallVector<PropertyDecl *, 4> Defined;
  for (PropertyDecl *Prop : Props)
    if (defineInterfaceProperty(I, Prop, Defined))
      Defined.push_back(Prop);
  I->setProperties(Ctx.copyArray<PropertyDecl *>(Defined));
}


/// property P: T read GetP [write SetP] in interface I (objfpc): its
/// accessors are methods of I (or of its ancestors), a function without
/// parameters returning T and a procedure taking one T. Array properties,
/// index and default specifiers are not supported. False after an error.
bool Checker::defineInterfaceProperty(InterfaceType *I, PropertyDecl *Prop,
                             llvm::ArrayRef<PropertyDecl *> Earlier){
  const Identifier &Id = Prop->getName();
  Type *T = resolveType(Prop->getTypeSpec());
  Prop->setType(T);
  if (Prop->isArrayProperty() || Prop->getIndexSpec() || Prop->isDefault() ||
      Prop->isClassProperty() || Prop->getDefaultValue()) {
    Diags.error(Id.Loc,
                "the properties of an interface are plain ones (no array "
                "property, index, default, class or stored value) for now",
                {Id.getRange()});
    return false;
  }
  if (findInterfaceMethod(I, Id.Name) || findInterfaceProperty(I, Id.Name) ||
      llvm::any_of(Earlier, [&](PropertyDecl *E) {
        return E->getName().Name == Id.Name;
      })) {
    Diags.error(Id.Loc,
                "'" + Id.Spelling + "' is already declared in interface '" +
                    I->getName() + "'",
                {Id.getRange()});
    return false;
  }
  if (T->isError())
    return false;
  auto Accessor = [&](const Identifier &N, bool Read) -> RoutineDecl * {
    if (!N.isValid())
      return nullptr;
    RoutineDecl *R = findInterfaceMethod(I, N.Name);
    bool Fits = R && (Read ? R->isFunction() && R->getParams().empty() &&
                                 R->getResultType() == T
                           : !R->isFunction() && R->getParams().size() == 1 &&
                                 !R->getParams()[0]->isVarParam() &&
                                 R->getParams()[0]->getType() == T);
    if (!Fits) {
      Diags.error(N.Loc,
                  "'" + N.Spelling + "' must be a method of interface '" +
                      I->getName() + "': " +
                      (Read ? "a function without parameters returning "
                            : "a procedure with one parameter of type ") +
                      quoted(T),
                  {N.getRange()});
      return nullptr;
    }
    return R;
  };
  RoutineDecl *Rd = Accessor(Prop->getReadName(), true);
  RoutineDecl *Wr = Accessor(Prop->getWriteName(), false);
  if ((Prop->getReadName().isValid() && !Rd) ||
      (Prop->getWriteName().isValid() && !Wr))
    return false;
  if (!Rd && !Wr) {
    Diags.error(Id.Loc, "property '" + Id.Spelling + "' needs 'read' or 'write'",
                {Id.getRange()});
    return false;
  }
  Prop->setAccessors(Rd, Wr);
  return true;
}


PropertyDecl *Checker::findInterfaceProperty(InterfaceType *I,
                                           llvm::StringRef Name){
  for (; I; I = I->getParent())
    for (PropertyDecl *P : I->getProperties())
      if (P->getName().Name == Name)
        return P;
  return nullptr;
}


/// The methods of class C implementing the methods of interface I (by
/// slot): methods (not class methods) of C or its ancestors with the same
/// name and heading. Where: the interface's name in C's declaration, to
/// report a missing one (null: inherited, already checked).
llvm::ArrayRef<RoutineDecl *> Checker::implementInterface(ClassType *C, InterfaceType *I,
                                                 const Identifier *Where){
  llvm::SmallVector<RoutineDecl *, 8> Impl;
  for (RoutineDecl *M : I->getAllMethods()) {
    RoutineDecl *Found = nullptr;
    for (const ClassType *X = C; X && !Found; X = X->getParent())
      for (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(
               X->findOwnMember(M->getName().Name));
           R && !Found; R = R->getNextOverload())
        if (R->getMethodKind() == MethodKind::Method && !R->isClassMethod() &&
            R->getBuiltin() == TObjectMethod::None &&
            R->isFunction() == M->isFunction() && sameSignature(R, M) &&
            (!M->isFunction() || R->getResultType() == M->getResultType()) &&
            R->isCDecl() == M->isCDecl())
          Found = R;
    if (!Found && Where) {
      Diags.error(Where->Loc,
                  "class '" + C->getName() + "' does not implement '" +
                      M->getInterface()->getName() + "." +
                      M->getName().Spelling.str() + "'",
                  {Where->getRange()});
      noteDeclaredHere(M);
    }
    Impl.push_back(Found);
  }
  return Ctx.copyArray<RoutineDecl *>(Impl);
}


/// The class named Id (a parent class or a method's class), or null after
/// a diagnostic. What: "parent class" / "class".
ClassType *Checker::lookupClass(const Identifier &Id){
  Decl *D = Cur->lookup(Id.Name);
  if (!D) {
    Diags.error(Id.Loc,
                "unknown class name '" + Id.Spelling + "'" + didYouMean(Id.Name),
                {Id.getRange()});
    return nullptr;
  }
  auto *TD = llvm::dyn_cast<TypeDecl>(D);
  if (TD && TD->getType() && TD->getType()->isError())
    return nullptr;
  auto *C = TD ? llvm::dyn_cast_or_null<ClassType>(TD->getType()) : nullptr;
  if (!C) {
    Diags.error(Id.Loc, "'" + Id.Spelling + "' is not a class type",
                {Id.getRange()});
    noteDeclaredHere(D);
    return nullptr;
  }
  if (!C->isDefined()) {
    Diags.error(Id.Loc,
                "class '" + Id.Spelling + "' is not defined yet (only declared "
                "forward)",
                {Id.getRange()});
    return nullptr;
  }
  return C;
}


/// The hidden parameter Self of method R of class C.
void Checker::makeSelf(RoutineDecl *R, ClassType *C){
  Identifier Name = memberName("Self");
  // A record's method gets the record by reference, a type helper's the
  // value it extends.
  RecordType *Rec = C->getRecord();
  auto *Self = Ctx.create<VarDecl>(
      Name, nullptr,
      C->hasSelfByReference() ? VarDecl::VarParam : VarDecl::ValueParam);
  // In a class method, Self is the class (a class reference).
  Self->setType(Rec                    ? static_cast<Type *>(Rec)
                : C->isHelper()        ? C->getHelperFor()
                : R->isClassMethod() ? static_cast<Type *>(classRefOf(C))
                                     : C);
  Self->setOwner(R);
  R->setSelfVar(Self);
}


bool Checker::sameHeading(RoutineDecl *A, RoutineDecl *B){
  return A->getMethodKind() == B->getMethodKind() &&
         A->isClassMethod() == B->isClassMethod() &&
         A->isFunction() == B->isFunction() && sameSignature(A, B) &&
         (!A->isFunction() || A->getResultType() == B->getResultType());
}


void Checker::defineClass(ClassType *C, ClassTypeSpec *CS){
  ClassType *Parent = TObjectTy;
  // class(TParent, I1, I2): the interfaces it implements (the first name
  // may be one too).
  llvm::SmallVector<std::pair<InterfaceType *, const Identifier *>, 2> Listed;
  auto AddInterface = [&](const Identifier &Id, TypeSpec *Spec) {
    InterfaceType *I = Spec ? specializedInterface(Id, Spec) : lookupInterface(Id);
    if (!I)
      return;
    for (auto &[J, _] : Listed)
      if (J == I) {
        Diags.error(Id.Loc,
                    "interface '" + Id.Spelling + "' is listed twice",
                    {Id.getRange()});
        return;
      }
    Listed.push_back({I, &Id});
  };
  if (TypeSpec *PS = CS->getParentSpec()) { // class(specialize TBase<T>)
    const Identifier &PId = CS->getParent();
    Type *T = resolveType(PS);
    if (llvm::isa<InterfaceType>(T))
      AddInterface(PId, PS);
    else if (auto *P = llvm::dyn_cast<ClassType>(T); P && !P->isHelper() &&
                                                      !P->getRecord())
      Parent = P;
    else if (!T->isError())
      Diags.error(PS->getBeginLoc(),
                  quoted(T) + " is not a class or interface type",
                  {PS->getRange()});
  } else if (CS->getParent().isValid()) {
    const Identifier &PId = CS->getParent();
    auto *TD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(PId.Name));
    if (PId.Name == C->getDeclName().lower()) {
      Diags.error(PId.Loc,
                  "class '" + C->getName() + "' cannot inherit from itself",
                  {PId.getRange()});
    } else if (TD && llvm::isa_and_nonnull<InterfaceType>(TD->getType())) {
      AddInterface(PId, nullptr);
    } else if (ClassType *P = lookupClass(PId)) {
      Parent = P;
    }
  }
  for (size_t K = 0; K < CS->getInterfaces().size(); ++K)
    AddInterface(CS->getInterfaces()[K], CS->getInterfaceSpec(K));
  // The objects of COM interfaces are reference counted by
  // TInterfacedObject.
  for (auto &[I, Where] : Listed)
    if (I->isCOM() && !Parent->inheritsFrom(TInterfacedObjectTy)) {
      Diags.error(Where->Loc,
                  "class '" + C->getName() + "' implements the COM interface '" +
                      I->getName() +
                      "', so it must descend from TInterfacedObject",
                  {Where->getRange()});
      break;
    }

  llvm::SmallVector<Decl *, 16> Members;
  llvm::SmallVector<FieldDecl *, 16> Fields(Parent->getAllFields().begin(),
                                            Parent->getAllFields().end());
  llvm::SmallVector<RoutineDecl *, 8> VTable(Parent->getVTable().begin(),
                                             Parent->getVTable().end());
  llvm::StringMap<Decl *> Own;
  PropertyDecl *DefaultProp = nullptr;
  // Nested constants and types are visible to the members after them, and
  // so are the ancestors'.
  Scope Inherited = Scope::nestedIn(Cur, Parent);
  Scope MemberScope(&Inherited);
  Scope *SavedScope = Cur;
  llvm::StringRef SavedPrefix = NestPrefix;
  Cur = &MemberScope;
  NestPrefix = C->getDeclName();
  size_t PendingClassMark = PendingClasses.size();
  size_t PendingPointerMark = PendingPointers.size();
  for (Decl *M : CS->getMembers()) {
    const Identifier &MN = M->getName();
    if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && R->isClassInit()) {
      defineClassInit(C, R); // not a member
      continue;
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(M); R && R->isGenericMethod()) {
      if (Decl *Prev = Own.lookup(R->getMemberKey())) {
        Diags.error(MN.Loc,
                    "redefinition of generic method '" + MN.Spelling + "'",
                    {MN.getRange()});
        Diags.note(Prev->getLocation(), "previous definition is here");
        continue;
      }
      defineGenericMethod(C, R);
      Own[R->getMemberKey()] = R;
      Members.push_back(R);
      continue;
    }
    RoutineDecl *OverloadOf = nullptr; // objfpc: an overloaded method
    if (Decl *Fwd = Own.lookup(MN.Name);
        isForwardClassDecl(Fwd) && llvm::isa<TypeDecl>(M)) {
      declareNestedType(llvm::cast<TypeDecl>(M)); // its definition
      Own[MN.Name] = M;
      std::replace(Members.begin(), Members.end(), Fwd, M);
      continue;
    }
    if (Decl *Prev = Own.lookup(MN.Name)) {
      OverloadOf = llvm::dyn_cast<RoutineDecl>(Prev);
      if (!LangOpts.ObjKeywords || !OverloadOf || !llvm::isa<RoutineDecl>(M)) {
        Diags.error(MN.Loc, "redefinition of '" + MN.Spelling + "'",
                    {MN.getRange()});
        Diags.note(Prev->getLocation(), "previous definition is here");
        continue;
      }
    }
    const ClassType *Owner = nullptr;
    Decl *Inherited = Parent->findMember(MN.Name, &Owner);
    if (Inherited &&
        !(llvm::isa<RoutineDecl>(M) && llvm::isa<RoutineDecl>(Inherited))) {
      Diags.error(MN.Loc,
                  "'" + MN.Spelling + "' is already declared in class '" +
                      Owner->getName() + "'",
                  {MN.getRange()});
      noteDeclaredHere(Inherited);
      continue;
    }
    if (!OverloadOf)
      Own[MN.Name] = M;
    Members.push_back(M);

    if (auto *CD = llvm::dyn_cast<ConstDecl>(M)) {
      declareConst(CD);
      continue;
    }
    if (auto *TD = llvm::dyn_cast<TypeDecl>(M)) {
      declareNestedType(TD);
      addNestedMembers(C, TD, Members, Own);
      continue;
    }
    if (auto *F = llvm::dyn_cast<FieldDecl>(M)) {
      F->setType(resolveType(F->getTypeSpec()));
      if (containsFile(F->getType()))
        Diags.error(MN.Loc, "a field of a class cannot be a file",
                    {MN.getRange()});
      if (F->getRace() == RaceQualifier::Isolated)
        Diags.error(MN.Loc,
                    "a field cannot be isolated (only variables can); its "
                    "object belongs to whoever has the object",
                    {MN.getRange()});
      else
        checkRaceDecl(F->getRace(), F->getType(), MN, "field");
      Fields.push_back(F);
      continue;
    }
    if (auto *V = llvm::dyn_cast<VarDecl>(M)) { // class var: static storage
      declareClassVar(C, V);
      continue;
    }
    if (auto *R = llvm::dyn_cast<RoutineDecl>(M)) {
      defineMethod(C, Parent, R,
                   llvm::dyn_cast_or_null<RoutineDecl>(Inherited), Owner,
                   VTable);
      if (OverloadOf && !addOverload(OverloadOf, R)) {
        R->setClass(nullptr); // rejected: not a member
        Members.pop_back();
      }
      continue;
    }
    auto *P = llvm::cast<PropertyDecl>(M);
    definePropertyAccessors(C, P, Own, Parent);
    if (P->isDefault()) {
      if (DefaultProp) {
        Diags.error(MN.Loc,
                    "class '" + C->getName() +
                        "' already has a default property",
                    {MN.getRange()});
        noteDeclaredHere(DefaultProp);
      }
      DefaultProp = P;
    }
  }
  // A forward class declared in the class must be defined in it; pointers
  // declared in it may point to its later types.
  for (size_t I = PendingClassMark; I < PendingClasses.size(); ++I) {
    auto &[TD, NC] = PendingClasses[I];
    Diags.error(TD->getName().Loc,
                "class '" + TD->getName().Spelling +
                    "' is declared forward but never defined",
                {TD->getName().getRange()});
    NC->define(TObjectTy, {}, TObjectTy->getAllFields(),
               TObjectTy->getVTable(), CurModule);
  }
  PendingClasses.resize(PendingClassMark);
  resolvePendingPointersFrom(PendingPointerMark);
  Cur = SavedScope;
  NestPrefix = SavedPrefix;
  C->define(Parent, Ctx.copyArray<Decl *>(Members),
            Ctx.copyArray<FieldDecl *>(Fields),
            Ctx.copyArray<RoutineDecl *>(VTable), CurModule);
  // The interfaces: the parent's (with this class's overrides), then its own.
  llvm::SmallVector<InterfaceImpl, 4> Impls;
  for (const InterfaceImpl &PI : Parent->getInterfaces())
    Impls.push_back({PI.Intf, implementInterface(C, PI.Intf, nullptr)});
  for (auto &[I, Where] : Listed)
    if (!Parent->findInterface(I))
      Impls.push_back({I, implementInterface(C, I, Where)});
  C->setInterfaces(Ctx.copyArray<InterfaceImpl>(Impls));
}


/// generic function M<T>(...) in class C: a member found only by its key
/// (M<N>), not checked; its specializations are made from its
/// implementation (instantiateMethod). Not virtual: each specialization
/// is a method of its own.
void Checker::defineGenericMethod(ClassType *C, RoutineDecl *R){
  const Identifier &Id = R->getName();
  R->setClass(C);
  R->setParent(nullptr);
  auto Params = R->getTypeParams();
  for (size_t I = 0; I < Params.size(); ++I)
    for (size_t J = 0; J < I; ++J)
      if (Params[I].Name == Params[J].Name)
        Diags.error(Params[I].Loc,
                    "duplicate type parameter '" + Params[I].Spelling + "'",
                    {Params[I].getRange()});
  const char *Bad =
      R->isVirtual() || R->isOverride() || R->isAbstract()
          ? "a generic method cannot be virtual, override or abstract (each "
            "specialization is a method of its own)"
      : R->isConstructor() || R->isDestructor()
          ? "a constructor or a destructor cannot be generic"
      : R->isOperator() ? "an operator cannot be generic"
                        : nullptr;
  if (Bad)
    Diags.error(Id.Loc, Bad, {Id.getRange()});
}


/// The class constructor or destructor R of class or advanced record C:
/// one of each, without parameters or directives, in a type declared at
/// program or unit level (as in Free Pascal); static, so without Self.
void Checker::defineClassInit(ClassType *C, RoutineDecl *R){
  const Identifier &Id = R->getName();
  bool Ctor = R->getClassInit() == ClassInitKind::Constructor;
  const char *Kind = Ctor ? "class constructor" : "class destructor";
  const char *What = C->getRecord() ? "record" : "class";
  R->setClass(C);
  R->setParent(nullptr);
  resolveSignature(R);
  if (CurRoutine) {
    Diags.error(Id.Loc,
                llvm::Twine("a ") + Kind + " needs a " + What +
                    " declared at program or unit level",
                {Id.getRange()});
    return;
  }
  if (RoutineDecl *Prev =
          Ctor ? C->getClassConstructor() : C->getClassDestructor()) {
    Diags.error(Id.Loc,
                llvm::Twine("only one ") + Kind + " can be declared in " +
                    What + " '" + C->getName() + "'",
                {Id.getRange()});
    noteDeclaredHere(Prev);
    return;
  }
  if (!R->getParams().empty())
    Diags.error(Id.Loc, llvm::Twine("a ") + Kind + " cannot have parameters",
                {Id.getRange()});
  const char *Directive = R->isVirtual()               ? "virtual"
                          : R->isOverride()            ? "override"
                          : R->isAbstract()            ? "abstract"
                          : R->hasOverloadDirective()  ? "overload"
                          : R->isInline()              ? "inline"
                          : R->getCallConv() != CallConv::Default
                              ? callConvName(R->getCallConv())
                              : nullptr;
  if (Directive)
    Diags.error(Id.Loc,
                llvm::Twine("'") + Directive + "' cannot be used with a " +
                    Kind,
                {Id.getRange()});
  if (Ctor)
    C->setClassConstructor(R);
  else
    C->setClassDestructor(R);
}


void Checker::defineMethod(ClassType *C, ClassType *Parent, RoutineDecl *R,
                  RoutineDecl *Inherited,
                  const ClassType *Owner,
                  llvm::SmallVectorImpl<RoutineDecl *> &VTable){
  const Identifier &Id = R->getName();
  R->setClass(C);
  R->setParent(nullptr);
  resolveSignature(R);
  // A static class method has no Self: it is called like a routine.
  if (R->isStatic()) {
    const char *Bad = !R->isClassMethod() ? "only class methods can be 'static'"
                      : R->isVirtual()    ? "'static' cannot be used with 'virtual'"
                      : R->isOverride()   ? "'static' cannot be used with 'override'"
                      : R->isAbstract()   ? "'static' cannot be used with 'abstract'"
                                          : nullptr;
    if (Bad) {
      Diags.error(Id.Loc, Bad, {Id.getRange()});
      R->setStatic(false);
    }
  }
  if (!R->isStatic())
    makeSelf(R, C);
  bool Virtual = R->isVirtual(), Override = R->isOverride();
  // Of overloaded inherited methods, the one with this heading.
  if (Inherited && Inherited->isOverloaded()) {
    RoutineDecl *Match = nullptr;
    for (RoutineDecl *X = Inherited; X; X = X->getNextOverload())
      if (sameSignature(X, R))
        Match = X;
    if (Match || Override)
      Inherited = Match;
  }
  if (Virtual && Override) {
    Diags.error(Id.Loc,
                "method '" + Id.Spelling +
                    "' cannot be both 'virtual' and 'override'",
                {Id.getRange()});
    return;
  }
  if (R->isAbstract() && !Virtual && !Override) {
    Diags.error(Id.Loc,
                "abstract method '" + Id.Spelling + "' must be 'virtual'",
                {Id.getRange()});
    return;
  }
  if (Override) {
    if (!Inherited || !Inherited->isDispatched()) {
      Diags.error(Id.Loc,
                  "method '" + Id.Spelling +
                      "' is marked 'override' but there is no virtual method '" +
                      Id.Spelling + "' to override in '" +
                      Parent->getName() + "'",
                  {Id.getRange()});
      if (Inherited)
        noteDeclaredHere(Inherited);
      return;
    }
    if (!sameHeading(Inherited, R)) {
      Diags.error(Id.Loc,
                  "heading of '" + Id.Spelling +
                      "' does not match the method it overrides in '" +
                      Owner->getName() + "'",
                  {Id.getRange()});
      noteDeclaredHere(Inherited);
      return;
    }
    if (R->getCallConv() == CallConv::Default) {
      if (Inherited->isCDecl())
        R->setInheritedCDecl(); // as in Free Pascal
    } else if (R->isCDecl() != Inherited->isCDecl()) {
      Diags.error(Id.Loc,
                  "calling convention of '" + Id.Spelling + "' is " +
                      callConvDesc(R->isCDecl()) +
                      " but the method it overrides in '" +
                      Owner->getName() + "' is " +
                      callConvDesc(Inherited->isCDecl()),
                  {Id.getRange()});
      noteDeclaredHere(Inherited);
      return;
    }
    R->setVMTIndex(Inherited->getVMTIndex());
    VTable[Inherited->getVMTIndex()] = R;
    return;
  }
  if (Virtual) {
    R->setVMTIndex(static_cast<int>(VTable.size()));
    VTable.push_back(R);
  }
}


/// property P[params]: T [index N] read R write W -- R: a field of type T
/// (a plain property) or a function returning T; W: a field of type T or
/// a procedure taking the value (a value or const parameter of type T).
/// The methods first take the index parameters of an array property
/// (same types and kinds), then N (an integer) for an indexed property.
/// Both must be declared before the property.
void Checker::definePropertyAccessors(ClassType *C, PropertyDecl *P,
                             const llvm::StringMap<Decl *> &Own,
                             ClassType *Parent){
  Type *T = resolveType(P->getTypeSpec());
  P->setType(T);
  bool ParamsOK = true;
  for (VarDecl *Param : P->getParams()) {
    Param->setType(resolveType(Param->getTypeSpec()));
    if (Param->getType()->isError())
      ParamsOK = false;
    if (Param->isVarParam()) {
      Diags.error(Param->getLocation(),
                  "the index parameters of a property cannot be 'var'",
                  {Param->getName().getRange()});
      ParamsOK = false;
    }
  }
  if (Expr *E = P->getIndexSpec()) {
    Type *IT = check(E);
    if (!IT->isError() && (!IT->isIntegerLike() || !fold(E))) {
      Diags.error(E->getBeginLoc(),
                  "the index of a property must be an integer constant",
                  {E->getRange()});
      ParamsOK = false;
    }
  }
  if (Expr *E = P->getDefaultValue()) {
    Type *DT = check(E);
    if (!DT->isError() && !T->isError() && (!fold(E) || !isAssignable(T, DT)))
      Diags.error(E->getBeginLoc(),
                  "the default value of property '" + P->getName().Spelling +
                      "' must be a constant of type " + quoted(T),
                  {E->getRange()});
  }
  if (P->isDefault() && !P->isArrayProperty())
    Diags.error(P->getLocation(),
                "only an array property can be the default property",
                {P->getName().getRange()});
  if (!P->getReadName().isValid() && !P->getWriteName().isValid()) {
    Diags.error(P->getLocation(),
                "property '" + P->getName().Spelling +
                    "' needs a 'read' or 'write' accessor",
                {P->getName().getRange()});
    return;
  }
  auto Find = [&](const Identifier &Id) -> Decl * {
    if (Decl *D = Own.lookup(Id.Name))
      return D;
    Decl *D = Parent ? Parent->findMember(Id.Name) : nullptr;
    if (!D)
      Diags.error(Id.Loc,
                  "no field or method named '" + Id.Spelling + "' in '" +
                      C->getName() + "'",
                  {Id.getRange()});
    return D;
  };
  // An accessor method's leading parameters: the indexes, then N.
  bool Plain = !P->isArrayProperty() && !P->getIndexSpec();
  auto IndexParamsMatch = [&](RoutineDecl *R) {
    auto Params = R->getParams();
    size_t N = P->getParams().size();
    if (Params.size() < N + (P->getIndexSpec() ? 1 : 0))
      return false;
    for (size_t I = 0; I < N; ++I)
      if (Params[I]->getType() != P->getParams()[I]->getType() ||
          Params[I]->getVarKind() != P->getParams()[I]->getVarKind() ||
          Params[I]->isConstRefParam() != P->getParams()[I]->isConstRefParam())
        return false;
    if (P->getIndexSpec())
      return !Params[N]->isVarParam() && Params[N]->getType() &&
             Params[N]->getType()->isIntegerLike();
    return true;
  };
  size_t Leading = P->getParams().size() + (P->getIndexSpec() ? 1 : 0);
  auto Wanted = [&](bool IsRead) {
    std::string S;
    for (VarDecl *Param : P->getParams())
      S += std::string(S.empty() ? "" : "; ") +
           (Param->isConstRefParam() ? "constref "
            : Param->isConstParam() ? "const "
                                    : "") +
           Param->getName().Spelling.str() + ": " +
           Param->getType()->getName();
    if (P->getIndexSpec())
      S += std::string(S.empty() ? "" : "; ") + "index: integer";
    if (!IsRead)
      S += std::string(S.empty() ? "" : "; ") + "value: " + T->getName();
    std::string Kind = P->isClassProperty() ? "a static class " : "a ";
    return IsRead ? Kind + "function (" + S + "): " + T->getName()
                  : Kind + "procedure (" + S + ")";
  };
  // A class property's accessors: class variables or static class
  // methods (as in Free Pascal); an instance property's: fields or
  // methods.
  bool ClassProp = P->isClassProperty();
  const char *PropWord = ClassProp ? "class property '" : "property '";
  auto Storage = [&](Decl *D) -> Type * {
    if (ClassProp) {
      auto *V = llvm::dyn_cast<VarDecl>(D);
      return V && V->isClassVar() ? V->getType() : nullptr;
    }
    auto *F = llvm::dyn_cast<FieldDecl>(D);
    return F ? F->getType() : nullptr;
  };
  const char *StorageWord = ClassProp ? "a class variable" : "a field";
  const char *FnWord = ClassProp ? "a static class function" : "a function";
  const char *ProcWord = ClassProp ? "a static class procedure" : "a procedure";
  Decl *Read = nullptr, *Write = nullptr;
  if (P->getReadName().isValid() && (Read = Find(P->getReadName()))) {
    const Identifier &Id = P->getReadName();
    Type *FT = Storage(Read);
    auto *R = llvm::dyn_cast<RoutineDecl>(Read);
    auto Fits = [&](RoutineDecl *X) {
      return X->isFunction() && X->getParams().size() == Leading &&
             IndexParamsMatch(X) && X->getResultType() == T &&
             (!ClassProp || X->isStatic());
    };
    for (RoutineDecl *X = R; R && R->isOverloaded() && X; X = X->getNextOverload())
      if (Fits(X)) { // of overloaded methods, the one that fits
        Read = R = X;
        break;
      }
    bool OK = (Plain && FT == T) || (R && Fits(R));
    if (!OK && !T->isError() && ParamsOK) {
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' cannot be read as " + PropWord +
                      P->getName().Spelling + "' of type " + quoted(T) +
                      (Plain ? " (use " + std::string(StorageWord) +
                                   " of that type or " + FnWord +
                                   " without parameters returning it)"
                             : " (use " + Wanted(true) + ")"),
                  {Id.getRange()});
      Read = nullptr;
    }
  }
  if (P->getWriteName().isValid() && (Write = Find(P->getWriteName()))) {
    const Identifier &Id = P->getWriteName();
    Type *FT = Storage(Write);
    auto *R = llvm::dyn_cast<RoutineDecl>(Write);
    auto Fits = [&](RoutineDecl *X) {
      return X->getMethodKind() == MethodKind::Method && X->isProcedure() &&
             X->getParams().size() == Leading + 1 && IndexParamsMatch(X) &&
             !X->getParams()[Leading]->isVarParam() &&
             X->getParams()[Leading]->getType() == T &&
             (!ClassProp || X->isStatic());
    };
    for (RoutineDecl *X = R; R && R->isOverloaded() && X; X = X->getNextOverload())
      if (Fits(X)) { // of overloaded methods, the one that fits
        Write = R = X;
        break;
      }
    bool OK = (Plain && FT == T) || (R && Fits(R));
    if (!OK && !T->isError() && ParamsOK) {
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' cannot be written as " + PropWord +
                      P->getName().Spelling + "' of type " + quoted(T) +
                      (Plain ? " (use " + std::string(StorageWord) +
                                   " of that type or " + ProcWord +
                                   " with one parameter of it)"
                             : " (use " + Wanted(false) + ")"),
                  {Id.getRange()});
      Write = nullptr;
    }
  }
  P->setAccessors(Read, Write);
}


/// The default property of C or its nearest ancestor declaring one.
PropertyDecl *Checker::defaultProperty(const ClassType *C){
  for (; C; C = C->getParent())
    for (Decl *M : C->getMembers())
      if (auto *P = llvm::dyn_cast<PropertyDecl>(M); P && P->isDefault())
        return P;
  return nullptr;
}


/// TS in the heading of a method of C: the nested types of C and of the
/// classes around it are visible.
Type *Checker::resolveTypeIn(ClassType *C, TypeSpec *TS){
  llvm::SmallVector<ClassType *, 2> Chain;
  for (ClassType *O = C; O; O = O->getOuter())
    Chain.push_back(O);
  llvm::SmallVector<std::unique_ptr<Scope>, 2> Scopes;
  Scope *Saved = Cur;
  for (ClassType *O : llvm::reverse(Chain)) {
    Scopes.push_back(std::make_unique<Scope>(Scope::nestedIn(Cur, O)));
    Cur = Scopes.back().get();
  }
  Type *T = resolveType(TS);
  Cur = Saved;
  return T;
}


/// procedure TFoo.Bar ...; begin ... end;  -- the body of a method.
void Checker::declareMethodBody(RoutineDecl *R){
  const Identifier &CN = R->getClassName(), &Id = R->getName();
  if (CurRoutine) {
    Diags.error(CN.Loc,
                "method '" + CN.Spelling + "." + Id.Spelling +
                    "' must be implemented at program or unit level",
                {CN.getRange()});
    return;
  }
  ClassType *C = nullptr;
  if (!R->getOuterClassNames().empty()) { // TOuter.TInner.Bar
    auto *TD = llvm::dyn_cast_or_null<TypeDecl>(
        lookupMemberPath(R->getOuterClassNames(), CN));
    Type *T = TD ? TD->getType() : nullptr;
    C = llvm::dyn_cast_or_null<ClassType>(T);
    if (!C)
      C = memberTableOf(T);
    if (!C) {
      if (TD && !T->isError())
        Diags.error(CN.Loc,
                    "'" + CN.Spelling + "' is not a class or record with members",
                    {CN.getRange()});
      return;
    }
  } else {
    if (auto *TD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(CN.Name)))
      C = memberTableOf(TD->getType()); // an advanced record
    if (!C)
      C = lookupClass(CN);
  }
  if (!C)
    return;
  const char *What = C->getRecord() ? "record" : "class";
  if (R->isClassInit()) { // class constructor TFoo.Create
    bool Ctor = R->getClassInit() == ClassInitKind::Constructor;
    RoutineDecl *M = Ctor ? C->getClassConstructor() : C->getClassDestructor();
    if (!M || M->getName().Name != Id.Name) {
      Diags.error(Id.Loc,
                  llvm::Twine(What) + " '" + C->getName() + "' has no " +
                      (Ctor ? "class constructor '" : "class destructor '") +
                      Id.Spelling + "'",
                  {Id.getRange()});
      if (M)
        noteDeclaredHere(M);
      return;
    }
    if (C->getModule() != CurModule) {
      Diags.error(Id.Loc,
                  "'" + qualified(C, M->getName()) +
                      "' must be implemented where its " + What +
                      " is declared",
                  {Id.getRange()});
      return;
    }
    if (M->getBody()) {
      Diags.error(Id.Loc, "redefinition of '" + qualified(C, M->getName()) + "'",
                  {Id.getRange()});
      return;
    }
    if (!R->getParams().empty())
      Diags.error(Id.Loc,
                  llvm::Twine("a ") +
                      (Ctor ? "class constructor" : "class destructor") +
                      " cannot have parameters",
                  {Id.getRange()});
    M->setBody(R->getBody());
    M->setResultName(R->hasResultName());
    M->setImplementation(R);
    R->setCanonical(M);
    checkRoutineBody(M);
    return;
  }
  auto *M = llvm::dyn_cast_or_null<RoutineDecl>(C->findOwnMember(Id.Name));
  if (!M || M->getBuiltin() != TObjectMethod::None) {
    Diags.error(Id.Loc,
                llvm::Twine(What) + " '" + C->getName() + "' has no " +
                    (R->isOperator() ? "operator '" : "method '") +
                    Id.Spelling + "'",
                {Id.getRange()});
    return;
  }
  if (M->isOverloaded()) { // objfpc: the method with this heading
    for (VarDecl *P : R->getParams())
      P->setType(resolveTypeIn(C, P->getTypeSpec()));
    Type *RT = isConversionOperator(M) && R->getResultTypeSpec()
                   ? resolveTypeIn(C, R->getResultTypeSpec())
                   : nullptr;
    RoutineDecl *Match = nullptr;
    for (RoutineDecl *X = M; X; X = X->getNextOverload())
      if (sameSignature(X, R) && !X->getBody() &&
          (!RT || RT == X->getResultType()))
        Match = X;
    if (!Match) {
      Diags.error(Id.Loc,
                  "heading of '" + qualified(C, M->getName()) +
                      "' does not match any of its overloads",
                  {Id.getRange()});
      for (RoutineDecl *X = M; X; X = X->getNextOverload())
        Diags.note(X->getLocation(), "candidate: " + routineHeading(X));
      return;
    }
    M = Match;
  }
  std::string QName = qualified(C, M->getName());
  if (C->getModule() != CurModule) {
    Diags.error(Id.Loc,
                "method '" + QName + "' must be implemented where its " +
                    What + " is declared",
                {Id.getRange()});
    return;
  }
  if (M->getMethodKind() != R->getMethodKind() ||
      (M->isFunction() || M->isAsync() || M->isIterator()) != R->isFunction() ||
      M->isClassMethod() != R->isClassMethod()) {
    Diags.error(Id.Loc,
                "'" + QName + "' was declared as a " +
                    (M->isClassMethod() ? "class " : "") + M->getKindName(),
                {Id.getRange()});
    noteDeclaredHere(M);
    return;
  }
  if (M->isAbstract()) {
    Diags.error(Id.Loc, "abstract method '" + QName + "' cannot have a body",
                {Id.getRange()});
    noteDeclaredHere(M);
    return;
  }
  if (M->getBody()) {
    Diags.error(Id.Loc, "redefinition of '" + QName + "'", {Id.getRange()});
    return;
  }
  if (R->hasParamList() || R->getResultTypeSpec()) {
    for (VarDecl *P : R->getParams())
      P->setType(resolveTypeIn(C, P->getTypeSpec()));
    Type *RT = R->getResultTypeSpec() ? resolveTypeIn(C, R->getResultTypeSpec())
                                      : nullptr;
    Type *Declared = M->isAsync()      ? M->getAsyncType()
                     : M->isIterator() ? M->getIteratorType()
                                       : M->getResultType();
    if (!sameSignature(M, R) || (RT && RT != Declared) ||
        !sameDefaults(M, R)) {
      Diags.error(Id.Loc,
                  "heading of '" + QName + "' does not match its declaration",
                  {Id.getRange()});
      noteDeclaredHere(M);
    }
  }
  if (SourceLocation SL = R->getStaticDirectiveLoc(); SL.isValid()) {
    if (!R->isClassMethod() || R->getMethodKind() != MethodKind::Method)
      Diags.error(SL, "'static' is only allowed on class methods");
    else if (!M->isStatic()) {
      Diags.error(SL, "'" + QName + "' was not declared 'static'");
      noteDeclaredHere(M);
    }
  }
  if (R->isCDecl() != M->isCDecl()) {
    Diags.error(Id.Loc,
                "calling convention of '" + QName + "' is " +
                    callConvDesc(R->isCDecl()) + " but its declaration's is " +
                    callConvDesc(M->isCDecl()),
                {Id.getRange()});
    noteDeclaredHere(M);
  }
  M->setBody(R->getBody());
  M->setResultName(R->hasResultName()); // (as where its body is)
  M->setImplementation(R);
  R->setCanonical(M);
  if (R->isInline())
    M->setInline();
  checkRoutineBody(M);
}


/// C's class constructor and destructor have bodies.
void Checker::checkClassInitImplemented(ClassType *C){
  for (RoutineDecl *R : {C->getClassConstructor(), C->getClassDestructor()})
    if (R && !R->getBody())
      Diags.error(R->getName().Loc,
                  llvm::Twine(R->getClassInit() == ClassInitKind::Constructor
                                  ? "class constructor '"
                                  : "class destructor '") +
                      qualified(C, R->getName()) +
                      "' is declared but never implemented",
                  {R->getName().getRange()});
}


/// Every method of the classes declared in B has a body.
void Checker::checkMethodsImplemented(Block &B){
  for (Decl *D : B.getDecls())
    if (auto *TD = llvm::dyn_cast<TypeDecl>(D))
      checkMethodsImplemented(TD);
}


/// ... of the class or advanced record TD, and of those it declares.
void Checker::checkMethodsImplemented(TypeDecl *TD){
  if (llvm::isa_and_nonnull<HelperTypeSpec>(TD->getTypeSpec())) {
    if (auto *C = llvm::dyn_cast_or_null<ClassType>(TD->getType()))
      for (Decl *M : C->getMembers())
        if (auto *R = llvm::dyn_cast<RoutineDecl>(M);
            R && !R->getBody() &&
            !(R->isGenericMethod() && R->getGenericDefinition() != R))
          Diags.error(R->getName().Loc,
                      "method '" + qualified(C, R->getName()) +
                          "' is declared but never implemented",
                      {R->getName().getRange()});
    return;
  }
  if (ClassType *MT = memberTableOf(TD->getType())) {
    checkClassInitImplemented(MT);
    for (Decl *M : MT->getMembers()) { // an advanced record's
      if (auto *R = llvm::dyn_cast<RoutineDecl>(M);
          R && !R->getBody() &&
          !(R->isGenericMethod() && R->getGenericDefinition() != R))
        Diags.error(R->getName().Loc,
                    "method '" + qualified(MT, R->getName()) +
                        "' is declared but never implemented",
                    {R->getName().getRange()});
      else if (auto *NT = llvm::dyn_cast<TypeDecl>(M))
        checkMethodsImplemented(NT);
    }
    return;
  }
  auto *CS = llvm::dyn_cast_or_null<ClassTypeSpec>(TD->getTypeSpec());
  if (!CS || CS->isForward())
    return;
  auto *C = llvm::dyn_cast_or_null<ClassType>(TD->getType());
  if (!C)
    return;
  checkClassInitImplemented(C);
  for (Decl *M : CS->getMembers()) {
    if (auto *R = llvm::dyn_cast<RoutineDecl>(M);
        R && R->getClass() == C && !R->isAbstract() && !R->getBody() &&
        !R->isClassInit() &&
        !(R->isGenericMethod() && R->getGenericDefinition() != R))
      Diags.error(R->getName().Loc,
                  "method '" + qualified(C, R->getName()) +
                      "' is declared but never implemented",
                  {R->getName().getRange()});
    else if (auto *NT = llvm::dyn_cast<TypeDecl>(M))
      checkMethodsImplemented(NT);
  }
}


/// The method whose body (or a routine nested in it) is being checked.
RoutineDecl *Checker::enclosingMethod() const{
  for (RoutineDecl *R = CurRoutine; R; R = R->getParent())
    if (R->isMethod())
      return R;
  return nullptr;
}


/// The class whose members a with context exposes (null for records):
/// an object's, or in a class method Self's (a class reference).
ClassType *Checker::classOf(WithContext *W){
  if (!W)
    return nullptr;
  if (auto *R = llvm::dyn_cast_or_null<ClassRefType>(W->Record->getType()))
    return R->getClass();
  return llvm::dyn_cast_or_null<ClassType>(W->Record->getType());
}


/// Does with context W expose methods (an object's, a class's in a class
/// method, or an advanced record's)?
bool Checker::hasMembers(WithContext *W){
  return W && (W->Helper || classOf(W) || memberTableOf(W->Record->getType()));
}


/// Is D an instance member (a field, a property, a method that is not a
/// class method or constructor)? Class methods cannot use them.
bool Checker::isInstanceMember(Decl *D){
  if (auto *P = llvm::dyn_cast<PropertyDecl>(D))
    return !P->isClassProperty();
  if (llvm::isa<FieldDecl>(D))
    return true;
  auto *R = llvm::dyn_cast<RoutineDecl>(D);
  return R && R->isMethod() && !R->isClassMethod() && !R->isConstructor();
}


/// In a class method, Self is a class: a constructor called through it
/// creates an object of that class (its static type: Self's class).
ClassType *Checker::constructedThroughSelf(RoutineDecl *R, WithContext *W){
  if (!R->isConstructor() || !W || !W->IsSelf)
    return nullptr;
  auto *CR = llvm::dyn_cast_or_null<ClassRefType>(W->Record->getType());
  return CR ? CR->getClass() : nullptr;
}


/// Reports use of a private or protected member D of class Owner where it
/// is not visible. Private members are visible in the program or unit
/// declaring the class, protected ones also in methods of descendants;
/// strict private members only in methods of the class, strict protected
/// ones also in methods of descendants.
void Checker::checkMemberAccess(Decl *D, const ClassType *Owner, const Identifier &Id){
  Visibility V = D->getVisibility();
  if (V == Visibility::Public || V == Visibility::Published || !Owner)
    return;
  bool Strict = V == Visibility::StrictPrivate || V == Visibility::StrictProtected;
  if (!Strict && Owner->getModule() == CurModule)
    return;
  RoutineDecl *M = enclosingMethod();
  // (private, from another module: a specialization's own methods)
  if (M && (V == Visibility::StrictPrivate || V == Visibility::Private
                ? M->getClass() == Owner
                : M->getClass()->inheritsFrom(Owner)))
    return;
  const char *What = V == Visibility::Private           ? "private"
                     : V == Visibility::Protected       ? "protected"
                     : V == Visibility::StrictPrivate   ? "strict private"
                                                        : "strict protected";
  Diags.error(Id.Loc,
              "'" + Id.Spelling + "' is a " + What + " member of '" +
                  Owner->getName() + "'",
              {Id.getRange()});
  noteDeclaredHere(D);
}


/// Visibility of a member found by name through a class scope; in a
/// class method (Self is a class), only class members are usable.
void Checker::checkScopeAccess(Decl *D, WithContext *W, const Identifier &Id){
  if (ClassType *C = classOf(W)) {
    const ClassType *Owner = nullptr;
    C->findMember(Id.Name, &Owner);
    checkMemberAccess(D, Owner, Id);
    if (W->IsSelf && W->Record->getType()->isClassRef() && isInstanceMember(D)) {
      Diags.error(Id.Loc,
                  "instance member '" + Id.Spelling +
                      "' cannot be used in a class method",
                  {Id.getRange()});
      noteDeclaredHere(D);
    }
  }
}


/// TFoo in TFoo.Create (objfpc): a class named before '.'.
/// The class Base names (TFoo.Create), or for an advanced record name
/// (TR.Create) its member table (Base's type: the record).
ClassType *Checker::classNamedBy(Expr *Base){
  if (!LangOpts.ObjKeywords)
    return nullptr;
  TypeDecl *TD = typeNamedBy(Base);
  auto *C = TD ? llvm::dyn_cast_or_null<ClassType>(TD->getType()) : nullptr;
  if (C && C->isHelper()) // (not a type: its methods are used on values)
    return nullptr;
  if (!C && TD)
    C = memberTableOf(TD->getType());
  if (C) {
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Base))
      N->setDecl(TD);
    Base->setType(C->getRecord() ? static_cast<Type *>(C->getRecord()) : C);
  }
  return C;
}


/// The type that Base names (TFoo, or a nested TOuter.TInner), or null.
TypeDecl *Checker::typeNamedBy(Expr *Base){
  if (auto *N = llvm::dyn_cast<NameRefExpr>(Base))
    return llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(N->getName().Name));
  auto *F = llvm::dyn_cast<FieldExpr>(Base);
  if (!F || !LangOpts.ObjKeywords)
    return nullptr;
  ClassType *Outer = classNamedBy(F->getBase());
  if (!Outer)
    return nullptr;
  const ClassType *Owner = nullptr;
  auto *TD = llvm::dyn_cast_or_null<TypeDecl>(
      Outer->findMember(F->getField().Name, &Owner));
  if (TD && F->getMember() != TD) {
    checkMemberAccess(TD, Owner, F->getField());
    F->setMember(TD);
    F->setType(TD->getType() ? TD->getType() : errorType());
  }
  return TD;
}


/// TColor.Red: the value of the enumeration type E's base names (any
/// enumeration's; the only way to a scoped one's, {$scopedenums on}).
/// Null if the base does not name an enumeration type (or names one with
/// a helper, and not one of its values: TColor.Helped), or after a
/// diagnostic (Reported).
Decl *Checker::enumValueNamedBy(FieldExpr *E, bool &Reported){
  TypeDecl *TD = typeNamedBy(E->getBase());
  auto *ET = TD ? llvm::dyn_cast_or_null<EnumType>(TD->getType()) : nullptr;
  if (!ET)
    return nullptr;
  const Identifier &F = E->getField();
  for (ConstDecl *V : ET->getValues())
    if (V->getName().Name == F.Name) {
      if (auto *N = llvm::dyn_cast<NameRefExpr>(E->getBase())) {
        N->setDecl(TD);
        warnHints(TD, N->getName());
      }
      E->getBase()->setType(ET);
      return V;
    }
  if (helperFor(ET))
    return nullptr; // a member of its helper
  Diags.error(F.Loc,
              "no value named '" + F.Spelling + "' in enumeration " +
                  quoted(ET),
              {F.getRange()});
  Reported = true;
  return nullptr;
}


/// AnsiString.Join: a static method or constant of the helper H of the
/// type E's base names.
Decl *Checker::resolveHelperStatic(FieldExpr *E, ClassType *H){
  const Identifier &F = E->getField();
  const ClassType *Owner = nullptr;
  Decl *D = H->findMember(F.Name, &Owner);
  if (!D) {
    Diags.error(F.Loc,
                "no member named '" + F.Spelling + "' in " +
                    H->helperKindName() + " " + quoted(H),
                {F.getRange()});
    return nullptr;
  }
  checkMemberAccess(D, Owner, F);
  auto *R = llvm::dyn_cast<RoutineDecl>(D);
  if (!llvm::isa<ConstDecl>(D) && !(R && R->isStatic()) &&
      !llvm::isa<VarDecl>(D)) {
    Diags.error(F.Loc,
                "only static class methods, class variables and constants "
                "of a " +
                    H->helperKindName() +
                    " can be used through a type name; '" +
                    F.Spelling + "' is a " + D->getKindName(),
                {F.getRange()});
    return nullptr;
  }
  E->setMember(D);
  return D;
}


/// Resolves r.f / obj.member / TFoo.Create; null after a diagnostic.
/// (and, in null-safe code, its object must not be nil)
Decl *Checker::resolveMember(FieldExpr *E){
  Decl *D = resolveMemberImpl(E);
  if (D && NullSafeHere && !classNamedBy(E->getBase()) &&
      isNullRef(E->getBase()->getType()))
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D);
        !R || R->getBuiltin() != TObjectMethod::Free) // (Free: nil is fine)
      requireNotNil(E->getBase(),
                    "but its member '" + E->getField().Spelling.str() +
                        "' is used",
                    ", or use '?." + E->getField().Spelling.str() + "'");
  return D;
}


Decl *Checker::resolveMemberImpl(FieldExpr *E){
  const Identifier &F = E->getField();
  ClassType *ViaClass = classNamedBy(E->getBase());
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E->getBase()); ViaClass && N)
    warnHints(N->getDecl(), N->getName()); // TFoo.Create: a use of TFoo
  if (!ViaClass) {
    bool Reported = false;
    if (Decl *V = enumValueNamedBy(E, Reported)) // TColor.Red
      return V;
    if (Reported)
      return nullptr;
    if (ClassType *H = helperNamedBy(E->getBase())) // AnsiString.Join
      return resolveHelperStatic(E, H);
    if (TypeDecl *TD = typeNamedBy(E->getBase())) // TH.Join
      if (auto *H = llvm::dyn_cast_or_null<ClassType>(TD->getType());
          H && H->isHelper()) {
        Diags.error(E->getBase()->getBeginLoc(),
                    "the members of " + H->helperKindName() + " " +
                        quoted(H) +
                        " are used through " + quoted(H->getHelperFor()),
                    {E->getBase()->getRange()});
        return nullptr;
      }
  }
  Type *BT = ViaClass ? ViaClass : check(E->getBase());
  if (BT->isError())
    return nullptr;
  if (!ViaClass && !BT->isClass() && !BT->isClassRef() && !BT->isInterface())
    if (ClassType *H = helperFor(BT)) { // a type helper's member
      const ClassType *Owner = nullptr;
      Decl *D = E->getTypeArgs().empty() ? H->findMember(F.Name, &Owner) : nullptr;
      if (!D) // a generic method (see the classes' below)
        D = genericMethodOf(E, H, Owner);
      if (D) {
        checkMemberAccess(D, Owner, F);
        E->setMember(D);
        // Self is by reference: a temporary for a value
        E->setHelperValue(!isHelperVariable(E->getBase()) ||
                          constParamRoot(E->getBase()));
        return D;
      }
    }
  if (ClassType *MT = memberTableOf(BT)) // an advanced record's member
    BT = MT;
  if (auto *RT = llvm::dyn_cast<RecordType>(BT)) {
    FieldDecl *FD = RT->findField(F.Name);
    if (!FD) {
      Diags.error(F.Loc,
                  "no field named '" + F.Spelling + "' in " + quoted(RT),
                  {F.getRange()});
      return nullptr;
    }
    E->setMember(FD);
    return FD;
  }
  if (auto *I = llvm::dyn_cast<InterfaceType>(BT)) { // s.M, s: an interface
    RoutineDecl *R = findInterfaceMethod(I, F.Name);
    if (!R)
      if (PropertyDecl *Prop = findInterfaceProperty(I, F.Name)) {
        E->setMember(Prop);
        return Prop;
      }
    if (!R) {
      Diags.error(F.Loc,
                  "no method named '" + F.Spelling + "' in interface " +
                      quoted(I),
                  {F.getRange()});
      return nullptr;
    }
    E->setMember(R);
    return R;
  }
  auto *C = llvm::dyn_cast<ClassType>(BT);
  auto *ViaRef = llvm::dyn_cast<ClassRefType>(BT); // c.M, c: class of T
  if (ViaRef)
    C = ViaRef->getClass();
  if (!C) {
    Diags.error(F.Loc,
                "'.' applied to a value of type " + quoted(BT) +
                    ", which is not a record",
                {E->getBase()->getRange()});
    return nullptr;
  }
  const ClassType *Owner = nullptr;
  Decl *D = E->getTypeArgs().empty() ? C->findMember(F.Name, &Owner) : nullptr;
  const char *What = C->getRecord() ? "record" : "class";
  if (!D) { // a generic method: specialized here, or by the call (inferred)
    if (!E->getTypeArgs().empty()) {
      D = C->findMember(genericKey(F.Name, E->getTypeArgs().size()), &Owner);
      if (!D) {
        Diags.error(F.Loc,
                    "no generic method '" + F.Spelling + "' with " +
                        llvm::Twine(E->getTypeArgs().size()) +
                        " type parameter" +
                        (E->getTypeArgs().size() == 1 ? "" : "s") + " in " +
                        What + " " + quoted(C),
                    {F.getRange()});
        return nullptr;
      }
    } else if (auto Gs = genericMethodsNamed(C, F.Name); !Gs.empty()) {
      D = Gs[0];
      C->findMember(genericKey(F.Name, Gs[0]->getTypeParams().size()), &Owner);
    }
  }
  if (!D) {
    Diags.error(F.Loc,
                "no member named '" + F.Spelling + "' in " + What + " " +
                    quoted(C) + didYouMeanMember(C, F.Name),
                {F.getRange()});
    return nullptr;
  }
  checkMemberAccess(D, Owner, F);
  if (ViaClass || ViaRef) {
    // Through a class: constructors, class methods, class variables and
    // class properties.
    bool OK = llvm::isa<VarDecl>(D) || llvm::isa<ConstDecl>(D) ||
              llvm::isa<TypeDecl>(D);
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D))
      OK = (R->isConstructor() || R->isClassMethod()) && !R->isOperator();
    if (auto *P = llvm::dyn_cast<PropertyDecl>(D))
      OK = P->isClassProperty();
    if (!OK && !(llvm::isa<RoutineDecl>(D) &&
                 llvm::cast<RoutineDecl>(D)->isGenericMethod() &&
                 llvm::cast<RoutineDecl>(D)->isClassMethod())) {
      Diags.error(F.Loc,
                  "only constructors, class methods, class variables and "
                  "class properties can be used through " +
                      (ViaClass ? std::string("the ") + What + " name '" +
                                      C->getName() + "'"
                                : std::string("a class reference")) +
                      "; '" + F.Spelling + "' is a " + D->getKindName(),
                  {F.getRange()});
      return nullptr;
    }
  }
  // obj.specialize M<T>: the specialization
  if (auto *G = llvm::dyn_cast<RoutineDecl>(D);
      G && G->isGenericMethod() && !E->getTypeArgs().empty()) {
    llvm::SmallVector<Type *, 2> Types;
    for (TypeSpec *TS : E->getTypeArgs()) {
      Type *T = resolveType(TS);
      if (T->isError())
        return nullptr;
      Types.push_back(T);
    }
    D = instantiateMethod(F, G, Types);
    if (!D)
      return nullptr;
  }
  E->setMember(D);
  return D;
}


/// E (obj.M, obj.specialize M<T>): a generic method of C or its
/// ancestors, specialized for E's type arguments if it gives them; null
/// if there is none (or after a diagnostic, Reported).
Decl *Checker::genericMethodOf(FieldExpr *E, ClassType *C, const ClassType *&Owner){
  const Identifier &F = E->getField();
  if (!E->getTypeArgs().empty()) {
    auto *G = llvm::dyn_cast_or_null<RoutineDecl>(
        C->findMember(genericKey(F.Name, E->getTypeArgs().size()), &Owner));
    if (!G)
      return nullptr;
    llvm::SmallVector<Type *, 2> Types;
    for (TypeSpec *TS : E->getTypeArgs()) {
      Type *T = resolveType(TS);
      if (T->isError())
        return nullptr;
      Types.push_back(T);
    }
    return instantiateMethod(F, G, Types);
  }
  auto Gs = genericMethodsNamed(C, F.Name);
  if (Gs.empty())
    return nullptr;
  C->findMember(genericKey(F.Name, Gs[0]->getTypeParams().size()), &Owner);
  return Gs[0];
}


/// "; did you mean 'X'?" for a member name of C: the closest member of
/// C or its ancestors (ties: the alphabetically first).
std::string Checker::didYouMeanMember(ClassType *C, llvm::StringRef Name){
  unsigned Limit = Name.size() <= 3 ? 1 : std::max<unsigned>(2, Name.size() / 3);
  unsigned Best = Limit + 1;
  llvm::StringRef BestName, BestSpelling;
  for (const ClassType *X = C; X; X = X->getParent())
    for (Decl *D : X->getMembers()) {
      llvm::StringRef Cand = D->getName().Name;
      if (Cand == Name)
        continue;
      unsigned Dist = Name.edit_distance(Cand, true, Limit);
      if (Dist < Best || (Dist == Best && Dist <= Limit && Cand < BestName)) {
        Best = Dist;
        BestName = Cand;
        BestSpelling = D->getName().Spelling;
      }
    }
  return BestName.empty() ? "" : "; did you mean '" + BestSpelling.str() + "'?";
}


/// A property used as a value: it needs a read accessor.
Type *Checker::checkPropertyRead(PropertyDecl *P, Expr *E, const Identifier &Id){
  if (P->isArrayProperty() && PendingIndexes.count(E) &&
      !llvm::isa<IndexExpr>(E))
    return P->getType(); // checked with its indexes
  if (E == CurAssignTarget || P->getRead() || P->getType()->isError() ||
      P->getReadName().isValid())
    return P->getType();
  Diags.error(Id.Loc, "property '" + Id.Spelling + "' is write-only",
              {Id.getRange()});
  noteDeclaredHere(P);
  return errorType();
}


/// p.Free or p.Destroy of an owned variable p (already resolved): an
/// error, since its object is freed again when the routine ends.
bool Checker::diagOwnedFree(FieldExpr *E){
  const Identifier &F = E->getField();
  VarDecl *OV = ownedVarOf(E->getBase());
  if (!OV || (F.Name != "free" && F.Name != "destroy"))
    return false;
  Diags.error(F.Loc,
              "'" + F.Spelling + "' cannot be called on owned variable '" +
                  OV->getName().Spelling +
                  "', which is freed when its routine ends; assign nil to "
                  "free its object now",
              {E->getRange()});
  return true;
}


Type *Checker::visitFieldExpr(FieldExpr *E){
  Decl *D = resolveMember(E);
  if (!D)
    return errorType();
  const Identifier &F = E->getField();
  if (diagOwnedFree(E))
    return errorType();
  if (auto *CD = llvm::dyn_cast<ConstDecl>(D)) { // TFoo.C, obj.C
    warnHints(CD, F);
    Identifier Id = CD->getName();
    Id.Loc = F.Loc;
    auto *Ref = Ctx.create<NameRefExpr>(Id);
    Ref->setDecl(CD);
    Ref->setType(CD->getType() ? CD->getType() : errorType());
    Used.insert(CD);
    E->setConstRef(Ref);
    return Ref->getType();
  }
  if (llvm::isa<TypeDecl>(D)) {
    Diags.error(F.Loc, "'" + F.Spelling + "' is a type, not a value",
                {F.getRange()});
    noteDeclaredHere(D);
    return errorType();
  }
  if (!llvm::isa<RoutineDecl>(D)) // (a method: once its overload is known)
    warnHints(D, F);
  if (auto *FD = llvm::dyn_cast<FieldDecl>(D))
    return FD->getType() ? FD->getType() : errorType();
  if (auto *P = llvm::dyn_cast<PropertyDecl>(D)) {
    if (!checkArrayPropertyUse(P, E, F))
      return errorType();
    return checkPropertyRead(P, E, F);
  }
  if (auto *V = llvm::dyn_cast<VarDecl>(D)) // a class variable
    return V->getType() ? V->getType() : errorType();
  // A method used as a value: a call without arguments.
  if (auto *G = llvm::cast<RoutineDecl>(D); G->isGenericMethod()) {
    Diags.error(F.Loc,
                "cannot infer the type parameters of generic method '" +
                    F.Spelling + "' without arguments; give them: "
                    "specialize " + F.Spelling + "<...>",
                {F.getRange()});
    noteDeclaredHere(G);
    return errorType();
  }
  auto *R = resolveOverload(llvm::cast<RoutineDecl>(D), {}, F, F.getRange());
  if (!R)
    return errorType();
  E->setMember(R);
  warnHints(R, F);
  if (!acceptsArgCount(R->getParams(), 0)) {
    Diags.error(F.Loc, "missing arguments in call to '" + F.Spelling + "'",
                {F.getRange()});
    noteDeclaredHere(R);
    return errorType();
  }
  if (Type *T = methodResultType(R, E))
    return T;
  Diags.error(F.Loc,
              llvm::Twine(R->getKindName()) + " '" + F.Spelling +
                  "' does not return a value",
              {F.getRange()});
  return errorType();
}


/// The value of calling method R through E (obj.M or TFoo.Create): a
/// function's result, or the new object of a constructor called through
/// its class; null if the call has no value.
Type *Checker::methodResultType(RoutineDecl *R, FieldExpr *E){
  if (R->isAsync())
    return R->getAsyncType(); // its task
  if (R->isIterator())
    return R->getIteratorType(); // its iterator
  if (R->isFunction())
    return R->getResultType();
  if (R->isConstructor()) {
    if (namesType(E->getBase())) // TFoo.Create, TOuter.TInner.Create
      return E->getBase()->getType();
    // c.Create, c: class of T: an object of class T (or a descendant)
    if (auto *CR = llvm::dyn_cast_or_null<ClassRefType>(E->getBase()->getType()))
      return CR->getClass();
  }
  return nullptr;
}


/// Does E name a type (TFoo, TOuter.TInner), as resolved?
bool Checker::namesType(Expr *E){
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
    return llvm::isa_and_nonnull<TypeDecl>(N->getDecl());
  auto *F = llvm::dyn_cast<FieldExpr>(E);
  return F && llvm::isa_and_nonnull<TypeDecl>(F->getMember());
}


/// Is Callee obj.M / TFoo.Create / inherited M -- a method call rather
/// than a call through a procedural value?
bool Checker::isMethodCallee(Expr *Callee){
  if (llvm::isa<InheritedExpr>(Callee))
    return true;
  auto *F = llvm::dyn_cast<FieldExpr>(Callee);
  if (!F || !LangOpts.ObjKeywords)
    return false;
  if (classNamedBy(F->getBase()))
    return true;
  if (ClassType *H = helperNamedBy(F->getBase()))
    return llvm::isa_and_nonnull<RoutineDecl>(H->findMember(F->getField().Name)) ||
           !genericMethodsNamed(H, F->getField().Name).empty();
  Type *BT = check(F->getBase());
  if (!BT->isClass() && !BT->isClassRef() && !BT->isInterface())
    if (ClassType *H = helperFor(BT);
        H && (llvm::isa_and_nonnull<RoutineDecl>(H->findMember(F->getField().Name)) ||
              !genericMethodsNamed(H, F->getField().Name).empty()))
      return true;
  if (auto *I = llvm::dyn_cast<InterfaceType>(BT))
    return findInterfaceMethod(I, F->getField().Name) != nullptr;
  if (ClassType *MT = memberTableOf(BT)) // an advanced record's method
    BT = MT;
  auto *C = llvm::dyn_cast<ClassType>(BT);
  if (auto *CR = llvm::dyn_cast<ClassRefType>(BT))
    C = CR->getClass();
  return C && (llvm::isa_and_nonnull<RoutineDecl>(C->findMember(F->getField().Name)) ||
               !genericMethodsNamed(C, F->getField().Name).empty());
}


/// obj.M(args), TFoo.Create(args), inherited M(args): the result type,
/// null for a call without a value, or the error type.
Type *Checker::checkMethodCall(IndirectCallExpr *E){
  if (auto *I = llvm::dyn_cast<InheritedExpr>(E->getCallee()))
    return checkInheritedCall(I, E, E->getRange());
  auto *F = llvm::cast<FieldExpr>(E->getCallee());
  Decl *D = resolveMember(F);
  if (D && diagOwnedFree(F))
    return errorType();
  auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D);
  if (R && R->isGenericMethod()) { // obj.M(args): the types from the args
    llvm::SmallVector<RoutineDecl *, 2> Gs =
        genericMethodsNamed(R->getClass(), F->getField().Name);
    R = inferFor(F->getField(), Gs, E->getArgs(), nullptr);
    if (!R) {
      for (Expr *A : E->getArgs())
        check(A);
      return errorType();
    }
    F->setMember(R);
  } else if (R) { // objfpc: overloads
    R = resolveOverload(R, E->getArgs(), F->getField(), E->getRange());
    if (!R)
      return errorType();
    F->setMember(R);
    warnHints(R, F->getField());
  }
  if (!R) {
    const Identifier &Id = F->getField();
    if (llvm::isa_and_nonnull<TypeDecl>(D))
      Diags.error(Id.Loc,
                  "typecasts through a qualified type name are not supported "
                  "yet; declare a type alias for '" + Id.Spelling + "'",
                  {Id.getRange()});
    else if (D)
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' is a " + D->getKindName() +
                      ", not a method",
                  {Id.getRange()});
    for (Expr *A : E->getArgs())
      check(A);
    return errorType();
  }
  checkCallArgs(R, F->getField(),
                orderedArgs(E, R->getParams(), F->getField()), E->getRange());
  Type *T = methodResultType(R, F);
  if (T)
    F->setType(T);
  return T;
}


/// inherited [Name]: the inherited method, or null (after a diagnostic,
/// or with Nothing set for a bare `inherited` without an inherited method,
/// which does nothing).
RoutineDecl *Checker::resolveInherited(InheritedExpr *I, bool &Nothing){
  Nothing = false;
  RoutineDecl *Encl = enclosingMethod();
  if (!Encl) {
    Diags.error(I->getBeginLoc(), "'inherited' is only allowed in a method",
                {I->getRange()});
    return nullptr;
  }
  I->setEnclosing(Encl);
  if (Encl != CurRoutine) {
    // From a routine nested in the method: the call takes the method's
    // Self, and a bare `inherited` its parameters.
    if (VarDecl *Self = Encl->getSelfVar())
      Self->setCaptured();
    for (VarDecl *P : Encl->getParams())
      P->setCaptured();
  }
  ClassType *Parent = Encl->getClass()->getParent();
  const Identifier &Name = I->isBare() ? Encl->getName() : I->getName();
  const ClassType *Owner = nullptr;
  Decl *D = Parent ? Parent->findMember(Name.Name, &Owner) : nullptr;
  auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D);
  if (!R) {
    if (I->isBare() && !D) {
      Nothing = true;
      return nullptr;
    }
    SourceLocation L = I->isBare() ? I->getBeginLoc() : Name.Loc;
    std::string In = Parent ? Parent->getName() : std::string("TObject");
    if (D) {
      Diags.error(L,
                  "'inherited' needs a method, but '" + Name.Spelling +
                      "' in '" + Owner->getName() + "' is a " +
                      D->getKindName(),
                  {I->getRange()});
      noteDeclaredHere(D);
    } else {
      Diags.error(L,
                  "no inherited method '" + Name.Spelling + "' in '" + In + "'",
                  {I->getRange()});
    }
    return nullptr;
  }
  checkMemberAccess(R, Owner, Name);
  if (R->isAbstract()) {
    Diags.error(I->getBeginLoc(),
                "cannot call abstract method '" + Name.Spelling + "'",
                {I->getRange()});
    noteDeclaredHere(R);
    return nullptr;
  }
  I->setMethod(R);
  return R;
}


/// inherited [Name] [(Args)] called: the result type or null.
Type *Checker::checkInheritedCall(InheritedExpr *I, IndirectCallExpr *Call,
                         SourceRange Range){
  llvm::ArrayRef<Expr *> Args = Call->getArgs();
  bool Nothing;
  RoutineDecl *R = resolveInherited(I, Nothing);
  if (R) { // objfpc: overloads; a bare inherited means the same heading
    llvm::SmallVector<RoutineDecl *, 4> Cands;
    collectOverloads(R, Cands);
    if (Cands.size() > 1 && I->isBare() && Args.empty()) {
      RoutineDecl *Same = nullptr;
      for (RoutineDecl *X : Cands)
        if (!Same && sameSignature(X, I->getEnclosing()))
          Same = X;
      if (Same)
        R = Same;
    } else if (Cands.size() > 1) {
      R = resolveOverload(R, Args, I->isBare() ? R->getName() : I->getName(),
                          Range);
      if (!R)
        return errorType();
    }
    I->setMethod(R);
  }
  if (!R) {
    for (Expr *A : Args)
      check(argValue(A));
    return Nothing ? nullptr : errorType();
  }
  Identifier Name = I->isBare() ? R->getName() : I->getName();
  if (I->isBare() && Args.empty()) {
    // The enclosing method's parameters, passed on unchanged.
    llvm::SmallVector<Expr *, 4> Implicit;
    for (VarDecl *P : I->getEnclosing()->getParams()) {
      Identifier PN = P->getName();
      PN.Loc = I->getBeginLoc();
      auto *N = Ctx.create<NameRefExpr>(PN);
      N->setDecl(P);
      N->setType(P->getType() ? P->getType() : errorType());
      Used.insert(P);
      Implicit.push_back(N);
    }
    I->setImplicitArgs(Ctx.copyArray<Expr *>(Implicit));
    Name.Loc = I->getBeginLoc();
    checkCallArgs(R, Name, I->getImplicitArgs(), Range);
  } else {
    checkCallArgs(R, Name, orderedArgs(Call, R->getParams(), Name), Range);
  }
  Type *T = R->isFunction() ? R->getResultType() : nullptr;
  if (T)
    I->setType(T);
  return T;
}


/// inherited Name used as a value: a function without arguments.
Type *Checker::visitInheritedExpr(InheritedExpr *E){
  if (E->isBare()) {
    Diags.error(E->getBeginLoc(),
                "expected a method name after 'inherited' in an expression",
                {E->getRange()});
    return errorType();
  }
  bool Nothing;
  RoutineDecl *R = resolveInherited(E, Nothing);
  if (!R)
    return errorType();
  const Identifier &Id = E->getName();
  if (!acceptsArgCount(R->getParams(), 0)) {
    Diags.error(Id.Loc, "missing arguments in call to '" + Id.Spelling + "'",
                {Id.getRange()});
    noteDeclaredHere(R);
    return errorType();
  }
  if (!R->isFunction()) {
    Diags.error(Id.Loc,
                llvm::Twine(R->getKindName()) + " '" + Id.Spelling +
                    "' does not return a value",
                {Id.getRange()});
    return errorType();
  }
  return R->getResultType();
}

