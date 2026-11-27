// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- Sema.cpp - Semantic analysis -------------------------------------===//

#include "Checker.h"

using namespace p3;
using namespace p3::detail;


ClassRefType *Checker::classRefOf(ClassType *C){
  ClassRefType *&R = ClassRefs[C];
  if (!R)
    R = Ctx.create<ClassRefType>(C);
  return R;
}


//===--------------------------------------------------------------------===//
// Standard scope
//===--------------------------------------------------------------------===//

Identifier Checker::predeclaredName(llvm::StringRef N){
  llvm::StringRef Interned = Ctx.getStrings().intern(N);
  return {Interned, Interned, SourceLocation()};
}


void Checker::declareStandardScope(){
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
    // FPC's pointer-sized signed integers (every target is 64-bit).
    Ty("sizeint", Ctx.getInt64Type());
    Ty("ptrint", Ctx.getInt64Type());
    Ty("nativeint", Ctx.getInt64Type());
    Ty("cardinal", Ctx.getCardinalType());
    Ty("longword", Ctx.getCardinalType());
    Ty("dword", Ctx.getCardinalType());
    // Unsigned 64-bit, and FPC's pointer-sized unsigned integers.
    Ty("qword", Ctx.getQWordType());
    Ty("uint64", Ctx.getQWordType());
    Ty("nativeuint", Ctx.getQWordType());
    Ty("ptruint", Ctx.getQWordType());
    Ty("sizeuint", Ctx.getQWordType());
    // FPC's THandle (a file descriptor on Unix): longint.
    Ty("thandle", Ctx.getIntegerType());
    Ty("byte", Ctx.getByteType());
    Ty("word", Ctx.getWordType());
    Ty("shortint", Ctx.getShortIntType());
    Ty("smallint", Ctx.getSmallIntType());
    Ty("double", Ctx.getRealType());
    // pchar: a pointer to char, for C strings (external routines).
    auto *PChar = Ctx.create<PointerType>(Ctx.getCharType());
    PChar->setDeclName("pchar");
    Ty("pchar", PChar);
    PCharTy = PChar;
    // Free Pascal's names for the one-byte char (char is AnsiChar there).
    Ty("ansichar", Ctx.getCharType());
    // A 16-bit character code (FPC's widechar; unicodechar is the same).
    Ty("widechar", Ctx.getWideCharType());
    Ty("unicodechar", Ctx.getWideCharType());
    Ty("pansichar", PChar);
    Ty("ansistring", Ctx.getAnsiStringType());
    Ty("shortstring", Ctx.getShortStringType());
    declareTGUID();
  }

  if (LangOpts.Mode == LangMode::TP)
    declareVarRec();
  if (LangOpts.ObjKeywords) {
    declareTObject();
    declareTMethod();
    declareVarRec();
    declareException();
    declareInterfacedObject();
    declareSharedObject();
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
if ((!isTPOnly(BuiltinKind::ID) || !isISO()) &&                              \
    (!isObjFPCOnly(BuiltinKind::ID) || LangOpts.ObjKeywords))                \
  Standard.insert(NAME, Ctx.create<BuiltinDecl>(predeclaredName(NAME),       \
                                                BuiltinKind::ID, true));
#define BUILTIN_FUNC(ID, NAME)                                                 \
if ((!isTPOnly(BuiltinKind::ID) || !isISO()) &&                              \
    (!isObjFPCOnly(BuiltinKind::ID) || LangOpts.ObjKeywords))                \
  Standard.insert(NAME, Ctx.create<BuiltinDecl>(predeclaredName(NAME),       \
                                                BuiltinKind::ID, false));
#include "p3/AST/Builtins.def"
}


/// Identifier for a predeclared member: lookup name lowercase, spelled
/// as in Free Pascal.
Identifier Checker::memberName(llvm::StringRef Spelling){
  return {Ctx.getStrings().intern(Spelling.lower()),
          Ctx.getStrings().intern(Spelling), SourceLocation()};
}


