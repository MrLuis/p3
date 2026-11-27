// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- SemaStmt.cpp - Semantic analysis -------------------------------------===//

#include "Checker.h"

using namespace p3;
using namespace p3::detail;


void Checker::visitStmt(Stmt *S){
  StmtVisitor<Checker>::visitStmt(S);
  RoutineDecl *CT = compiletimeRoutine();
  if (!CT)
    return;
  if (auto *P = llvm::dyn_cast<ProcCallStmt>(S))
    checkCompiletimeUse(CT, P->getDecl(), P->getCallee().Loc,
                        P->getCallee().getRange());
  else if (llvm::isa<IndirectCallStmt>(S))
    compiletimeError(CT, S->getBeginLoc(), S->getRange(),
                     "call methods or procedural values");
  else if (llvm::isa<RaiseStmt>(S))
    compiletimeError(CT, S->getBeginLoc(), S->getRange(),
                     "raise exceptions");
}


//===--------------------------------------------------------------------===//
// Statement helpers
//===--------------------------------------------------------------------===//

void Checker::checkCondition(Expr *Cond, llvm::StringRef Keyword){
  Type *T = check(Cond);
  if (!T->isError() && !T->getBase()->isBoolean())
    Diags.error(Cond->getBeginLoc(),
                "condition of '" + Keyword + "' must be of type 'boolean', "
                "not " + quoted(T),
                {Cond->getRange()});
}


/// Reports an assignment (or read / var argument) to the control variable
/// of an enclosing for statement. Returns true if one was reported.
bool Checker::checkNotForControlVar(Expr *Target){
  auto *N = llvm::dyn_cast<NameRefExpr>(Target);
  if (!N || !N->getDecl())
    return false;
  for (const ActiveFor &F : ActiveFors) {
    if (F.Var != N->getDecl())
      continue;
    Diags.error(N->getBeginLoc(),
                "cannot assign to '" + N->getName().Spelling +
                    "' inside a 'for' loop that it controls",
                {N->getRange()});
    Diags.note(F.Loc, "'for' loop is here");
    return true;
  }
  return false;
}


/// The order of label value V of a selector of type SelTy: qword's
/// values compare unsigned.
int64_t Checker::labelKey(int64_t V, Type *SelTy){
  return isQWordValue(SelTy) ? V ^ INT64_MIN : V;
}


/// Checks one case or variant label against the selector/tag type.
bool Checker::checkLabel(CaseLabel *L, Type *SelTy, llvm::StringRef What){
  bool OK = true;
  Type *LoTy = nullptr, *HiTy = nullptr;
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
    (E == L->Lo ? LoTy : HiTy) = C->Ty;
  }
  if (!L->Hi) {
    L->HiValue = L->LoValue;
    HiTy = LoTy;
  }
  if (OK && labelKey(L->LoValue, SelTy) > labelKey(L->HiValue, SelTy)) {
    Diags.error(L->Lo->getBeginLoc(), "case label range is empty",
                {L->getRange()});
    OK = false;
  }
  if (OK && !SelTy->isError()) {
    if (!constantInRange(L->LoValue, LoTy, SelTy) ||
        !constantInRange(L->HiValue, HiTy, SelTy)) {
      Diags.error(L->Lo->getBeginLoc(),
                  What + " is outside the range of type " + quoted(SelTy),
                  {L->getRange()});
      OK = false;
    }
  }
  return OK;
}


//===--------------------------------------------------------------------===//
// Statements
//===--------------------------------------------------------------------===//

void Checker::visitEmptyStmt(EmptyStmt *){}


void Checker::visitSequence(const void *Seq, llvm::ArrayRef<Stmt *> Body){
  Containers.push_back(Seq);
  std::unique_ptr<Scope> Declared; // of the variables declared in it
  Scope *SavedScope = Cur;
  for (Stmt *Child : Body) {
    if (Diags.shouldStop())
      break;
    DirectSequence = Seq;
    SeqScope = &Declared;
    visitStmt(Child);
  }
  DirectSequence = nullptr;
  SeqScope = nullptr;
  if (Declared)
    Cur = SavedScope;
  Containers.pop_back();
}


/// The type of a variable declared without one, from its value's type T.
/// Is E a quoted character ('a')? Where a type is inferred it is a
/// string, like any quoted text.
bool Checker::isCharLiteral(Expr *E){
  while (auto *P = llvm::dyn_cast<ParenExpr>(E))
    E = P->getInner();
  auto *L = llvm::dyn_cast<StringLiteralExpr>(E);
  return L && L->isCharLiteral();
}


/// The type that values of types A and B have in common where one type
/// is inferred for both (the elements of [a, b], the arguments for one
/// type parameter), or null: the same type; the wider integer type; real
/// for a real and an integer (nothing is lost); Str for strings and
/// characters; else the one the other can be assigned to.
Type *Checker::commonType(Type *A, Type *B, Type *Str){
  if (A == B)
    return A;
  bool IA = llvm::isa<IntegerType>(A->getBase());
  bool IB = llvm::isa<IntegerType>(B->getBase());
  if (IA && IB) {
    Type *PA = promoteInteger(A), *PB = promoteInteger(B);
    return PA == PB ? PA : Ctx.getInt64Type();
  }
  if ((A->isReal() && (IB || B->isReal())) || (B->isReal() && IA))
    return Ctx.getRealType();
  if (A->isStringish() && B->isStringish())
    return Str;
  if (isAssignable(A, B))
    return A;
  if (isAssignable(B, A))
    return B;
  return nullptr;
}


/// Can a variable (an element, a type argument) have the type T of a
/// value, when it is given no type of its own?
bool Checker::isInferable(Type *T){
  auto *ST = llvm::dyn_cast<SetType>(T);
  return !(T->isNil() || T->isUntyped() || T->isOpenArray() ||
           T->isArrayOfConst() || containsFile(T) ||
           (ST && ST->isEmptySet()));
}


/// var x := [a, b, c]: the dynamic array type of the value, an array
/// written out. Null if Init is not one: [], a list with a range, or one
/// whose elements are all enumeration values, characters or booleans (a
/// set, as ever). The error type after a diagnostic.
Type *Checker::inferArrayLiteral(Expr *Init, bool LongStr, const Identifier &Name){
  auto *SC = llvm::dyn_cast<SetCtorExpr>(Init);
  if (!SC || SC->getElements().empty())
    return nullptr;
  for (const SetElement &El : SC->getElements())
    if (El.Hi)
      return nullptr;
  Type *Str = LongStr ? static_cast<Type *>(Ctx.getAnsiStringType())
                      : Ctx.getShortStringType(255);
  Type *Elem = nullptr;
  bool OK = true, SetLike = true;
  for (const SetElement &El : SC->getElements()) {
    Type *T = check(El.Lo);
    if (T->isError()) {
      OK = false;
      continue;
    }
    SetLike &= T->isOrdinal() && !llvm::isa<IntegerType>(T->getBase());
    if (T->isStringLiteral())
      T = Str;
    if (!isInferable(T)) {
      Diags.error(El.Lo->getBeginLoc(),
                  "cannot tell the element type of the array '" +
                      Name.Spelling + "' from " +
                      (T->isNil() ? std::string("nil")
                                  : "a value of type " + quoted(T)) +
                      "; give the variable a type",
                  {El.Lo->getRange()});
      OK = false;
      continue;
    }
    Type *Joined = Elem ? commonType(Elem, T, Str) : T;
    if (!Joined) {
      Diags.error(El.Lo->getBeginLoc(),
                  "the elements of the array '" + Name.Spelling +
                      "' have no type in common: " + quoted(Elem) + " and " +
                      quoted(T) + "; give the variable a type",
                  {El.Lo->getRange()});
      OK = false;
      continue;
    }
    Elem = Joined;
  }
  if (!OK)
    return errorType();
  if (SetLike)
    return nullptr;
  return Ctx.getDynArrayType(Elem);
}


Type *Checker::inferVarType(VarDeclStmt *S, VarDecl *V, Type *T){
  if (T->isError())
    return T;
  if (T->isStringLiteral() || isCharLiteral(S->getInit()))
    return S->hasLongStrings() ? Ctx.getAnsiStringType()
                               : Ctx.getShortStringType(255);
  auto *ST = llvm::dyn_cast<SetType>(T);
  if (T->isNil() || T->isUntyped() || T->isOpenArray() ||
      T->isArrayOfConst() || T->isFile() || (ST && ST->isEmptySet())) {
    Diags.error(S->getInit()->getBeginLoc(),
                "cannot tell the type of '" + V->getName().Spelling +
                    "' from " +
                    (T->isNil() ? std::string("nil")
                     : ST       ? std::string("an empty set")
                                : "a value of type " + quoted(T)) +
                    "; give the variable a type",
                {S->getInit()->getRange()});
    return errorType();
  }
  return T;
}


/// var x: T;  var x: T := e;  var x := e;  -- among the statements of a
/// sequence, to whose end the variables are visible.
void Checker::visitVarDeclStmt(VarDeclStmt *S){
  const void *Seq = DirectSequence;
  std::unique_ptr<Scope> *Declared = SeqScope;
  DirectSequence = nullptr;
  Expr *Init = S->getInit();
  VarDecl *First = S->getVars()[0];
  if (!Seq || !Declared) {
    Diags.error(S->getBeginLoc(),
                "a variable can only be declared in a statement sequence "
                "(between 'begin' and 'end')",
                {S->getRange()});
    if (Init)
      check(Init);
    return;
  }
  // The value is checked first: it cannot use the variable.
  Type *T;
  Type *ValueTy = nullptr;
  if (First->getTypeSpec()) {
    T = resolveType(First->getTypeSpec());
    if (Init)
      ValueTy = checkValueFor(T, Init);
  } else if (Type *AT = inferArrayLiteral(Init, S->hasLongStrings(),
                                          First->getName())) {
    T = AT; // var a := [1, 2, 3]: a dynamic array
    ValueTy = AT->isError() ? AT : checkValueFor(AT, Init);
  } else {
    ValueTy = check(Init);
    T = inferVarType(S, First, ValueTy);
  }
  if (!*Declared) {
    *Declared = std::make_unique<Scope>(Cur);
    Cur = Declared->get();
  }
  for (VarDecl *V : S->getVars()) {
    V->setType(T);
    V->setOwner(CurRoutine);
    checkOwnedDecl(V);
    declare(V);
    if (InlineVars)
      InlineVars->push_back(V);
    if (V->isOwned())
      ownedDeclare(V);
    if (isNullTrackedType(T)) // (nil until assigned)
      nullTrack(V, true, false);
  }
  if (!Init)
    return;
  Identifier Id = First->getName();
  auto *Target = Ctx.create<NameRefExpr>(Id);
  Target->setDecl(First);
  Target->setType(T);
  Used.insert(First); // (as an assignment does)
  auto *Assign = Ctx.create<AssignStmt>(
      SourceRange(Id.Loc, Init->getRange().getEnd()), Target,
      S->getAssignLoc(), Init);
  S->setInitAssign(Assign);
  finishAssign(Assign, T, ValueTy);
  if (S->isConst()) // from here on
    First->setReadOnly();
}


/// (a, b) := e;  var (a, b) := e;  -- e, a tuple, is kept in a hidden
/// variable (a tuple written out is built in one), then each target is
/// assigned its element, with the checks of an assignment.
void Checker::visitDestructureStmt(DestructureStmt *S){
  const void *Seq = DirectSequence;
  std::unique_ptr<Scope> *Declared = SeqScope;
  DirectSequence = nullptr;
  Expr *Value = S->getValue();
  bool IsVar = !S->getVars().empty();
  size_t N = IsVar ? S->getVars().size() : S->getTargets().size();
  auto Fail = [&] {
    if (IsVar)
      return;
    for (Expr *T : S->getTargets())
      check(T);
  };
  if (IsVar && (!Seq || !Declared)) {
    Diags.error(S->getBeginLoc(),
                "a variable can only be declared in a statement sequence "
                "(between 'begin' and 'end')",
                {S->getRange()});
    check(Value);
    return;
  }
  // (a, b) := ('x', 1): the targets have types, so 'x' stays a char,
  // which a char and a string can take.
  KeepCharLiterals = !IsVar;
  Type *VT = check(Value);
  KeepCharLiterals = false;
  auto *RT = llvm::dyn_cast<RecordType>(VT);
  if (!VT->isError() &&
      (!RT || !RT->isTuple() || RT->getAllFields().size() != N)) {
    Diags.error(Value->getBeginLoc(),
                std::to_string(N) + " variables cannot take the elements of " +
                    (RT && RT->isTuple() ? "the tuple " : "a value of type ") +
                    quoted(VT),
                {Value->getRange()});
    VT = errorType();
  }
  if (IsVar) { // declared also after an error, with the error type
    if (!*Declared) {
      *Declared = std::make_unique<Scope>(Cur);
      Cur = Declared->get();
    }
    llvm::SmallVector<Expr *, 4> Targets;
    for (size_t I = 0; I < N; ++I) {
      VarDecl *V = S->getVars()[I];
      Type *T = VT->isError() ? VT : RT->getAllFields()[I]->getType();
      V->setType(T);
      V->setOwner(CurRoutine);
      declare(V);
      if (InlineVars)
        InlineVars->push_back(V);
      auto *Ref = Ctx.create<NameRefExpr>(V->getName());
      Targets.push_back(Ref);
    }
    S->setTargets(Ctx.copyArray<Expr *>(Targets));
  }
  if (VT->isError())
    return Fail();
  // The variable the elements are read from.
  VarDecl *Temp;
  if (auto *TE = llvm::dyn_cast<TupleExpr>(Value)) {
    Temp = TE->getTemp();
  } else {
    Temp = makeHiddenVar(VT, Value->getBeginLoc());
    if (!Temp) // (not among the statements of a block: unreachable)
      return Fail();
    auto *Ref = Ctx.create<NameRefExpr>(Temp->getName());
    Ref->setDecl(Temp);
    Ref->setType(VT);
    S->setInit(Ctx.create<AssignStmt>(Value->getRange(), Ref,
                                      S->getAssignLoc(), Value));
  }
  llvm::SmallVector<AssignStmt *, 4> Assigns;
  for (size_t I = 0; I < N; ++I) {
    FieldDecl *F = RT->getAllFields()[I];
    Expr *Target = S->getTargets()[I];
    Identifier TempId = Temp->getName();
    TempId.Loc = Value->getBeginLoc();
    auto *Ref = Ctx.create<NameRefExpr>(TempId);
    Ref->setDecl(Temp);
    Ref->setType(VT);
    Identifier FieldId = F->getName();
    FieldId.Loc = Value->getBeginLoc();
    auto *Elem = Ctx.create<FieldExpr>(Value->getRange(), Ref, FieldId);
    Elem->setMember(F);
    Elem->setType(F->getType());
    auto *A = Ctx.create<AssignStmt>(
        SourceRange(Target->getBeginLoc(), Value->getRange().getEnd()),
        Target, S->getAssignLoc(), Elem);
    visitAssignStmt(A);
    Assigns.push_back(A);
  }
  S->setAssigns(Ctx.copyArray<AssignStmt *>(Assigns));
}