/// Turbo/FPC builtins, predeclared only with --mode=tp.
bool Checker::isTPOnly(BuiltinKind K){
  switch (K) {
  case BuiltinKind::Break: case BuiltinKind::Continue: case BuiltinKind::Exit:
  case BuiltinKind::Inc: case BuiltinKind::Dec: case BuiltinKind::Delete:
  case BuiltinKind::Insert: case BuiltinKind::SetLength: case BuiltinKind::Slice:
  case BuiltinKind::Length: case BuiltinKind::Copy:
  case BuiltinKind::Pos: case BuiltinKind::Concat: case BuiltinKind::UpCase:
  case BuiltinKind::GetMem: case BuiltinKind::FreeMem:
  case BuiltinKind::ReallocMem: case BuiltinKind::FillChar:
  case BuiltinKind::Move: case BuiltinKind::Halt: case BuiltinKind::SizeOf:
  case BuiltinKind::Low: case BuiltinKind::High: case BuiltinKind::Assigned:
  case BuiltinKind::ParamCount: case BuiltinKind::ParamStr:
  case BuiltinKind::Assign: case BuiltinKind::Close: case BuiltinKind::Flush:
  case BuiltinKind::BlockRead: case BuiltinKind::BlockWrite:
  case BuiltinKind::Seek: case BuiltinKind::FilePos:
  case BuiltinKind::FileSize: case BuiltinKind::GetFileHandle:
    return true;
  case BuiltinKind::PopCnt: case BuiltinKind::CountTrailingZeros:
  case BuiltinKind::CountLeadingZeros: case BuiltinKind::VectorMask:
  case BuiltinKind::VectorLoad: case BuiltinKind::VectorStore:
    return true;
  case BuiltinKind::AssignFile: case BuiltinKind::CloseFile:
  case BuiltinKind::ExceptAddr:
    return true; // (and objfpc only: see isObjFPCOnly)
  default:
    return false;
  }
}


/// Free Pascal's objpas built-ins, predeclared only with --mode=objfpc.
bool Checker::isObjFPCOnly(BuiltinKind K){
  return K == BuiltinKind::AssignFile || K == BuiltinKind::CloseFile ||
         K == BuiltinKind::ExceptAddr || K == BuiltinKind::AddChecked ||
         K == BuiltinKind::SubChecked || K == BuiltinKind::MulChecked ||
         K == BuiltinKind::Wait || K == BuiltinKind::WhenAll ||
         K == BuiltinKind::WhenAny || K == BuiltinKind::IteratorNext ||
         K == BuiltinKind::IteratorValue;
}


//===--------------------------------------------------------------------===//
// Diagnostics helpers
//===--------------------------------------------------------------------===//