void Checker::visitLabeledStmt(LabeledStmt *S){
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


void Checker::visitGotoStmt(GotoStmt *S){
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


/// The procedural type of the field or property D, or null.
RoutineType *Checker::proceduralMember(Decl *D){
  Type *T = nullptr;
  if (auto *F = llvm::dyn_cast<FieldDecl>(D))
    T = F->getType();
  else if (auto *P = llvm::dyn_cast<PropertyDecl>(D))
    T = P->getType();
  return llvm::dyn_cast_or_null<RoutineType>(T);
}


void Checker::visitProcCallStmt(ProcCallStmt *S){
  const Identifier &Callee = S->getCallee();
  WithContext *W = nullptr;
  Decl *D = S->getTypeArgs().empty() ? Cur->lookup(Callee.Name, &W)
                                     : specialize(Callee, S->getTypeArgs(), &W);
  noteLambdaUse(D, W, Callee);
  bool Generic = false; // F(args) for a generic F: its types inferred
  if (!D && S->getTypeArgs().empty())
    D = inferSpecialization(Callee, S->getArgs(), nullptr, Generic, &W);
  if (!D) {
    if (S->getTypeArgs().empty() && !Generic) {
      diagUndeclared(Callee);
      return;
    }
    for (Expr *Arg : S->getArgs())
      check(argValue(Arg));
    return;
  }
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D)) { // objfpc: overloads
    D = resolveOverload(R, S->getArgs(), Callee, S->getRange());
    if (!D)
      return;
  }
  S->setDecl(D);
  warnHints(D, Callee);
  if (hasMembers(W)) { // a method of an object or advanced record
    S->setWith(W);
    checkScopeAccess(D, W, Callee);
  }

  if (auto *BD = llvm::dyn_cast<BuiltinDecl>(D);
      BD && BD->getBuiltinKind() == BuiltinKind::Wait) {
    checkWait(S->getArgs(), Callee, S->getRange(), /*NeedValue=*/false);
    return;
  }
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
    case BuiltinKind::Close: case BuiltinKind::CloseFile:
    case BuiltinKind::Flush:
      return checkFileProc(S, BD->getBuiltinKind());
    case BuiltinKind::Assign: case BuiltinKind::AssignFile:
      return checkAssignFile(S);
    case BuiltinKind::BlockRead: case BuiltinKind::BlockWrite:
      return checkBlockIO(S);
    case BuiltinKind::Seek:
      return checkSeek(S);
    case BuiltinKind::Break: case BuiltinKind::Continue:
      if (!S->getArgs().empty())
        Diags.error(Callee.Loc, "'" + Callee.Spelling + "' takes no arguments",
                    {S->getRange()});
      else if (!LoopDepth)
        Diags.error(Callee.Loc,
                    "'" + Callee.Spelling + "' is only allowed inside a loop",
                    {Callee.getRange()});
      else if (FinallyLoopDepth >= 0 &&
               LoopDepth == static_cast<unsigned>(FinallyLoopDepth))
        Diags.error(Callee.Loc,
                    "'" + Callee.Spelling +
                        "' cannot leave a 'finally' part",
                    {Callee.getRange()});
      else
        ownedBreak(BD->getBuiltinKind() == BuiltinKind::Continue);
      return;
    case BuiltinKind::Exit: {
      auto Args = S->getArgs();
      if (FinallyLoopDepth >= 0)
        Diags.error(Callee.Loc,
                    "'" + Callee.Spelling +
                        "' is not allowed in a 'finally' part",
                    {Callee.getRange()});
      if (Args.empty()) {
        nullCheckEnd(Callee.Loc, /*AtExit=*/true);
        ownedDead();
        return;
      }
      // exit(value) (Turbo/FPC): sets the function result, then exits.
      if (Args.size() > 1 || !CurRoutine || !CurRoutine->isFunction()) {
        Diags.error(Callee.Loc,
                    Args.size() > 1
                        ? "'exit' takes at most one argument"
                        : "'exit' with a value is only allowed in a function",
                    {S->getRange()});
        for (Expr *A : Args)
          check(A);
        ownedDead();
        return;
      }
      // (a lambda whose result type is not known yet: exit(value) gives it)
      if (CurRoutine->isLambda() && !CurRoutine->getResultType())
        inferLambdaResult(Args[0]);
      Type *T = llvm::isa<TupleExpr>(Args[0]) || llvm::isa<CallExpr>(Args[0])
                    ? checkValueFor(CurRoutine->getResultType(), Args[0])
                    : check(Args[0]);
      if (VarDecl *OV = ownedVarOf(Args[0]))
        diagOwnedEscape(OV, Args[0], /*Returned=*/true);
      checkLambdaEscape(Args[0], /*Local=*/false);
      if (anyNullSafety() && !T->isError())
        if (VarDecl *Res = CurRoutine->getResultVar()) {
          nullAssign(nullptr, Res, Args[0]);
          nullCheckEnd(Callee.Loc, /*AtExit=*/true);
        }
      ownedDead();
      Type *RT = CurRoutine->getResultType();
      ResultAssigned.insert(CurRoutine);
      if (!T->isError() && !RT->isError() && !isAssignable(RT, T) &&
          !convertImplicitly(Args[0], RT))
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
    case BuiltinKind::SetLength:
      return checkSetLength(S);
    case BuiltinKind::Pack: case BuiltinKind::Unpack:
      return checkPackUnpack(S, BD->getBuiltinKind() == BuiltinKind::Pack);
    case BuiltinKind::GetMem: case BuiltinKind::FreeMem:
    case BuiltinKind::ReallocMem: case BuiltinKind::FillChar:
    case BuiltinKind::Move: case BuiltinKind::Halt:
      return checkMemoryProc(S, BD->getBuiltinKind());
    case BuiltinKind::VectorStore:
      return checkVectorStore(S);
    default: llvm_unreachable("not a builtin procedure");
    }
  }
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isAsync()) {
    checkCallArgs(R, Callee, orderedArgs(S, R->getParams(), Callee),
                  S->getRange());
    warnDanglingTask(Callee, S->getRange());
    return;
  }
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isIterator()) {
    checkCallArgs(R, Callee, orderedArgs(S, R->getParams(), Callee),
                  S->getRange());
    warnUnusedIterator(Callee, S->getRange());
    return;
  }
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D);
      R && (R->isProcedure() || !isISO())) { // TP: discard a function result
    checkCallArgs(R, Callee, orderedArgs(S, R->getParams(), Callee),
                  S->getRange());
    // A constructor's call of another of its class (on Self): that one
    // sets the fields.
    if (NullSafeHere && R->isConstructor() && CurRoutine &&
        CurRoutine->isConstructor() && Flow)
      for (unsigned I = 0; I < Flow->N; ++I)
        if (Flow->Null[I] && llvm::isa<FieldDecl>(Flow->Vars[I]))
          nullSet(I, false, true);
    return;
  }
  if (auto *V = llvm::dyn_cast<VarDecl>(D))
    if (auto *RT = llvm::dyn_cast_or_null<RoutineType>(V->getType());
        RT && !RT->isFunction()) {
      Used.insert(V); // (called: used)
      return checkCallArgs(RT->getParams(), V, Callee,
                           orderedArgs(S, RT->getParams(), Callee),
                           S->getRange());
    }
  if (RoutineType *RT = proceduralMember(D)) {
    // FOnEvent(x) in a method or a with statement: through the member
    auto *Ref = Ctx.create<NameRefExpr>(Callee);
    if (check(Ref)->isError())
      return;
    S->setCalleeValue(Ref);
    return checkCallArgs(RT->getParams(), nullptr, Callee,
                         orderedArgs(S, RT->getParams(), Callee),
                         S->getRange());
  }

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
Type *Checker::checkNameTarget(NameRefExpr *Target){
  const Identifier &Id = Target->getName();
  WithContext *W = nullptr;
  Decl *D = Cur->lookup(Id.Name, &W);
  // (a function's name: its result, noted below)
  auto *FR = llvm::dyn_cast_or_null<RoutineDecl>(D);
  if (!FR || !FR->isFunction())
    noteLambdaUse(D, W, Id);
  if (!D) {
    diagUndeclared(Id);
    return errorType();
  }
  checkScopeAccess(D, W, Id);

  if (auto *R = llvm::dyn_cast<RoutineDecl>(D); R && R->isFunction()) {
    for (RoutineDecl *Enc = CurRoutine; Enc; Enc = Enc->getParent())
      for (RoutineDecl *X = R; X; X = X->getNextOverload()) // (overloads)
        if (Enc == X) {
          if (X != CurRoutine) // from a nested routine
            X->getResultVar()->setCaptured();
          noteLambdaUse(X->getResultVar(), nullptr, Id);
          Target->setDecl(X->getResultVar());
          ResultAssigned.insert(X);
          return X->getResultType();
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
  warnHints(D, Id);
  if (RoutineDecl *CT = compiletimeRoutine())
    checkCompiletimeUse(CT, D, Id.Loc, Id.getRange(), /*Write=*/true);
  if (auto *V = llvm::dyn_cast<VarDecl>(D)) {
    checkNotForControlVar(Target);
    if (V->getVarKind() == VarDecl::Result) // objfpc `Result`
      ResultAssigned.insert(V->getOwner());
    return V->getType() ? V->getType() : errorType();
  }
  if (auto *F = llvm::dyn_cast<FieldDecl>(D))
    return F->getType() ? F->getType() : errorType();
  if (auto *P = llvm::dyn_cast<PropertyDecl>(D)) {
    if (!checkArrayPropertyUse(P, nullptr, Id))
      return errorType();
    return checkPropertyWrite(P, Id);
  }

  Diags.error(Id.Loc,
              "cannot assign to " + llvm::Twine(D->getKindName()) + " '" +
                  Id.Spelling + "'",
              {Id.getRange()});
  noteDeclaredHere(D);
  return errorType();
}


void Checker::visitIndirectCallStmt(IndirectCallStmt *S){
  DirectSequence = nullptr;
  IndirectCallExpr *Call = S->getCall();
  if (isMethodCallee(Call->getCallee())) {
    Type *T = checkMethodCall(Call);
    if (T)
      Call->setType(T);
    if (T && T->isTask() && isAsyncCall(Call))
      if (auto *F = llvm::dyn_cast<FieldExpr>(Call->getCallee()))
        warnDanglingTask(F->getField(), Call->getRange());
    if (T && T->isIterator())
      if (auto *F = llvm::dyn_cast<FieldExpr>(Call->getCallee()))
        warnUnusedIterator(F->getField(), Call->getRange());
    return;
  }
  if (RoutineType *RT = checkIndirectCallee(Call))
    Call->setType(RT->isFunction() ? RT->getResult() : RT);
  else
    Call->setType(errorType());
}


/// Type of a property assigned to; it needs a write accessor.
Type *Checker::checkPropertyWrite(PropertyDecl *P, const Identifier &Id){
  if (ClassType *H =
          helperOfMember(P->getWrite() ? P->getWrite() : P->getRead())) {
    Diags.error(Id.Loc,
                "writing a property of a " + H->helperKindName() +
                    " is not supported yet",
                {Id.getRange()});
    return errorType();
  }
  if (P->getWrite() || P->getType()->isError() ||
      (!P->getRead() && P->getWriteName().isValid()))
    return P->getType();
  Diags.error(Id.Loc, "property '" + Id.Spelling + "' is read-only",
              {Id.getRange()});
  noteDeclaredHere(P);
  return errorType();
}


/// Is the target of an assignment a property (written by its accessor)?
PropertyDecl *Checker::assignedProperty(Expr *Target){
  if (auto *N = llvm::dyn_cast<NameRefExpr>(Target))
    return llvm::dyn_cast_or_null<PropertyDecl>(N->getDecl());
  if (auto *F = llvm::dyn_cast<FieldExpr>(Target))
    return llvm::dyn_cast_or_null<PropertyDecl>(F->getMember());
  if (auto *I = llvm::dyn_cast<IndexExpr>(Target))
    return I->getProperty();
  return nullptr;
}


/// The name an array property is used by (obj.P / P), or for a default
/// property, its name at the indexed object.
Identifier Checker::propertyName(IndexExpr *E){
  Expr *Root = E->getPropertyRoot();
  if (auto *F = llvm::dyn_cast<FieldExpr>(Root); F && F->getMember() == E->getProperty())
    return F->getField();
  if (auto *N = llvm::dyn_cast<NameRefExpr>(Root); N && N->getDecl() == E->getProperty())
    return N->getName();
  Identifier Id = E->getProperty()->getName();
  Id.Loc = Root->getBeginLoc();
  return Id;
}


/// An array property used without its indexes (outside IndexBase).
bool Checker::checkArrayPropertyUse(PropertyDecl *P, Expr *E, const Identifier &Id){
  if (!P->isArrayProperty())
    return true;
  if (E && E == IndexBase) {
    PendingIndexes[E] = {P, E, {}};
    return true;
  }
  Diags.error(Id.Loc,
              "array property '" + Id.Spelling + "' needs " +
                  (P->getParams().size() == 1
                       ? std::string("an index")
                       : std::to_string(P->getParams().size()) + " indexes"),
              {Id.getRange()});
  noteDeclaredHere(P);
  return false;
}


/// Value, the first value a lambda's body gives its result while its
/// result type is not known: that type (a quoted constant: a string).
void Checker::inferLambdaResult(Expr *Value){
  Type *T = check(Value);
  if (!T->isError() && (T->isStringLiteral() || isCharLiteral(Value)))
    T = Ctx.getAnsiStringType();
  if (!T->isError() && !isInferable(T)) {
    Diags.error(Value->getBeginLoc(),
                "cannot tell the result type of this lambda from " +
                    (T->isNil() ? std::string("nil")
                                : "a value of type " + quoted(T)) +
                    "; give it one: function(...): T => ...",
                {Value->getRange()});
    T = errorType();
  }
  CurRoutine->setResultType(T);
  CurRoutine->getResultVar()->setType(T);
}


void Checker::visitAssignStmt(AssignStmt *S){
  Expr *Target = S->getTarget();
  // A lambda's body, $lambda := e (or, in a block body, the first
  // Result := e), where no result type is known: the type of e (a quoted
  // constant: a string).
  if (CurRoutine && CurRoutine->isLambda() && CurRoutine->isFunction() &&
      !CurRoutine->getResultType())
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Target);
        N && (N->getName().Name == CurRoutine->getName().Name ||
              (N->getName().Name == "result" && CurRoutine->hasResultName() &&
               Cur->lookup("result") == CurRoutine->getResultVar())))
      inferLambdaResult(S->getValue());
  Type *TargetTy;
  if (auto *N = llvm::dyn_cast<NameRefExpr>(Target)) {
    TargetTy = checkNameTarget(N);
    N->setType(TargetTy);
  } else {
    Expr *SavedTarget = CurAssignTarget;
    CurAssignTarget = Target;
    TargetTy = check(Target);
    CurAssignTarget = SavedTarget;
    // r.f := v / Result.f := v: assigns the result (a record) in part
    Expr *Root = Target;
    while (true) {
      if (auto *FE = llvm::dyn_cast<FieldExpr>(Root))
        Root = FE->getBase();
      else if (auto *IE = llvm::dyn_cast<IndexExpr>(Root))
        Root = IE->getBase();
      else
        break;
    }
    if (auto *RN = llvm::dyn_cast<NameRefExpr>(Root))
      if (auto *V = llvm::dyn_cast_or_null<VarDecl>(RN->getDecl());
          V && V->getVarKind() == VarDecl::Result)
        ResultAssigned.insert(V->getOwner());
    if (auto *P = assignedProperty(Target); P && !TargetTy->isError()) {
      auto *I = llvm::dyn_cast<IndexExpr>(Target);
      TargetTy = checkPropertyWrite(
          P, I ? propertyName(I) : llvm::cast<FieldExpr>(Target)->getField());
      Target->setType(TargetTy);
    } else if (!TargetTy->isError() && !isVariableReference(Target) &&
               !isBitField(Target)) {
      Diags.error(Target->getBeginLoc(), "expression is not assignable",
                  {Target->getRange()});
      TargetTy = errorType();
    }
  }

  // x += e (Free Pascal's C operators): Value is x + (e), x read again.
  auto *Sum = S->isCompound() ? llvm::cast<BinaryExpr>(S->getValue())
                              : nullptr;
  if (Sum && !TargetTy->isError() && assignedProperty(Target)) {
    Diags.error(S->getAssignLoc(),
                "'" + llvm::Twine(getOpSpelling(Sum->getOp())) +
                    "=' cannot assign to a property; use ':='",
                {Target->getRange()});
    TargetTy = errorType();
  }
  if (Sum && TargetTy->isError()) { // reported once: x is not read again
    check(Sum->getRHS());
    return;
  }
  if (TargetTy->isUntyped()) {
    Diags.error(Target->getBeginLoc(),
                "cannot assign to untyped parameter '" +
                    llvm::cast<NameRefExpr>(Target)->getName().Spelling +
                    "'; assign to a typecast such as longint(" +
                    llvm::cast<NameRefExpr>(Target)->getName().Spelling + ")",
                {Target->getRange()});
    check(Sum ? Sum->getRHS() : S->getValue());
    return;
  }
  if (Sum)
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Target))
      if (auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl());
          V && V->getVarKind() == VarDecl::Result &&
          llvm::isa_and_nonnull<RoutineDecl>(Cur->lookup(N->getName().Name))) {
        CompoundResultRef = llvm::dyn_cast<NameRefExpr>(Sum->getLHS());
        CompoundResultVar = V;
      }
  Type *ValueTy = checkValueFor(TargetTy, S->getValue());
  CompoundResultRef = nullptr;
  finishAssign(S, TargetTy, ValueTy);
}


/// The rest of the checks of Target := Value, both already checked.
void Checker::finishAssign(AssignStmt *S, Type *TargetTy, Type *ValueTy){
  Expr *Target = S->getTarget();
  checkIsolatedCopy(S->getValue(), "assigned");
  if (auto *TN = llvm::dyn_cast<NameRefExpr>(Target))
    if (auto *TV = llvm::dyn_cast_or_null<VarDecl>(TN->getDecl());
        TV && TV->isShared() && !TN->getWith() && spawnOutside(TV->getOwner()))
      Diags.error(TN->getName().Loc,
                  "a spawned task cannot assign shared variable '" +
                      TN->getName().Spelling +
                      "', which other tasks use; change what it holds "
                      "through its methods",
                  {TN->getRange()});
  // An owned variable's object stays its own unless moved: only a local
  // variable can refer to it too. Assigning to the variable gives it an
  // object again.
  VarDecl *OwnedV = ownedVarOf(Target);
  if (VarDecl *OV = ownedVarOf(S->getValue()); OV && !ValueTy->isError()) {
    auto *TN = llvm::dyn_cast<NameRefExpr>(Target);
    auto *TV = TN ? llvm::dyn_cast_or_null<VarDecl>(TN->getDecl()) : nullptr;
    bool Local = TV && !TN->getWith() && !TV->isOwned() && CurRoutine &&
                 TV->getOwner() == CurRoutine &&
                 (TV->getVarKind() == VarDecl::Variable ||
                  TV->getVarKind() == VarDecl::ValueParam) &&
                 !TV->isTypedConst() && !TV->isClassVar();
    if (!Local)
      diagOwnedEscape(OV, S->getValue(),
                      TV && TV->getVarKind() == VarDecl::Result);
  }
  if (OwnedV)
    ownedAssign(OwnedV);
  if (llvm::isa<AddrOfExpr>(S->getValue())) { // a lambda: where it is kept
    auto *TN = llvm::dyn_cast<NameRefExpr>(Target);
    auto *TV = TN ? llvm::dyn_cast_or_null<VarDecl>(TN->getDecl()) : nullptr;
    checkLambdaEscape(S->getValue(),
                      TV && !TN->getWith() && CurRoutine &&
                          TV->getOwner() == CurRoutine &&
                          TV->getVarKind() == VarDecl::Variable &&
                          !TV->isTypedConst() && !TV->isClassVar());
  }
  if (TargetTy->isError() || ValueTy->isError())
    return;
  Expr *SavedOwnedTarget = OwnedTarget;
  OwnedTarget = Target;
  bool Modifiable = checkModifiable(Target);
  OwnedTarget = SavedOwnedTarget;
  if (!Modifiable)
    return;
  if (containsFile(TargetTy)) {
    Diags.error(S->getAssignLoc(), "files cannot be assigned",
                {Target->getRange()});
    return;
  }
  checkWideToChar(TargetTy, S->getValue());
  if (!isAssignable(TargetTy, ValueTy) &&
      !convertImplicitly(S->getValue(), TargetTy))
    Diags.error(S->getAssignLoc(),
                "assigning to " + quoted(TargetTy) +
                    " from incompatible type " + quoted(ValueTy),
                {Target->getRange(), S->getValue()->getRange()});
  else if (anyNullSafety()) {
    Decl *TD = nullDeclOf(Target);
    if (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(TD)) // F := v
      TD = R->getResultVar();
    if (auto *N = llvm::dyn_cast<NameRefExpr>(Target);
        N && llvm::isa_and_nonnull<RoutineDecl>(N->getDecl()) && TD)
      nullAssign(nullptr, TD, S->getValue());
    else
      nullAssign(Target, TD, S->getValue());
  }
}