/// The visible name closest to Name (edit distance), for "did you mean".
std::string Checker::suggest(llvm::StringRef Name) const{
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
      // (labels; generic routines' keys, F<1>)
      if (Candidate.starts_with("#") || Candidate.ends_with(">") ||
          Candidate == Name)
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


std::string Checker::didYouMean(llvm::StringRef Name) const{
  std::string S = suggest(Name);
  if (S.empty())
    return "";
  // Show the name as it was declared (Point, not point).
  if (Decl *D = Cur->lookup(S); D && !D->getName().Spelling.empty())
    S = D->getName().Spelling.str();
  return "; did you mean '" + S + "'?";
}


void Checker::diagUndeclared(const Identifier &Id){
  // In a record's static method, the record's instance members.
  for (RoutineDecl *R = CurRoutine; R; R = R->getParent())
    if (R->isStatic()) {
      if (Decl *D = R->getClass()->findMember(Id.Name)) {
        Diags.error(Id.Loc,
                    (isInstanceMember(D) ? "instance member '"
                                         : "class method '") +
                        Id.Spelling +
                        "' cannot be used in a static class method",
                    {Id.getRange()});
        noteDeclaredHere(D);
        return;
      }
      break;
    }
  if (isGenericName(Id.Name)) {
    Diags.error(Id.Loc,
                "'" + Id.Spelling + "' is a generic; give its type "
                "arguments: specialize " + Id.Spelling + "<...>",
                {Id.getRange()});
    return;
  }
  Diags.error(Id.Loc,
              "use of undeclared identifier '" + Id.Spelling + "'" +
                  didYouMean(Id.Name),
              {Id.getRange()});
}


/// A use (named Id) of D: warnings for its hint directives (deprecated,
/// experimental, platform, library, unimplemented), as in Free Pascal.
void Checker::warnHints(Decl *D, const Identifier &Id){
  unsigned char H = D ? D->getHints() : 0;
  if (!H || !Diags.isWarningEnabled("deprecated"))
    return;
  std::string Name = "'" + D->getName().Spelling.str() + "'";
  auto Warn = [&](const std::string &What) {
    Diags.warning("deprecated", Id.Loc, Name + What, {Id.getRange()});
  };
  if (H & Decl::HintDeprecated)
    Warn(" is deprecated" + (D->getHintMessage().empty()
                                 ? std::string()
                                 : ": " + D->getHintMessage().str()));
  if (H & Decl::HintExperimental)
    Warn(" is experimental");
  if (H & Decl::HintPlatform)
    Warn(" is not portable (platform-specific)");
  if (H & Decl::HintLibrary)
    Warn(" belongs to a library");
  if (H & Decl::HintUnimplemented)
    Warn(" is not implemented");
  noteDeclaredHere(D);
}


void Checker::noteDeclaredHere(Decl *D){
  if (!D->isPredeclared())
    Diags.note(D->getLocation(),
               "'" + D->getName().Spelling + "' declared here");
}


void Checker::declare(Decl *D){
  const Identifier &Id = D->getName();
  if (Decl *Prev = Cur->insert(Id.Name, D)) {
    Diags.error(Id.Loc, "redefinition of '" + Id.Spelling + "'",
                {Id.getRange()});
    if (!Prev->isPredeclared())
      Diags.note(Prev->getLocation(), "previous definition is here");
  }
}


/// A routine's calling convention in a message (see CallConv).
const char *Checker::callConvDesc(bool CDecl){
  return CDecl ? "'cdecl'" : "'stdcall' (the default)";
}


/// Type-checks E once (later calls return the cached type).
Type *Checker::check(Expr *E){
  bool Allow = AllowUntyped;
  AllowUntyped = false;
  if (Type *Cached = E->getType())
    return Cached;
  Type *T = visitExpr(E);
  if (RoutineDecl *CT = compiletimeRoutine())
    checkCompiletimeExpr(CT, E, T);
  if (T->isUntyped() && !Allow) {
    auto *N = llvm::dyn_cast<NameRefExpr>(E);
    Diags.error(E->getBeginLoc(),
                "'" + (N ? N->getName().Spelling : llvm::StringRef("?")) +
                    "' is an untyped parameter: use a typecast such as "
                    "longint(" +
                    (N ? N->getName().Spelling : llvm::StringRef("x")) +
                    "), its address, or pass it to an untyped parameter",
                {E->getRange()});
    T = errorType();
  }
  E->setType(T);
  if (isManaged(T))
    markUsesManaged();
  return T;
}

Type *Checker::checkAllowingUntyped(Expr *E){
  AllowUntyped = true;
  Type *T = check(E);
  AllowUntyped = false;
  return T;
}


/// The current routine has a try statement (see hasTry).
void Checker::markHasTry(){
  if (!CurRoutine)
    return;
  CurRoutine->setHasTry();
  if (RoutineDecl *C = CurRoutine->getCanonical())
    C->setHasTry();
}


/// The current routine computes a managed value (see usesManaged).
void Checker::markUsesManaged(){
  if (!CurRoutine)
    return;
  CurRoutine->setUsesManaged();
  if (RoutineDecl *C = CurRoutine->getCanonical())
    C->setUsesManaged();
}


void Checker::checkBlock(Block &B){
  declareAll(B);
  finishBlock(B, /*WarnUnused=*/true);
}


void Checker::declareAll(Block &B){
  for (Decl *D : B.getDecls()) {
    if (Diags.shouldStop())
      return;
    if (!llvm::isa<TypeDecl>(D)) {
      resolvePendingPointers();
      checkPendingClasses();
    }
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
  checkPendingClasses();
}


void Checker::finishBlock(Block &B, bool WarnUnused){
  // ISO 7185 6.1.4: every forward declaration needs a definition in the
  // same block.
  for (Decl *D : B.getDecls())
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D);
        R && R->isForward() && isUndefinedHeading(R))
      Diags.error(R->getName().Loc,
                  "'" + R->getName().Spelling +
                      "' is declared forward but never defined",
                  {R->getName().getRange()});
  checkMethodsImplemented(B);
  CompoundStmt *SavedBody = RoutineBody;
  RoutineBody = B.getBody();
  unsigned SavedHandlers = HandlerDepth;
  int SavedFinally = FinallyLoopDepth;
  HandlerDepth = 0;
  FinallyLoopDepth = -1;
  OwnedFlow *SavedFlow = Flow;
  OwnedFlow BlockFlow;
  BlockFlow.S.Valid = LiveBit;
  Flow = &BlockFlow;
  for (Decl *D : B.getDecls())
    if (auto *V = llvm::dyn_cast<VarDecl>(D); V && V->isOwned())
      ownedDeclare(V);
  bool SavedNullSafe = NullSafeHere;
  bool SavedNilPointers = NilPointersHere;
  NullSafeHere = B.isNullSafe() && LangOpts.ObjKeywords;
  NilPointersHere = B.isNilPointers() && LangOpts.Mode != LangMode::ISO;
  if (anyNullSafety())
    nullTrackBlock(B);
  std::vector<Decl *> *SavedInline = InlineVars;
  std::vector<Decl *> BlockInline;
  InlineVars = &BlockInline;
  visitStmt(B.getBody());
  if (CurRoutine && CurRoutine->getBody() == &B)
    nullCheckEnd(B.getBody()->getRange().getEnd().getLocWithOffset(-3),
                 /*AtExit=*/false);
  NullSafeHere = SavedNullSafe;
  NilPointersHere = SavedNilPointers;
  InlineVars = SavedInline;
  Flow = SavedFlow;
  if (!BlockInline.empty()) { // they are the block's too, after the others
    BlockInline.insert(BlockInline.begin(), B.getDecls().begin(),
                       B.getDecls().end());
    B.setDecls(Ctx.copyArray<Decl *>(BlockInline));
  }
  HandlerDepth = SavedHandlers;
  FinallyLoopDepth = SavedFinally;
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


void Checker::checkGotos(){
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


void Checker::checkProgramParams(ProgramDecl &P){
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
void Checker::checkProgramParamDecls(ProgramDecl &P){
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

Checker::Checker(ASTContext &Ctx, DiagnosticsEngine &Diags, const LangOptions &LO,
        Sema::ReparseFn Reparse,
        Sema::ReparseTypeFn ReparseType,
        const CheckDirectives *Directives)
    : Ctx(Ctx), Diags(Diags), LangOpts(LO), Directives(Directives),
      Reparse(std::move(Reparse)), ReparseType(std::move(ReparseType)) {
  declareStandardScope();
}


std::vector<std::string> Checker::standardNames() const{
  std::vector<std::string> Names;
  Standard.forEachName([&](llvm::StringRef N) { Names.push_back(N.str()); });
  return Names;
}


/// Makes the interfaces of the units in Uses visible in S.
void Checker::importUnits(Scope &S, llvm::ArrayRef<Identifier> Uses){
  for (const Identifier &U : Uses) {
    if (Scope *I = UnitInterfaces.lookup(U.Name))
      S.addImport(I);
    // Missing units are reported by the driver.
  }
}


/// Checks unit U. InterfaceOnly: U was compiled separately (S11), so
/// only its interface is declared, for the units and program using it.
void Checker::checkUnit(UnitDecl &U, bool InterfaceOnly){
  CurModule = &U;
  bool SavedInterfaceOnly = CheckingInterfaceOnly;
  CheckingInterfaceOnly = InterfaceOnly;
  auto Iface = std::make_unique<Scope>(&Standard);
  importUnits(*Iface, U.getInterfaceUses());
  Cur = Iface.get();
  declareAll(*U.getInterface());
  if (InterfaceOnly) {
    for (Decl *D : U.getInterface()->getDecls())
      Used.insert(D);
    // The definitions of its generic routines, which the units and the
    // program using it specialize; they see the unit's interface only.
    auto Impl = std::make_unique<Scope>(Iface.get());
    importUnits(*Impl, U.getImplementationUses());
    Cur = Impl.get();
    CurUnitInterface = Iface.get();
    for (Decl *D : U.getImplementation()->getDecls())
      if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isGeneric())
        declareRoutine(R);
    CurUnitInterface = nullptr;
    CheckingInterfaceOnly = SavedInterfaceOnly;
    UnitInterfaces[U.getName().Name] = Iface.get();
    UnitScopes.push_back(std::move(Iface));
    UnitScopes.push_back(std::move(Impl));
    Cur = &Standard;
    return;
  }
  CheckingInterfaceOnly = SavedInterfaceOnly;

  auto Impl = std::make_unique<Scope>(Iface.get());
  importUnits(*Impl, U.getImplementationUses());
  Cur = Impl.get();
  CurUnitInterface = Iface.get();
  declareAll(*U.getImplementation());
  for (Decl *D : U.getInterface()->getDecls())
    if (auto *R = llvm::dyn_cast<RoutineDecl>(D);
        R && isUndefinedHeading(R) && !R->isExternal())
      Diags.error(R->getName().Loc,
                  "'" + R->getName().Spelling +
                      "' is declared in the interface of unit '" +
                      U.getName().Spelling + "' but not implemented",
                  {R->getName().getRange()});
  checkMethodsImplemented(*U.getInterface());
  finishBlock(*U.getImplementation(), /*WarnUnused=*/true);
  checkInstances(*U.getImplementation());
  CurUnitInterface = nullptr;

  // Interface variables may be used by other units: never "unused".
  for (Decl *D : U.getInterface()->getDecls())
    Used.insert(D);
  UnitInterfaces[U.getName().Name] = Iface.get();
  UnitScopes.push_back(std::move(Iface));
  UnitScopes.push_back(std::move(Impl));
  Cur = &Standard;
}


void Checker::checkProgram(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units,
                  bool SeparateUnits){
  for (UnitDecl *U : Units)
    checkUnit(*U, SeparateUnits);
  checkProgramParams(P);
  CurModule = &P;
  Scope ProgramScope(&Standard);
  importUnits(ProgramScope, P.getUses());
  Cur = &ProgramScope;
  checkBlock(*P.getBlock());
  checkInstances(*P.getBlock());
  checkProgramParamDecls(P);
  checkGotos();
  Cur = &Standard;
}


/// A unit compiled on its own: the units it uses were compiled separately.
void Checker::checkSeparateUnit(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps){
  for (UnitDecl *D : Deps)
    checkUnit(*D, /*InterfaceOnly=*/true);
  checkUnit(U);
  checkGotos();
}


bool Sema::check(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units,
                 bool SeparateUnits) {
  unsigned ErrorsBefore = Diags.getNumErrors();
  Checker(Ctx, Diags, LangOpts, Reparse, ReparseType, Directives)
      .checkProgram(P, Units, SeparateUnits);
  return Diags.getNumErrors() == ErrorsBefore;
}

std::vector<std::string> Sema::standardNames() {
  return Checker(Ctx, Diags, LangOpts).standardNames();
}

bool Sema::checkUnit(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps) {
  unsigned ErrorsBefore = Diags.getNumErrors();
  Checker C(Ctx, Diags, LangOpts, Reparse, ReparseType, Directives);
  C.checkSeparateUnit(U, Deps);
  return Diags.getNumErrors() == ErrorsBefore;
}