void Checker::visitTryStmt(TryStmt *S){
  markHasTry();
  DirectSequence = nullptr;
  OwnedTry Protected;
  ownedTryBegin(Protected);
  visitSequence(S, S->getBody());
  ownedTryEnd(Protected);
  OwnedState BodyEnd = ownedGet();
  if (S->isFinally()) {
    // The finally part runs after any of the body's states; what it
    // leaves alone is as the body left it.
    ownedSet(Protected.Seen);
    unsigned Tick = Flow ? Flow->Tick : 0;
    int Saved = FinallyLoopDepth;
    FinallyLoopDepth = static_cast<int>(LoopDepth);
    visitSequence(S, S->getFinallyBody());
    FinallyLoopDepth = Saved;
    if (Flow) {
      OwnedState Out = Flow->S;
      if (!BodyEnd.isLive() || !Out.isLive()) {
        ownedDead();
      } else {
        for (unsigned I = 0; I < Flow->N; ++I)
          if (Flow->Changed[I] > Tick) {
            uint64_t Bit = uint64_t(1) << I;
            BodyEnd.Valid = (BodyEnd.Valid & ~Bit) | (Out.Valid & Bit);
            BodyEnd.Moved = (BodyEnd.Moved & ~Bit) | (Out.Moved & Bit);
            BodyEnd.Entry[I] = Out.Entry[I];
          }
        Flow->S = BodyEnd;
      }
    }
    return;
  }
  ++HandlerDepth;
  for (ExceptHandler *H : S->getHandlers()) {
    if (Diags.shouldStop())
      break;
    ownedSet(Protected.Seen);
    checkHandler(H);
    BodyEnd.join(ownedGet());
  }
  ownedSet(Protected.Seen);
  visitSequence(S, S->getExceptBody());
  if (S->catchesAll())
    BodyEnd.join(ownedGet());
  ownedSet(BodyEnd);
  --HandlerDepth;
}


/// The object of the owned variable OV would be kept (or returned)
/// through E without `move`.
void Checker::diagOwnedEscape(VarDecl *OV, Expr *E, bool Returned){
  Diags.error(E->getBeginLoc(),
              "the object of owned variable '" + OV->getName().Spelling +
                  "' can only be " + (Returned ? "returned" : "stored") +
                  " with 'move " + OV->getName().Spelling + "'",
              {E->getRange()});
}


/// on [E:] TFoo do Body: E is a variable of type TFoo visible in Body.
void Checker::checkHandler(ExceptHandler *H){
  ClassType *C = lookupClass(H->getClassName());
  H->setClass(C);
  Scope HandlerScope(Cur);
  if (H->getVarName().isValid()) {
    auto *V = Ctx.create<VarDecl>(H->getVarName(), nullptr);
    V->setType(C ? static_cast<Type *>(C) : errorType());
    V->setOwner(CurRoutine);
    H->setVar(V);
    Used.insert(V);
    HandlerScope.insert(V->getName().Name, V);
  }
  Scope *Saved = Cur;
  Cur = &HandlerScope;
  DirectSequence = nullptr;
  visitStmt(H->getBody());
  Cur = Saved;
}


void Checker::visitRaiseStmt(RaiseStmt *S){
  DirectSequence = nullptr;
  if (!S->getObject()) {
    if (!HandlerDepth)
      Diags.error(S->getBeginLoc(),
                  "'raise' without an object (a re-raise) is only allowed "
                  "in an exception handler",
                  {S->getRange()});
    ownedDead();
    return;
  }
  Type *T = check(S->getObject());
  if (!T->isError() && !llvm::isa<ClassType>(T))
    Diags.error(S->getObject()->getBeginLoc(),
                "'raise' needs an object of a class type, not " + quoted(T),
                {S->getObject()->getRange()});
  // raise ... at Address, Frame: pointers (FPC's CodePointer and Pointer)
  for (Expr *P : {S->getAddress(), S->getFrame()}) {
    if (!P)
      continue;
    Type *Ptr = Ctx.getUntypedPointerType();
    Type *PT = checkValueFor(Ptr, P);
    if (!PT->isError() && !isAssignable(Ptr, PT))
      Diags.error(P->getBeginLoc(),
                  "the " +
                      llvm::Twine(P == S->getAddress() ? "address"
                                                       : "frame") +
                      " of a raised exception must be a pointer, not " +
                      quoted(PT),
                  {P->getRange()});
  }
  ownedDead();
}


void Checker::visitIfStmt(IfStmt *S){
  DirectSequence = nullptr;
  checkCondition(S->getCond(), "if");
  OwnedState Before = ownedGet();
  nullNarrowBy(S->getCond(), true);
  visitStmt(S->getThen());
  OwnedState Then = ownedGet();
  ownedSet(Before);
  nullNarrowBy(S->getCond(), false);
  if (S->getElse())
    visitStmt(S->getElse());
  ownedJoin(Then);
}


void Checker::visitWhileStmt(WhileStmt *S){
  DirectSequence = nullptr;
  OwnedLoop Loop;
  ownedLoopBegin(Loop);
  checkCondition(S->getCond(), "while");
  OwnedState Before = ownedGet();
  nullNarrowBy(S->getCond(), true);
  ++LoopDepth;
  visitStmt(S->getBody());
  --LoopDepth;
  OwnedState Saved = ownedGet();
  ownedSet(Before); // (it ends where the condition is false)
  nullNarrowBy(S->getCond(), false);
  Before = ownedGet();
  ownedSet(Saved);
  ownedLoopEnd(Loop, Before);
}


void Checker::visitRepeatStmt(RepeatStmt *S){
  OwnedLoop Loop;
  ownedLoopBegin(Loop);
  ++LoopDepth;
  visitSequence(S, S->getBody());
  --LoopDepth;
  ownedJoin(Loop.Continues); // continue goes to the condition
  Loop.Continues = OwnedState();
  checkCondition(S->getCond(), "until");
  ownedLoopEnd(Loop, OwnedState());
}


/// `for var x [: T] ...`: declares x in Loop, the scope of the loop. T is
/// its type if it has none written (null: not known yet).
void Checker::declareLoopVar(VarDecl *V, Type *T, Scope &Loop){
  if (V->getTypeSpec())
    T = resolveType(V->getTypeSpec());
  V->setType(T);
  V->setOwner(CurRoutine);
  Cur = &Loop;
  declare(V);
  if (InlineVars)
    InlineVars->push_back(V);
}


void Checker::visitForStmt(ForStmt *S){
  DirectSequence = nullptr;
  NameRefExpr *VarRef = S->getVar();
  const Identifier &Id = VarRef->getName();
  Type *VarTy = errorType();
  bool Usable = false;
  Scope LoopVars(Cur);
  Scope *OuterScope = Cur;
  if (VarDecl *IV = S->getInlineVar()) // (its values cannot use it)
    declareLoopVar(IV, IV->getTypeSpec() ? nullptr : check(S->getInit()),
                   LoopVars);

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
    ActiveFors.push_back({VarRef->getDecl(), S->getBeginLoc()});
  OwnedState Before = ownedGet();
  OwnedLoop Loop;
  ownedLoopBegin(Loop);
  ++LoopDepth;
  visitStmt(S->getBody());
  --LoopDepth;
  ownedLoopEnd(Loop, Before);
  if (Usable)
    ActiveFors.pop_back();
  Cur = OuterScope;
}


/// A hidden variable of a for-in loop (code generation binds it), named
/// in Sc only: its name is no Pascal identifier.
VarDecl *Checker::forInVar(llvm::StringRef Name, Type *T, SourceLocation Loc,
                  Scope &Sc){
  llvm::StringRef N = Ctx.getStrings().intern(Name);
  auto *V = Ctx.create<VarDecl>(Identifier{N, N, Loc}, nullptr);
  V->setType(T);
  V->setOwner(CurRoutine);
  Used.insert(V);
  Sc.insert(N, V);
  return V;
}


NameRefExpr *Checker::forInRef(VarDecl *V, SourceLocation Loc){
  Identifier Id = V->getName();
  Id.Loc = Loc;
  return Ctx.create<NameRefExpr>(Id);
}


/// Base.Name (Name as written in Free Pascal), at Loc.
FieldExpr *Checker::forInMember(Expr *Base, llvm::StringRef Spelling,
                       SourceLocation Loc){
  Identifier Id{Ctx.getStrings().intern(llvm::StringRef(Spelling).lower()),
                Ctx.getStrings().intern(Spelling), Loc};
  return Ctx.create<FieldExpr>(Id.getRange(), Base, Id);
}


/// The member Name of a class, advanced record or interface, or null.
Decl *Checker::findEnumMember(Type *T, llvm::StringRef Name){
  Type *Own = T;
  if (ClassType *MT = memberTableOf(Own))
    Own = MT;
  if (auto *C = llvm::dyn_cast<ClassType>(Own))
    if (Decl *D = C->findMember(Name))
      return D;
  if (auto *I = llvm::dyn_cast<InterfaceType>(Own)) {
    if (RoutineDecl *R = findInterfaceMethod(I, Name))
      return R;
    return findInterfaceProperty(I, Name);
  }
  // (a type helper's: GetEnumerator added to a type by a helper)
  if (ClassType *H = helperFor(T))
    return H->findMember(Name);
  return nullptr;
}


/// for Var in Collection do Body (Turbo/FPC), as in Free Pascal: Var
/// takes the elements of an array or string, the members of a set in
/// order, the values of an ordinal type, or the values of an enumerator.
/// Var := element is an ordinary assignment (Step).
void Checker::visitForInStmt(ForInStmt *S){
  DirectSequence = nullptr;
  NameRefExpr *VarRef = S->getVar();
  const Identifier &Id = VarRef->getName();
  Type *VarTy = errorType();
  bool Usable = false;
  Scope LoopVars(Cur);
  Scope *OuterScope = Cur;
  // for var x in ...: without a type, x has the type of the elements,
  // known when the first one is assigned to it (MakeStep).
  VarDecl *IV = S->getInlineVar();
  VarDecl *Untyped = IV && !IV->getTypeSpec() ? IV : nullptr;
  if (IV) {
    checkTypeOrValue(S->getCollection()); // (it cannot use x)
    declareLoopVar(IV, nullptr, LoopVars);
  }
  if (Untyped) {
    VarRef->setDecl(Untyped);
    Used.insert(Untyped);
    Usable = true;
  } else if (Decl *D = Cur->lookup(Id.Name)) {
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
      Usable = !checkNotForControlVar(VarRef);
    }
  } else {
    diagUndeclared(Id);
  }
  VarRef->setType(VarTy);

  Expr *C = S->getCollection();
  SourceLocation CLoc = C->getBeginLoc();
  Type *CT = checkTypeOrValue(C);
  auto *TypeRef = llvm::dyn_cast<NameRefExpr>(C);
  auto *TypeField = llvm::dyn_cast<FieldExpr>(C);
  bool IsType =
      (TypeRef && llvm::isa_and_nonnull<TypeDecl>(TypeRef->getDecl())) ||
      (TypeField && llvm::isa_and_nonnull<TypeDecl>(TypeField->getMember()));
  ForInStmt::InKind Kind = ForInStmt::Invalid;
  Type *Elem = nullptr;
  if (CT->isError()) {
  } else if (IsType) {
    if (CT->isOrdinal()) {
      Kind = ForInStmt::Ordinals;
      Elem = CT;
    } else {
      Diags.error(CLoc,
                  "'for ... in' over a type needs an ordinal type, not " +
                      quoted(CT),
                  {C->getRange()});
    }
  } else if (auto *A = llvm::dyn_cast<ArrayType>(CT)) {
    Kind = ForInStmt::Array;
    Elem = A->getElementType();
  } else if (auto *OA = llvm::dyn_cast<OpenArrayType>(CT)) {
    Kind = ForInStmt::OpenArray;
    Elem = OA->getElementType();
  } else if (auto *DA = llvm::dyn_cast<DynArrayType>(CT)) {
    Kind = ForInStmt::DynArray;
    Elem = DA->getElementType();
    markUsesManaged(); // the array is held during the loop
  } else if (CT->isAnsiString()) {
    Kind = ForInStmt::AnsiString;
    Elem = Ctx.getCharType();
    markUsesManaged(); // the string is held during the loop
  } else if (CT->isShortString()) {
    Kind = ForInStmt::ShortString;
    Elem = Ctx.getCharType();
  } else if (CT->isStringLiteral()) {
    Kind = ForInStmt::StringConst;
    Elem = Ctx.getCharType();
  } else if (auto *ST = llvm::dyn_cast<SetType>(CT)) {
    Kind = ForInStmt::Set;
    Elem = ST->getElementType(); // null for []: no elements
  } else if (auto *IT = llvm::dyn_cast<IteratorType>(CT)) {
    Kind = ForInStmt::Iterator; // its values, as it yields them
    Elem = IT->getElementType();
    markUsesManaged(); // the iterator is held during the loop
  } else if (CT->isClass() || CT->isInterface() || memberTableOf(CT) ||
             (!CT->isError() && !IsType && findEnumMember(CT, "getenumerator"))) {
    Kind = ForInStmt::Enumerator; // (also through a helper's GetEnumerator)
  }
  if (Kind == ForInStmt::Invalid && !CT->isError() && !IsType)
    Diags.error(CLoc,
                "'for ... in' cannot enumerate a value of type " + quoted(CT) +
                    " (an array, string, set, ordinal type, iterator, or an "
                    "object or record with a GetEnumerator method)",
                {C->getRange()});

  Scope InScope(Cur);
  Scope *Saved = Cur;
  auto MakeStep = [&](Expr *Value) {
    if (Untyped && !Untyped->getType()) {
      Cur = &InScope;
      Type *ET = check(Value);
      Cur = Saved;
      Untyped->setType(ET);
      VarRef->setType(ET);
    }
    auto *Target = Ctx.create<NameRefExpr>(Id);
    auto *Step = Ctx.create<AssignStmt>(
        SourceRange(Id.Loc, C->getRange().getEnd()), Target, S->getInLoc(),
        Value);
    Cur = &InScope;
    visitAssignStmt(Step);
    Cur = Saved;
    return Step;
  };
  if (Kind == ForInStmt::Enumerator) {
    if (!checkEnumerator(S, CT, Usable, InScope, MakeStep))
      Kind = ForInStmt::Invalid;
  } else if (Kind != ForInStmt::Invalid) {
    VarDecl *ElemV =
        Elem ? forInVar("in.elem", Elem, S->getInLoc(), InScope) : nullptr;
    AssignStmt *Step =
        Usable && Elem ? MakeStep(forInRef(ElemV, CLoc)) : nullptr;
    S->setLowering(Kind, Elem, ElemV, Step);
  }
  if (Untyped && !Untyped->getType()) {
    if (Kind != ForInStmt::Invalid)
      Diags.error(Id.Loc,
                  "cannot tell the type of '" + Id.Spelling +
                      "' from an empty set; give the variable a type",
                  {Id.getRange()});
    Untyped->setType(errorType());
    Usable = false;
  }

  if (Usable)
    ActiveFors.push_back({VarRef->getDecl(), S->getBeginLoc()});
  OwnedState Before = ownedGet();
  OwnedLoop Loop;
  ownedLoopBegin(Loop);
  ++LoopDepth;
  visitStmt(S->getBody());
  --LoopDepth;
  ownedLoopEnd(Loop, Before);
  if (Usable)
    ActiveFors.pop_back();
  Cur = OuterScope;
}


void Checker::visitCaseStmt(CaseStmt *S){
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
  OwnedState Before = ownedGet(), After;
  for (CaseArm *Arm : S->getArms()) {
    for (CaseLabel *L : Arm->getLabels()) {
      if (!checkLabel(L, LabelTy, "case label") || SelTy->isError())
        continue;
      for (CaseLabel *Prev : Seen) {
        auto K = [&](int64_t V) { return labelKey(V, LabelTy); };
        if (K(L->LoValue) > K(Prev->HiValue) || K(L->HiValue) < K(Prev->LoValue))
          continue;
        int64_t Dup = K(L->LoValue) > K(Prev->LoValue) ? L->LoValue
                                                        : Prev->LoValue;
        Diags.error(L->Lo->getBeginLoc(),
                    "duplicate case label value " + formatValue(SelTy, Dup),
                    {L->getRange()});
        Diags.note(Prev->Lo->getBeginLoc(), "previous case label is here");
        break;
      }
      Seen.push_back(L);
    }
    ownedSet(Before);
    visitStmt(Arm->getBody());
    After.join(ownedGet());
  }
  ownedSet(Before); // (no else part: no arm may run)
  visitSequence(S->getElseBody().data(), S->getElseBody());
  ownedJoin(After);
}


void Checker::visitWithStmt(WithStmt *S){
  DirectSequence = nullptr;
  // Each record opens a scope; they nest left to right.
  llvm::SmallVector<std::unique_ptr<Scope>, 2> Scopes;
  Scope *Saved = Cur;
  for (WithContext *W : S->getRecords()) {
    W->Routine = CurRoutine;
    Type *T = check(W->Record);
    if (T->isError())
      continue;
    if (auto *CT = llvm::dyn_cast<ClassType>(T)) { // objfpc: an object
      requireNotNil(W->Record, "but its members are used in 'with'");
      Scopes.push_back(std::make_unique<Scope>(Cur, CT, W));
      Cur = Scopes.back().get();
      continue;
    }
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
    if (ClassType *MT = RT->getMemberTable()) // its methods, properties too
      Scopes.push_back(std::make_unique<Scope>(Cur, MT, W));
    else
      Scopes.push_back(std::make_unique<Scope>(Cur, RT, W));
    Cur = Scopes.back().get();
    if (ClassType *H = helperFor(RT)) { // its helper's members (first)
      W->Helper = H;
      Scopes.push_back(std::make_unique<Scope>(Cur, H, W));
      Cur = Scopes.back().get();
    }
  }
  visitStmt(S->getBody());
  Cur = Saved;
}


void Checker::visitExprStmt(ExprStmt *S){
  DirectSequence = nullptr;
  Expr *E = S->getExpr();
  Type *T;
  if (auto *Nav = llvm::dyn_cast<SafeNavExpr>(E); Nav && Nav->isStatement()) {
    checkSafeNav(Nav); // (no value)
    return;
  }
  if (auto *A = llvm::dyn_cast<AwaitExpr>(E))
    T = checkAwait(A, /*NeedValue=*/false);
  else
    T = check(E);
  E->setType(T);
  if (auto *SE = llvm::dyn_cast<SpawnExpr>(E); SE && SE->isPoolRun() && !T->isError())
    Diags.warning(E->getBeginLoc(),
                  "dangling task: the task Pool.Run starts is never awaited; "
                  "keep it in a variable to await later",
                  {E->getRange()});
}


void Checker::visitScopeStmt(ScopeStmt *S){
  DirectSequence = nullptr;
  if (Expr *Tok = S->getToken()) {
    Type *T = check(Tok);
    if (!T->isError() && !T->isToken() && !T->isNil())
      Diags.error(Tok->getBeginLoc(),
                  "the token of a scope must be a TCancelToken, not a value "
                  "of type " + quoted(T),
                  {Tok->getRange()});
  }
  markHasTry(); // its end waits however the block ends
  ++ScopeDepth;
  visitSequence(S, S->getBody());
  --ScopeDepth;
}


/// yield e / yield break / yield from it, in an iterator routine (not in
/// a lambda in one, nor in a finally part).
void Checker::visitYieldStmt(YieldStmt *S){
  DirectSequence = nullptr;
  SourceLocation L = S->getBeginLoc();
  RoutineDecl *R = CurRoutine;
  Expr *V = S->getValue();
  IteratorType *IT = R ? R->getIteratorType() : nullptr;
  if (!IT) {
    Diags.error(L,
                R && R->isLambda()
                    ? "'yield' is not allowed in a lambda"
                    : "'yield' is only allowed in an iterator routine (one "
                      "whose result is 'iterator T')",
                {S->getRange()});
    if (V)
      check(V);
    return;
  }
  if (FinallyLoopDepth >= 0)
    Diags.error(L, "'yield' is not allowed in a 'finally' part",
                {S->getRange()});
  if (S->getYieldKind() == YieldStmt::Break) {
    ownedDead();
    return;
  }
  Type *ET = IT->getElementType();
  if (S->getYieldKind() == YieldStmt::From) {
    Type *T = check(V);
    if (T->isError() || ET->isError())
      return;
    auto *VT = llvm::dyn_cast<IteratorType>(T);
    if (!VT || !isAssignable(ET, VT->getElementType()) ||
        (ET->isStructured() && ET != VT->getElementType()))
      Diags.error(V->getBeginLoc(),
                  "'yield from' needs an iterator of " + quoted(ET) +
                      " values, not a value of type " + quoted(T),
                  {V->getRange()});
    markUsesManaged(); // (held while its values are yielded)
    return;
  }
  Type *T = llvm::isa<TupleExpr>(V) || llvm::isa<CallExpr>(V)
                ? checkValueFor(ET, V)
                : check(V);
  if (VarDecl *OV = ownedVarOf(V))
    diagOwnedEscape(OV, V, /*Returned=*/true);
  checkLambdaEscape(V, /*Local=*/false);
  if (!T->isError() && !ET->isError() && !isAssignable(ET, T) &&
      !convertImplicitly(V, ET))
    Diags.error(V->getBeginLoc(),
                "cannot yield a value of type " + quoted(T) +
                    " from an iterator of " + quoted(ET) + " values",
                {V->getRange()});
  markUsesManaged();
}


void Checker::visitDetachedStmt(DetachedStmt *S){
  DirectSequence = nullptr;
  Expr *C = S->getCall();
  Type *T = check(C);
  if (!T->isError() && (!T->isTask() || !isAsyncCall(C)))
    Diags.error(C->getBeginLoc(),
                "'detached' runs the call of an async routine, not a value "
                "of type " + quoted(T),
                {C->getRange()});
  if (Expr *H = S->getHandler()) {
    // procedure(E: Exception): a routine (or procedural value)
    auto *EParam = Ctx.create<VarDecl>(memberName("E"), nullptr,
                                       VarDecl::ValueParam);
    EParam->setType(ExceptionTy);
    auto *Formal = Ctx.create<RoutineType>(
        Ctx.copyArray<VarDecl *>({EParam}), nullptr);
    auto *P = Ctx.create<VarDecl>(memberName("handler"), nullptr,
                                  VarDecl::ValueParam);
    P->setType(Formal);
    checkRoutineArg(H, P, Formal);
    checkLambdaEscape(H, /*Local=*/false);
    if (!H->getType())
      H->setType(Formal);
  }
}

