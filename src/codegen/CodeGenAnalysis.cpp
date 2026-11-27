// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- CodeGenAnalysis.cpp - AST -> LLVM IR ---------------------------------------===//

#include "CodeGenInternal.h"

using namespace p3;
using namespace p3::detail;

/// The variable of a plain `x := e`: a bare variable name, not reached
/// through `with` and not an `absolute` overlay.
VarDecl *PinAnalysis::plainAssignTarget(Expr *E){
  auto *N = llvm::dyn_cast<NameRefExpr>(E);
  if (!N || N->getWith())
    return nullptr;
  auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl());
  return V && !V->getAbsoluteRoot() ? V : nullptr;
}


/// The variable whose storage an assignment to E writes, if local: x,
/// r.f and a[i] write x, r and a (not the index).
void PinAnalysis::addTarget(Expr *E, VarSet &Out){
  if (!E)
    return;
  if (auto *P = llvm::dyn_cast<ParenExpr>(E)) {
    addTarget(P->getInner(), Out);
    return;
  }
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
    if (auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl())) {
      Out.insert(V->getAbsoluteRoot() ? V->getAbsoluteRoot() : V);
    } else if (N->getWith()) {
      addTarget(N->getWith()->Record, Out);
    }
    return;
  }
  if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
    addTarget(F->getBase(), Out);
    return;
  }
  if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
    addTarget(I->getBase(), Out);
    return;
  }
  if (auto *D = llvm::dyn_cast<DerefExpr>(E)) {
    // The pointee changes. Keep the pointer in memory too: if LLVM keeps it
    // in a register, it may forward a field through it across the setjmp.
    addTarget(D->getBase(), Out);
    return;
  }
}


/// Calls that can write through a var/out parameter. A routine's parameter
/// list says which arguments; builtins are handled by their known effects;
/// an unknown call gets the conservative answer (every argument).
void PinAnalysis::addCallWrites(Decl *D, llvm::ArrayRef<Expr *> Args, VarSet &Out){
  auto Mark = [&](size_t I) {
    if (I < Args.size())
      addTarget(Args[I], Out);
  };
  if (auto *B = llvm::dyn_cast_or_null<BuiltinDecl>(D)) {
    switch (B->getBuiltinKind()) {
    case BuiltinKind::Write:
    case BuiltinKind::Writeln:
    case BuiltinKind::Halt:
    case BuiltinKind::Break:
    case BuiltinKind::Continue:
      return;
    case BuiltinKind::Exit:
      if (!Args.empty() && R->isFunction())
        Out.insert(R->getResultVar());
      return;
    case BuiltinKind::Inc:
    case BuiltinKind::Dec:
    case BuiltinKind::SetLength:
    case BuiltinKind::Delete:
    case BuiltinKind::Insert:
    case BuiltinKind::GetMem:
    case BuiltinKind::FreeMem:
    case BuiltinKind::ReallocMem:
    case BuiltinKind::FillChar:
    case BuiltinKind::New:
    case BuiltinKind::Dispose:
    case BuiltinKind::Assign:
    case BuiltinKind::AssignFile:
    case BuiltinKind::Reset:
    case BuiltinKind::Rewrite:
    case BuiltinKind::Get:
    case BuiltinKind::Put:
    case BuiltinKind::Page:
    case BuiltinKind::Close:
    case BuiltinKind::CloseFile:
    case BuiltinKind::Flush:
    case BuiltinKind::Seek:
      Mark(0);
      return;
    case BuiltinKind::Move:
      Mark(1);
      return;
    case BuiltinKind::BlockRead:
    case BuiltinKind::BlockWrite:
      for (size_t I = 1; I < Args.size(); ++I)
        Mark(I);
      return;
    case BuiltinKind::Pack:
    case BuiltinKind::Unpack:
      for (size_t I = 0; I < Args.size(); ++I)
        Mark(I);
      return;
    default:
      for (size_t I = 0; I < Args.size(); ++I)
        Mark(I);
      return;
    }
  }
  if (auto *RD = llvm::dyn_cast_or_null<RoutineDecl>(D)) {
    auto Params = RD->getParams();
    for (size_t I = 0; I < Args.size() && I < Params.size(); ++I)
      if (Params[I]->isVarParam() || Params[I]->isOutParam())
        addTarget(Args[I], Out);
    return;
  }
  // A procedural value or an unknown declaration.
  for (size_t I = 0; I < Args.size(); ++I)
    Mark(I);
}


void PinAnalysis::addWrittenExpr(Expr *E, VarSet &Out){
  if (!E)
    return;
  if (auto *C = llvm::dyn_cast<CallExpr>(E)) {
    addCallWrites(C->getDecl(), C->getArgs(), Out);
    if (C->getWith())
      addTarget(C->getWith()->Record, Out);
    addTarget(C->getCalleeValue(), Out);
    addWrittenExpr(C->getCalleeValue(), Out);
    for (Expr *A : C->getArgs())
      addWrittenExpr(A, Out);
    return;
  }
  if (auto *C = llvm::dyn_cast<IndirectCallExpr>(E)) {
    for (Expr *A : C->getArgs())
      addTarget(A, Out);
    addTarget(C->getCallee(), Out);
    addWrittenExpr(C->getCallee(), Out);
    for (Expr *A : C->getArgs())
      addWrittenExpr(A, Out);
    return;
  }
  if (auto *P = llvm::dyn_cast<ParenExpr>(E)) {
    addWrittenExpr(P->getInner(), Out);
    return;
  }
  if (auto *U = llvm::dyn_cast<UnaryExpr>(E)) {
    addWrittenExpr(U->getOperand(), Out);
    return;
  }
  if (auto *B = llvm::dyn_cast<BinaryExpr>(E)) {
    addWrittenExpr(B->getLHS(), Out);
    addWrittenExpr(B->getRHS(), Out);
    return;
  }
  if (auto *F = llvm::dyn_cast<FormattedExpr>(E)) {
    addWrittenExpr(F->getValue(), Out);
    addWrittenExpr(F->getWidth(), Out);
    addWrittenExpr(F->getPrecision(), Out);
    return;
  }
  if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
    addWrittenExpr(I->getBase(), Out);
    addWrittenExpr(I->getIndex(), Out);
    return;
  }
  if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
    addWrittenExpr(F->getBase(), Out);
    return;
  }
  if (auto *A = llvm::dyn_cast<AddrOfExpr>(E)) {
    // The address escapes: a call inside the protected code may write it.
    addTarget(A->getOperand(), Out);
    addWrittenExpr(A->getOperand(), Out);
    return;
  }
  if (auto *D = llvm::dyn_cast<DerefExpr>(E)) {
    addWrittenExpr(D->getBase(), Out);
    return;
  }
  if (auto *S = llvm::dyn_cast<SetCtorExpr>(E)) {
    for (const SetElement &El : S->getElements()) {
      addWrittenExpr(El.Lo, Out);
      addWrittenExpr(El.Hi, Out);
    }
    return;
  }
  if (auto *A = llvm::dyn_cast<AggregateExpr>(E)) {
    for (AggregateElement *El : A->getElements())
      addWrittenExpr(El->Value, Out);
    return;
  }
  if (auto *T = llvm::dyn_cast<TemplateExpr>(E)) {
    for (Expr *P : T->getParts())
      addWrittenExpr(P, Out);
    return;
  }
  if (auto *T = llvm::dyn_cast<TupleExpr>(E)) {
    for (Expr *X : T->getElements())
      addWrittenExpr(X, Out);
    return;
  }
  if (auto *N = llvm::dyn_cast<NamedArgExpr>(E)) {
    addWrittenExpr(N->getValue(), Out);
    return;
  }
  if (auto *C = llvm::dyn_cast<CondExpr>(E)) {
    addWrittenExpr(C->getCond(), Out);
    addWrittenExpr(C->getThen(), Out);
    addWrittenExpr(C->getElse(), Out);
    return;
  }
  if (auto *A = llvm::dyn_cast<AwaitExpr>(E)) {
    addWrittenExpr(A->getOperand(), Out);
    return;
  }
  if (auto *S = llvm::dyn_cast<SpawnExpr>(E)) {
    addWrittenExpr(S->getCall(), Out);
    return;
  }
  if (auto *S = llvm::dyn_cast<SafeNavExpr>(E)) {
    addWrittenExpr(S->getBase(), Out);
    addWrittenExpr(S->getAccess(), Out);
    return;
  }
  if (auto *C = llvm::dyn_cast<CoalesceExpr>(E)) {
    addWrittenExpr(C->getLeft(), Out);
    addWrittenExpr(C->getRight(), Out);
    return;
  }
  if (auto *M = llvm::dyn_cast<MoveExpr>(E)) {
    addWrittenExpr(M->getOperand(), Out);
    return;
  }
  if (auto *I = llvm::dyn_cast<InheritedExpr>(E)) {
    for (Expr *A : I->getImplicitArgs())
      addWrittenExpr(A, Out);
    return;
  }
}


void PinAnalysis::addWrittenList(llvm::ArrayRef<Stmt *> Body, VarSet &Out){
  for (Stmt *S : Body)
    addWrittenStmt(S, Out);
}


void PinAnalysis::addWrittenStmt(Stmt *S, VarSet &Out){
  if (!S)
    return;
  if (auto *C = llvm::dyn_cast<CompoundStmt>(S)) {
    addWrittenList(C->getBody(), Out);
    return;
  }
  if (auto *A = llvm::dyn_cast<AssignStmt>(S)) {
    if (!SkipPlainAssigns || !plainAssignTarget(A->getTarget()))
      addTarget(A->getTarget(), Out);
    addWrittenExpr(A->getValue(), Out);
    return;
  }
  if (auto *P = llvm::dyn_cast<ProcCallStmt>(S)) {
    addCallWrites(P->getDecl(), P->getArgs(), Out);
    if (P->getWith())
      addTarget(P->getWith()->Record, Out);
    addTarget(P->getCalleeValue(), Out);
    addWrittenExpr(P->getCalleeValue(), Out);
    for (Expr *A : P->getArgs())
      addWrittenExpr(A, Out);
    return;
  }
  if (auto *I = llvm::dyn_cast<IndirectCallStmt>(S)) {
    addWrittenExpr(I->getCall(), Out);
    return;
  }
  if (auto *I = llvm::dyn_cast<IfStmt>(S)) {
    addWrittenExpr(I->getCond(), Out);
    addWrittenStmt(I->getThen(), Out);
    addWrittenStmt(I->getElse(), Out);
    return;
  }
  if (auto *W = llvm::dyn_cast<WhileStmt>(S)) {
    addWrittenExpr(W->getCond(), Out);
    addWrittenStmt(W->getBody(), Out);
    return;
  }
  if (auto *R = llvm::dyn_cast<RepeatStmt>(S)) {
    addWrittenList(R->getBody(), Out);
    addWrittenExpr(R->getCond(), Out);
    return;
  }
  if (auto *F = llvm::dyn_cast<ForStmt>(S)) {
    addTarget(F->getVar(), Out);
    addWrittenExpr(F->getInit(), Out);
    addWrittenExpr(F->getFinal(), Out);
    addWrittenStmt(F->getBody(), Out);
    return;
  }
  if (auto *F = llvm::dyn_cast<ForInStmt>(S)) {
    addTarget(F->getVar(), Out);
    if (F->getElementVar())
      Out.insert(F->getElementVar());
    if (F->getCollectionVar())
      Out.insert(F->getCollectionVar());
    if (F->getEnumeratorVar())
      Out.insert(F->getEnumeratorVar());
    addWrittenExpr(F->getCollection(), Out);
    if (F->getStep())
      addWrittenStmt(F->getStep(), Out);
    addWrittenExpr(F->getGetEnumerator(), Out);
    addWrittenExpr(F->getMoveNext(), Out);
    addWrittenStmt(F->getBody(), Out);
    return;
  }
  if (auto *C = llvm::dyn_cast<CaseStmt>(S)) {
    addWrittenExpr(C->getSelector(), Out);
    for (CaseArm *A : C->getArms()) {
      for (CaseLabel *L : A->getLabels()) {
        addWrittenExpr(L->Lo, Out);
        addWrittenExpr(L->Hi, Out);
      }
      addWrittenStmt(A->getBody(), Out);
    }
    addWrittenList(C->getElseBody(), Out);
    return;
  }
  if (auto *W = llvm::dyn_cast<WithStmt>(S)) {
    for (WithContext *C : W->getRecords())
      addWrittenExpr(C->Record, Out);
    addWrittenStmt(W->getBody(), Out);
    return;
  }
  if (auto *L = llvm::dyn_cast<LabeledStmt>(S)) {
    addWrittenStmt(L->getBody(), Out);
    return;
  }
  if (auto *T = llvm::dyn_cast<TryStmt>(S)) {
    addWrittenList(T->getBody(), Out);
    for (ExceptHandler *H : T->getHandlers())
      addWrittenStmt(H->getBody(), Out);
    addWrittenList(T->getExceptBody(), Out);
    return;
  }
  if (auto *R = llvm::dyn_cast<RaiseStmt>(S)) {
    addWrittenExpr(R->getObject(), Out);
    addWrittenExpr(R->getAddress(), Out);
    addWrittenExpr(R->getFrame(), Out);
    return;
  }
  if (auto *V = llvm::dyn_cast<VarDeclStmt>(S)) {
    for (VarDecl *D : V->getVars())
      Out.insert(D);
    addWrittenExpr(V->getInit(), Out);
    return;
  }
  if (auto *D = llvm::dyn_cast<DestructureStmt>(S)) {
    for (VarDecl *V : D->getVars())
      Out.insert(V);
    for (Expr *T : D->getTargets())
      addTarget(T, Out);
    addWrittenExpr(D->getValue(), Out);
    return;
  }
  if (auto *E = llvm::dyn_cast<ExprStmt>(S)) {
    addWrittenExpr(E->getExpr(), Out);
    return;
  }
  if (auto *S2 = llvm::dyn_cast<ScopeStmt>(S)) {
    addWrittenExpr(S2->getToken(), Out);
    addWrittenList(S2->getBody(), Out);
    return;
  }
  if (auto *D = llvm::dyn_cast<DetachedStmt>(S)) {
    addWrittenExpr(D->getHandler(), Out);
    addWrittenExpr(D->getCall(), Out);
    return;
  }
  if (auto *Y = llvm::dyn_cast<YieldStmt>(S)) {
    addWrittenExpr(Y->getValue(), Out);
    return;
  }
}


/// The variables an assignment target reads to compute its address: a[i]
/// reads a and i, p^ reads p, r.f reads r, but x := e does not read x.
void PinAnalysis::addTargetReads(Expr *E, VarSet &Out){
  if (!E)
    return;
  if (auto *P = llvm::dyn_cast<ParenExpr>(E)) {
    addTargetReads(P->getInner(), Out);
    return;
  }
  if (llvm::isa<NameRefExpr>(E))
    return;
  if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
    addUsed(F->getBase(), Out);
    return;
  }
  if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
    addUsed(I->getBase(), Out);
    addUsed(I->getIndex(), Out);
    return;
  }
  if (auto *D = llvm::dyn_cast<DerefExpr>(E)) {
    addUsed(D->getBase(), Out);
    return;
  }
}


void PinAnalysis::addUsed(Expr *E, VarSet &Out){
  if (!E)
    return;
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E)) {
    if (auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl())) {
      Out.insert(V->getAbsoluteRoot() ? V->getAbsoluteRoot() : V);
    } else if (N->getWith()) {
      addUsed(N->getWith()->Record, Out);
    }
    return;
  }
  if (auto *P = llvm::dyn_cast<ParenExpr>(E)) {
    addUsed(P->getInner(), Out);
    return;
  }
  if (auto *U = llvm::dyn_cast<UnaryExpr>(E)) {
    addUsed(U->getOperand(), Out);
    return;
  }
  if (auto *B = llvm::dyn_cast<BinaryExpr>(E)) {
    addUsed(B->getLHS(), Out);
    addUsed(B->getRHS(), Out);
    return;
  }
  if (auto *C = llvm::dyn_cast<CallExpr>(E)) {
    if (C->getWith())
      addUsed(C->getWith()->Record, Out);
    addUsed(C->getCalleeValue(), Out);
    for (Expr *A : C->getArgs())
      addUsed(A, Out);
    return;
  }
  if (auto *F = llvm::dyn_cast<FormattedExpr>(E)) {
    addUsed(F->getValue(), Out);
    addUsed(F->getWidth(), Out);
    addUsed(F->getPrecision(), Out);
    return;
  }
  if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
    addUsed(I->getBase(), Out);
    addUsed(I->getIndex(), Out);
    return;
  }
  if (auto *F = llvm::dyn_cast<FieldExpr>(E)) {
    addUsed(F->getBase(), Out);
    return;
  }
  if (auto *A = llvm::dyn_cast<AddrOfExpr>(E)) {
    addUsed(A->getOperand(), Out);
    return;
  }
  if (auto *D = llvm::dyn_cast<DerefExpr>(E)) {
    addUsed(D->getBase(), Out);
    return;
  }
  if (auto *S = llvm::dyn_cast<SetCtorExpr>(E)) {
    for (const SetElement &El : S->getElements()) {
      addUsed(El.Lo, Out);
      addUsed(El.Hi, Out);
    }
    return;
  }
  if (auto *A = llvm::dyn_cast<AggregateExpr>(E)) {
    for (AggregateElement *El : A->getElements())
      addUsed(El->Value, Out);
    return;
  }
  if (auto *C = llvm::dyn_cast<IndirectCallExpr>(E)) {
    addUsed(C->getCallee(), Out);
    for (Expr *A : C->getArgs())
      addUsed(A, Out);
    return;
  }
  if (auto *I = llvm::dyn_cast<InheritedExpr>(E)) {
    for (Expr *A : I->getImplicitArgs())
      addUsed(A, Out);
    return;
  }
  if (auto *M = llvm::dyn_cast<MoveExpr>(E)) {
    addUsed(M->getOperand(), Out);
    return;
  }
  if (auto *T = llvm::dyn_cast<TemplateExpr>(E)) {
    for (Expr *P : T->getParts())
      addUsed(P, Out);
    return;
  }
  if (auto *T = llvm::dyn_cast<TupleExpr>(E)) {
    for (Expr *X : T->getElements())
      addUsed(X, Out);
    return;
  }
  if (auto *N = llvm::dyn_cast<NamedArgExpr>(E)) {
    addUsed(N->getValue(), Out);
    return;
  }
  if (auto *C = llvm::dyn_cast<CondExpr>(E)) {
    addUsed(C->getCond(), Out);
    addUsed(C->getThen(), Out);
    addUsed(C->getElse(), Out);
    return;
  }
  if (auto *A = llvm::dyn_cast<AwaitExpr>(E)) {
    addUsed(A->getOperand(), Out);
    return;
  }
  if (auto *S = llvm::dyn_cast<SpawnExpr>(E)) {
    addUsed(S->getCall(), Out);
    return;
  }
  if (auto *S = llvm::dyn_cast<SafeNavExpr>(E)) {
    addUsed(S->getBase(), Out);
    addUsed(S->getAccess(), Out);
    return;
  }
  if (auto *C = llvm::dyn_cast<CoalesceExpr>(E)) {
    addUsed(C->getLeft(), Out);
    addUsed(C->getRight(), Out);
    return;
  }
}


void PinAnalysis::addUsedList(llvm::ArrayRef<Stmt *> Body, VarSet &Out){
  for (Stmt *S : Body)
    addUsedStmt(S, Out);
}


void PinAnalysis::addUsedStmt(Stmt *S, VarSet &Out){
  if (!S)
    return;
  if (auto *C = llvm::dyn_cast<CompoundStmt>(S)) {
    addUsedList(C->getBody(), Out);
    return;
  }
  if (auto *P = llvm::dyn_cast<ProcCallStmt>(S)) {
    if (P->getWith())
      addUsed(P->getWith()->Record, Out);
    addUsed(P->getCalleeValue(), Out);
    for (Expr *A : P->getArgs())
      addUsed(A, Out);
    return;
  }
  if (auto *I = llvm::dyn_cast<IndirectCallStmt>(S)) {
    addUsed(I->getCall(), Out);
    return;
  }
  if (auto *A = llvm::dyn_cast<AssignStmt>(S)) {
    if (A->isCompound())
      addUsed(A->getTarget(), Out);
    else
      addTargetReads(A->getTarget(), Out);
    addUsed(A->getValue(), Out);
    return;
  }
  if (auto *I = llvm::dyn_cast<IfStmt>(S)) {
    addUsed(I->getCond(), Out);
    addUsedStmt(I->getThen(), Out);
    addUsedStmt(I->getElse(), Out);
    return;
  }
  if (auto *W = llvm::dyn_cast<WhileStmt>(S)) {
    addUsed(W->getCond(), Out);
    addUsedStmt(W->getBody(), Out);
    return;
  }
  if (auto *R = llvm::dyn_cast<RepeatStmt>(S)) {
    addUsedList(R->getBody(), Out);
    addUsed(R->getCond(), Out);
    return;
  }
  if (auto *F = llvm::dyn_cast<ForStmt>(S)) {
    addUsed(F->getVar(), Out);
    addUsed(F->getInit(), Out);
    addUsed(F->getFinal(), Out);
    addUsedStmt(F->getBody(), Out);
    return;
  }
  if (auto *F = llvm::dyn_cast<ForInStmt>(S)) {
    addUsed(F->getVar(), Out);
    addUsed(F->getCollection(), Out);
    addUsedStmt(F->getBody(), Out);
    return;
  }
  if (auto *C = llvm::dyn_cast<CaseStmt>(S)) {
    addUsed(C->getSelector(), Out);
    for (CaseArm *A : C->getArms()) {
      for (CaseLabel *L : A->getLabels()) {
        addUsed(L->Lo, Out);
        addUsed(L->Hi, Out);
      }
      addUsedStmt(A->getBody(), Out);
    }
    addUsedList(C->getElseBody(), Out);
    return;
  }
  if (auto *W = llvm::dyn_cast<WithStmt>(S)) {
    for (WithContext *C : W->getRecords())
      addUsed(C->Record, Out);
    addUsedStmt(W->getBody(), Out);
    return;
  }
  if (auto *L = llvm::dyn_cast<LabeledStmt>(S)) {
    addUsedStmt(L->getBody(), Out);
    return;
  }
  if (auto *T = llvm::dyn_cast<TryStmt>(S)) {
    addUsedList(T->getBody(), Out);
    for (ExceptHandler *H : T->getHandlers())
      addUsedStmt(H->getBody(), Out);
    addUsedList(T->getExceptBody(), Out);
    return;
  }
  if (auto *R = llvm::dyn_cast<RaiseStmt>(S)) {
    addUsed(R->getObject(), Out);
    addUsed(R->getAddress(), Out);
    addUsed(R->getFrame(), Out);
    return;
  }
  if (auto *V = llvm::dyn_cast<VarDeclStmt>(S)) {
    addUsed(V->getInit(), Out);
    return;
  }
  if (auto *D = llvm::dyn_cast<DestructureStmt>(S)) {
    for (Expr *T : D->getTargets())
      addUsed(T, Out);
    addUsed(D->getValue(), Out);
    return;
  }
  if (auto *E = llvm::dyn_cast<ExprStmt>(S)) {
    addUsed(E->getExpr(), Out);
    return;
  }
  if (auto *S2 = llvm::dyn_cast<ScopeStmt>(S)) {
    addUsed(S2->getToken(), Out);
    addUsedList(S2->getBody(), Out);
    return;
  }
  if (auto *D = llvm::dyn_cast<DetachedStmt>(S)) {
    addUsed(D->getHandler(), Out);
    addUsed(D->getCall(), Out);
    return;
  }
  if (auto *Y = llvm::dyn_cast<YieldStmt>(S)) {
    addUsed(Y->getValue(), Out);
    return;
  }
}


void PinAnalysis::pinList(llvm::ArrayRef<Stmt *> Body, VarSet After){
  for (auto It = Body.rbegin(); It != Body.rend(); ++It) {
    pinStmt(*It, After);
    addUsedStmt(*It, After);
  }
}


void PinAnalysis::pinStmt(Stmt *S, const VarSet &After){
  if (!S)
    return;
  if (auto *C = llvm::dyn_cast<CompoundStmt>(S)) {
    pinList(C->getBody(), After);
    return;
  }
  if (auto *T = llvm::dyn_cast<TryStmt>(S)) {
    VarSet Landing = After;
    for (ExceptHandler *H : T->getHandlers())
      addUsedStmt(H->getBody(), Landing);
    addUsedList(T->getExceptBody(), Landing);
    VarSet Written;
    addWrittenList(T->getBody(), Written);
    for (VarDecl *V : Written)
      if (Landing.count(V))
        Pins.insert(V);
    pinList(T->getBody(), Landing);
    for (ExceptHandler *H : T->getHandlers())
      pinStmt(H->getBody(), After);
    pinList(T->getExceptBody(), After);
    return;
  }
  if (auto *I = llvm::dyn_cast<IfStmt>(S)) {
    pinStmt(I->getThen(), After);
    pinStmt(I->getElse(), After);
    return;
  }
  if (auto *W = llvm::dyn_cast<WhileStmt>(S)) {
    VarSet LoopAfter = After;
    addUsed(W->getCond(), LoopAfter);
    addUsedStmt(W->getBody(), LoopAfter);
    pinStmt(W->getBody(), LoopAfter);
    return;
  }
  if (auto *R = llvm::dyn_cast<RepeatStmt>(S)) {
    VarSet LoopAfter = After;
    addUsedList(R->getBody(), LoopAfter);
    addUsed(R->getCond(), LoopAfter);
    pinList(R->getBody(), LoopAfter);
    return;
  }
  if (auto *F = llvm::dyn_cast<ForStmt>(S)) {
    VarSet LoopAfter = After;
    addUsed(F->getVar(), LoopAfter);
    addUsed(F->getInit(), LoopAfter);
    addUsed(F->getFinal(), LoopAfter);
    addUsedStmt(F->getBody(), LoopAfter);
    pinStmt(F->getBody(), LoopAfter);
    return;
  }
  if (auto *F = llvm::dyn_cast<ForInStmt>(S)) {
    VarSet LoopAfter = After;
    addUsed(F->getVar(), LoopAfter);
    addUsed(F->getCollection(), LoopAfter);
    addUsedStmt(F->getBody(), LoopAfter);
    pinStmt(F->getBody(), LoopAfter);
    return;
  }
  if (auto *C = llvm::dyn_cast<CaseStmt>(S)) {
    for (CaseArm *A : C->getArms())
      pinStmt(A->getBody(), After);
    pinList(C->getElseBody(), After);
    return;
  }
  if (auto *W = llvm::dyn_cast<WithStmt>(S)) {
    pinStmt(W->getBody(), After);
    return;
  }
  if (auto *L = llvm::dyn_cast<LabeledStmt>(S)) {
    pinStmt(L->getBody(), After);
    return;
  }
  if (auto *S2 = llvm::dyn_cast<ScopeStmt>(S)) {
    pinList(S2->getBody(), After);
    return;
  }
}


/// The variables that S writes other than by a plain `x := e`: through
/// a var argument, @x, a for loop, an overlay... (optimization3 item 4).
VarSet PinAnalysis::otherWrites(Stmt *S){
  VarSet Out;
  SkipPlainAssigns = true;
  addWrittenStmt(S, Out);
  SkipPlainAssigns = false;
  return Out;
}


VarSet PinAnalysis::run(){
  VarSet After;
  if (R->isFunction())
    After.insert(R->getResultVar());
  // Owned variables are freed by an implicit try-finally around the body;
  // its landing reads them, and any write to one happens inside it.
  bool Owned = false;
  for (Decl *D : R->getBody()->getDecls())
    if (auto *V = llvm::dyn_cast<VarDecl>(D)) {
      // A file local is closed on the way out, after the landing.
      if (V->getType() && containsFile(V->getType()))
        After.insert(V);
      if (V->isOwned()) {
        After.insert(V);
        Owned = true;
      }
    }
  if (Owned) {
    VarSet Written;
    addWrittenStmt(R->getBody()->getBody(), Written);
    for (VarDecl *V : Written)
      if (After.count(V))
        Pins.insert(V);
  }
  pinStmt(R->getBody()->getBody(), After);
  return Pins;
}


/// Are range / overflow checks on at L? (command line, then {$R±}/{$Q±})
bool CodeGenModule::rangeChecks(SourceLocation L) const{
  return !CGOpts.Directives
             ? CGOpts.RangeChecks
             : CGOpts.Directives->isEnabled(CheckDirectives::Range, L,
                                            CGOpts.RangeChecks);
}

/// In an unchecked block at L: integer values converted to a narrower
/// integer type wrap instead of being range-checked.
bool CodeGenModule::wraps(SourceLocation L) const{
  return CGOpts.Directives &&
         CGOpts.Directives->isEnabled(CheckDirectives::Wrap, L, false);
}

/// In a checked block or expression at L (Wrap explicitly off): integer
/// values converted to a narrower integer type are checked, whatever
/// {$R} says.
bool CodeGenModule::checksConversions(SourceLocation L) const{
  return CGOpts.Directives && CGOpts.Directives->stateAt(
                                  CheckDirectives::Wrap, L) == CheckDirectives::Off;
}

bool CodeGenModule::overflowChecks(SourceLocation L) const{
  return !CGOpts.Directives
             ? CGOpts.OverflowChecks
             : CGOpts.Directives->isEnabled(CheckDirectives::Overflow, L,
                                            CGOpts.OverflowChecks);
}

bool CodeGenModule::pointerMathChecked(SourceLocation L) const{
  return CGOpts.Directives &&
         CGOpts.Directives->stateAt(CheckDirectives::PointerMath, L) ==
             CheckDirectives::Checked;
}

void CodeGenModule::emitPointerAddCheck(PointerType *PT, llvm::Value *Base,
                                        llvm::Value *Delta, SourceLocation L){
  int64_t ElemSize = static_cast<int64_t>(DL.getTypeAllocSize(pointeeMemType(PT)));
  if (ElemSize < 1)
    ElemSize = 1;
  llvm::Value *Bad = B.CreateICmpNE(
      astrCall("__pas_ptr_bad_add", B.getInt32Ty(),
               {Base, Delta, B.getInt64(ElemSize)}),
      B.getInt32(0));
  emitRuntimeCheck(Bad, "pointer arithmetic out of bounds", L);
}

void CodeGenModule::emitPointerIndexCheck(PointerType *PT, llvm::Value *Base,
                                          llvm::Value *Index, SourceLocation L){
  int64_t ElemSize = static_cast<int64_t>(DL.getTypeAllocSize(pointeeMemType(PT)));
  if (ElemSize < 1)
    ElemSize = 1;
  llvm::Value *Bad = B.CreateICmpNE(
      astrCall("__pas_ptr_bad_index", B.getInt32Ty(),
               {Base, Index, B.getInt64(ElemSize)}),
      B.getInt32(0));
  emitRuntimeCheck(Bad, "pointer index out of bounds", L);
}

void CodeGenModule::emitPointerDiffCheck(llvm::Value *LHS, llvm::Value *RHS,
                                         SourceLocation L){
  llvm::Value *Bad = B.CreateICmpNE(
      astrCall("__pas_ptr_bad_diff", B.getInt32Ty(), {LHS, RHS}),
      B.getInt32(0));
  emitRuntimeCheck(
      Bad, "pointer difference requires pointers in the same live allocation", L);
}


/// Ordinal values compare as signed if their host is a signed integer
/// type, unsigned otherwise (byte, word, cardinal, char, boolean, enum).
bool CodeGenModule::isSignedOrdinal(Type *T){ return T->isSignedIntegerLike(); }


/// The loop's hoist scope that covers the index's base: the array whose
/// length check is hoisted, innermost loop last.
/// The loop's hoist scope that covers the index's base: the array whose
/// length this for checks (null: none). Innermost loop last.
const std::pair<VarDecl *, llvm::Value *> *CodeGenModule::checkedScopeOf(const Expr *E) const{
  if (!CurRoutine) // (the walk and its proof are per routine)
    return nullptr;
  auto *V = countedBase(E);
  if (!V)
    return nullptr;
  for (size_t I = CheckedBases.size(); I > 0; --I)
    if (CheckedBases[I - 1].first == V)
      return &CheckedBases[I - 1];
  return nullptr;
}


/// An integer expression as i32 (field widths, string positions).
llvm::Value *CodeGenModule::emitI32(Expr *E){
  return B.CreateTrunc(toI64(visitExpr(E), E->getType()), B.getInt32Ty());
}


/// The no-wrap flags for an element index: `nuw` when the index is a
/// plain variable whose range the analysis knows at or above LoNeeded
/// and small enough that the scaled offset stays inside 64 bits
/// (llvm-23.md LB-1). Inbounds is the GEP's own.
llvm::GEPNoWrapFlags CodeGenModule::indexFlags(Expr *Idx, int64_t LoNeeded){
  auto *N = llvm::dyn_cast<NameRefExpr>(unwrapConst(Idx));
  auto *VD =
      N ? llvm::dyn_cast_or_null<VarDecl>(N->getDecl()) : nullptr;
  if (!VD)
    return llvm::GEPNoWrapFlags::inBounds();
  auto It = VarRanges.find(VD);
  if (It == VarRanges.end())
    return llvm::GEPNoWrapFlags::inBounds();
  const ValueRange &R = It->second;
  if (!R.Known || R.Lo < LoNeeded || R.Hi > LoNeeded + (int64_t(1) << 30))
    return llvm::GEPNoWrapFlags::inBounds();
  return llvm::GEPNoWrapFlags::fromRaw(
      llvm::GEPNoWrapFlags::inBounds().getRaw() |
      llvm::GEPNoWrapFlags::noUnsignedWrap().getRaw());
}


unsigned CodeGenModule::intBits(Type *T){
  return llvm::cast<IntegerType>(T->getBase())->getBits();
}


/// Widens an ordinal register value to i64 for range comparisons.
llvm::Value *CodeGenModule::toI64(llvm::Value *V, Type *T){
  return isSignedOrdinal(T) ? B.CreateSExt(V, B.getInt64Ty())
                            : B.CreateZExt(V, B.getInt64Ty());
}


/// The range of an already emitted integer value: a literal, a load of an
/// active loop variable, or a result noted by the arithmetic below.
CodeGenModule::ValueRange CodeGenModule::rangeOfValue(const llvm::Value *V) const {
  auto It = ValueRanges.find(V);
  if (It != ValueRanges.end())
    return It->second;
  if (auto *C = llvm::dyn_cast<llvm::ConstantInt>(V))
    if (C->getBitWidth() <= 64)
      return {true, C->getSExtValue(), C->getSExtValue()};
  return {};
}


/// (Not for a constant: those are shared, and rangeOfValue knows their
/// exact range.)
void CodeGenModule::noteRange(const llvm::Value *V, ValueRange R){
  if (R.Known && !llvm::isa<llvm::Constant>(V))
    ValueRanges[V] = R;
}


/// Sets (or, R unknown, forgets) the range of a variable of RangeLocals.
/// Inside a loop that bounds it, a forgotten range is that bound.
void CodeGenModule::setVarRange(VarDecl *V, ValueRange R){
  if (!R.Known) {
    auto It = LoopRanges.find(V);
    if (It != LoopRanges.end())
      R = It->second;
  }
  if (R.Known)
    VarRanges[V] = R;
  else
    VarRanges.erase(V);
}


/// The variables of RangeLocals that S may write.
VarSet CodeGenModule::rangeLocalsWrittenBy(Stmt *S){
  VarSet All, Out;
  PinAnalysis(CurRoutine).writtenBy(S, All);
  for (VarDecl *V : All)
    if (RangeLocals.count(V))
      Out.insert(V);
  return Out;
}


void CodeGenModule::forgetRanges(const VarSet &Vars){
  for (VarDecl *V : Vars)
    setVarRange(V, {});
}


/// A bare variable reference, unwrapping parens and const wrappers.
NameRefExpr *CodeGenModule::plainVarRef(const Expr *E){
  return llvm::dyn_cast_or_null<NameRefExpr>(unwrapConst(const_cast<Expr *>(E)));
}


/// The base's variable when E reads or writes a counted type (dynamic
/// array, open array, array of const) through a plain variable of the
/// routine itself (never captured: a nested routine or a spawn block
/// could write it unseen).
VarDecl *CodeGenModule::countedBase(const Expr *E) const{
  E = unwrapConst(const_cast<Expr *>(E));
  if (auto *P = llvm::dyn_cast<ParenExpr>(E))
    return countedBase(P->getInner());
  if (auto *F = llvm::dyn_cast<FieldExpr>(E))
    return nullptr;
  const Expr *Base = E;
  if (auto *I = llvm::dyn_cast<IndexExpr>(E))
    Base = I->getBase();
  auto *N = plainVarRef(Base);
  if (!N)
    return nullptr;
  auto *D = N->getDecl();
  if (!D)
    return nullptr;
  auto *V = llvm::dyn_cast_or_null<VarDecl>(D);
  if (!V || V->isCaptured() || !V->getType())
    return nullptr;
  if (V->getOwner() != CurRoutine) {
    // A global: only when the routine is flat — no nested routines in
    // its declarations (none of them — however deep — could otherwise
    // write the global's length with no var parameter to see).
    if (!CurRoutine || !CurRoutine->getBody())
      return nullptr;
    for (Decl *ND : CurRoutine->getBody() ? CurRoutine->getBody()->getDecls() : llvm::ArrayRef<Decl *>())
      if (llvm::isa<RoutineDecl>(ND))
        return nullptr;
    return V;
  }
  if (!V || false || !V->getType())
    return nullptr;
  Type *T = V->getType();
  if (!T->isDynArray() && !T->isOpenArray() && !T->isArrayOfConst())
    return nullptr;
  return V;
}


void CodeGenModule::hoistWalk(const Stmt *S, HoistWalk &W, bool Unconditional) const{
  if (!S)
    return;
  if (auto *C = llvm::dyn_cast<CompoundStmt>(S)) {
    for (Stmt *X : C->getBody())
      hoistWalk(X, W, Unconditional);
    return;
  }
  if (auto *A = llvm::dyn_cast<AssignStmt>(S)) {
    hoistExpr(A->getTarget(), W, Unconditional);
    hoistExpr(A->getValue(), W, Unconditional);
    return;
  }
  if (auto *P = llvm::dyn_cast<ProcCallStmt>(S)) {
    if (!P->getDecl()) // a call whose callee's writes are unseen
      W.Bail = true;
    for (Expr *A : P->getArgs())
      hoistExpr(A, W, Unconditional);
    if (P->getCalleeValue())
      hoistExpr(P->getCalleeValue(), W, Unconditional);
    return;
  }
  if (llvm::isa<IndirectCallStmt>(S)) {
    W.Bail = true;
    return;
  }
  if (auto *I = llvm::dyn_cast<IfStmt>(S)) {
    hoistExpr(I->getCond(), W, Unconditional);
    // the accesses inside are conditional
    hoistWalk(I->getThen(), W, false);
    if (I->getElse())
      hoistWalk(I->getElse(), W, false);
    return;
  }
  if (auto *W2 = llvm::dyn_cast<WhileStmt>(S)) {
    hoistExpr(W2->getCond(), W, Unconditional);
    // the cond may run unseen rounds: its accesses are conditional
    hoistWalk(W2->getBody(), W, false);
    return;
  }
  if (auto *R = llvm::dyn_cast<RepeatStmt>(S)) {
    for (Stmt *X : R->getBody())
      hoistWalk(X, W, Unconditional);
    hoistExpr(R->getCond(), W, Unconditional);
    return;
  }
  if (auto *F = llvm::dyn_cast<ForStmt>(S)) {
    hoistExpr(F->getInit(), W, Unconditional);
    hoistExpr(F->getFinal(), W, Unconditional);
    // (an entered for runs at least once: the entry check is hoisted)
    hoistWalk(F->getBody(), W, Unconditional);
    return;
  }
  if (auto *F = llvm::dyn_cast<ForInStmt>(S)) {
    hoistExpr(F->getCollection(), W, Unconditional);
    // (a collection may be empty: its body's accesses are conditional)
    hoistWalk(F->getBody(), W, false);
    return;
  }
  if (auto *T = llvm::dyn_cast<TryStmt>(S)) {
    W.Bail = true;
    return;
  }
  if (auto *C = llvm::dyn_cast<CaseStmt>(S)) {
    hoistExpr(C->getSelector(), W, Unconditional);
    for (CaseArm *A : C->getArms()) {
      for (CaseLabel *L : A->getLabels()) {
        hoistExpr(L->Lo, W, Unconditional);
        if (L->Hi)
          hoistExpr(L->Hi, W, Unconditional);
      }
      hoistWalk(A->getBody(), W, false);
    }
    for (Stmt *X : C->getElseBody())
      hoistWalk(X, W, false);
    return;
  }
  if (auto *W2 = llvm::dyn_cast<WithStmt>(S)) {
    // the records' own evaluations may name arrays
    for (WithContext *X : W2->getRecords())
      if (X->Record)
        hoistExpr(X->Record, W, Unconditional);
    hoistWalk(W2->getBody(), W, Unconditional);
    return;
  }
  if (auto *L = llvm::dyn_cast<LabeledStmt>(S)) {
    W.Bail = true; // a goto target inside
    return;
  }
  if (auto *SC = llvm::dyn_cast<ScopeStmt>(S)) {
    for (Stmt *X : SC->getBody())
      hoistWalk(X, W, Unconditional);
    return;
  }
  if (llvm::isa<ExprStmt>(S) || llvm::isa<VarDeclStmt>(S) ||
      llvm::isa<DestructureStmt>(S) || llvm::isa<RaiseStmt>(S) ||
      llvm::isa<GotoStmt>(S) || llvm::isa<YieldStmt>(S) ||
      llvm::isa<DetachedStmt>(S))
    W.Bail = true;
}


void CodeGenModule::hoistExpr(const Expr *E, HoistWalk &W, bool Unconditional) const{
  if (!E)
    return;
  E = unwrapConst(const_cast<Expr *>(E));
  if (auto *P = llvm::dyn_cast<ParenExpr>(E))
    return hoistExpr(P->getInner(), W, Unconditional);
  if (llvm::isa<SpawnExpr>(E) || llvm::isa<AwaitExpr>(E) ||
      llvm::isa<MoveExpr>(E) || llvm::isa<AddrOfExpr>(E) ||
      llvm::isa<IndirectCallExpr>(E) || llvm::isa<DerefExpr>(E)) {
    W.Bail = true;
    return;
  }
  if (auto *C = llvm::dyn_cast<CallExpr>(E)) {
    if (!C->getDecl()) {
      W.Bail = true;
      return;
    }
    if (auto *B = llvm::dyn_cast<BuiltinDecl>(C->getDecl()))
      switch (B->getBuiltinKind()) {
      case BuiltinKind::GetMem:
      case BuiltinKind::FreeMem:
      case BuiltinKind::ReallocMem:
        W.Bail = true;
        return;
      default:
        break;
      }
    hoistExpr(C->getCalleeValue(), W, Unconditional);
    for (Expr *A : C->getArgs())
      hoistExpr(A, W, Unconditional);
    return;
  }
  if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
    hoistExpr(I->getBase(), W, Unconditional);
    hoistExpr(I->getIndex(), W, Unconditional);
    // an element access of a counted type (whose base is a plain
    // variable), recorded: bare index, and whether unconditional
    auto *V = countedBase(E);
    if (V) {
      bool Bare = false;
      if (auto *N = plainVarRef(I->getIndex()))
        Bare = N->getDecl() == CurForVar;
      W.touch(V, const_cast<Expr *>(I->getBase()), Bare, Unconditional);
    }
    return;
  }
  if (auto *U = llvm::dyn_cast<UnaryExpr>(E))
    return hoistExpr(U->getOperand(), W, Unconditional);
  if (auto *B = llvm::dyn_cast<BinaryExpr>(E))
    return hoistExpr(B->getLHS(), W, Unconditional),
           hoistExpr(B->getRHS(), W, Unconditional);
  if (auto *F = llvm::dyn_cast<FormattedExpr>(E))
    return hoistExpr(F->getValue(), W, Unconditional);
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
    (void)N; // plain reads
}


/// The bases the loop hoists: every access's index is the bare control
/// variable, unconditional, with nothing that bails. Null on none.
llvm::SmallVector<std::pair<VarDecl *, Expr *>, 2>
CodeGenModule::hoistableBases(ForStmt *S){
  llvm::SmallVector<std::pair<VarDecl *, Expr *>, 2> Out;
  // (Parked until the self-hosted compiler's walk mirrors this one:
  // parity requires both or neither, and the mirror is the next step.)
  return Out;
  if (!CurRoutine) // (the program's own body: no frame to speak of)
    return Out;
  VarDecl *Var = llvm::cast<VarDecl>(S->getVar()->getDecl());
  VarDecl *SavedForVar = CurForVar;
  CurForVar = Var;
  HoistWalk W;
  hoistWalk(S->getBody(), W, true);
  hoistExpr(S->getInit(), W, true);
  hoistExpr(S->getFinal(), W, true);
  CurForVar = SavedForVar;
  if (W.Bail)
    return Out;
  VarSet Written;
  PinAnalysis(CurRoutine).writtenBy(S->getBody(), Written);
  if (Written.count(Var))
    return Out; // the control variable is written inside
  for (auto &[V, T] : W.Bases) {
    auto *BaseExpr = plainVarRef(std::get<3>(T));
    auto *Base = llvm::dyn_cast_or_null<VarDecl>(BaseExpr ? BaseExpr->getDecl()
                                                          : nullptr);
    if (std::get<0>(T) && std::get<1>(T) && !std::get<2>(T) && Base &&
        !Written.count(Base))
      Out.push_back({V, std::get<3>(T)}); // (a SetLength or an
                                          // assignment of the base was
                                          // walked: it stays out)
  }
  return Out;
}


/// Chooses RangeLocals for R, each starting at 0 (the value every scalar
/// local starts with, see emitZeroInit).
void CodeGenModule::initRangeLocals(RoutineDecl *R){
  RangeLocals.clear();
  LoopRanges.clear();
  // (a label: a goto may arrive with any value)
  for (Decl *D : R->getBody()->getDecls())
    if (llvm::isa<LabelDecl>(D))
      return;
  VarSet Other = PinAnalysis(R).otherWrites(R->getBody()->getBody());
  for (Decl *D : R->getBody()->getDecls()) {
    auto *V = llvm::dyn_cast<VarDecl>(D);
    if (!V || V->getVarKind() != VarDecl::Variable || V->isTypedConst() ||
        V->isInitializedVar() || V->isInline() || V->getAbsolute() ||
        V->isCaptured() || V->isOwned() || Other.count(V) || !V->getType() ||
        !V->getType()->isOrdinal() || !isSignedOrdinal(V->getType()))
      continue;
    RangeLocals.insert(V);
    VarRanges[V] = {true, 0, 0};
  }
}


/// An integer constant written as a literal, a named constant or one of
/// those negated: its value.
bool CodeGenModule::constInt(Expr *E, int64_t &Out){
  if (auto *P = llvm::dyn_cast<ParenExpr>(E))
    return constInt(P->getInner(), Out);
  if (auto *L = llvm::dyn_cast<IntegerLiteralExpr>(E)) {
    if (L->getValue() > uint64_t(INT64_MAX))
      return false;
    Out = int64_t(L->getValue());
    return true;
  }
  if (auto *U = llvm::dyn_cast<UnaryExpr>(E)) {
    int64_t X;
    if (U->getOp() == UnaryOp::Not || !constInt(U->getOperand(), X) ||
        X == INT64_MIN)
      return false;
    Out = U->getOp() == UnaryOp::Minus ? -X : X;
    return true;
  }
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
    if (auto *C = llvm::dyn_cast_or_null<ConstDecl>(N->getDecl()))
      if (C->getValue().K == ConstValue::Ordinal && C->getType() &&
          C->getType()->isIntegerLike()) {
        Out = C->getValue().Int;
        return true;
      }
  return false;
}


/// V := V + c, V := c + V or V := V - c, for a constant c: its step.
bool CodeGenModule::accumulatorStep(AssignStmt *A, VarDecl *V, int64_t &Step){
  auto *E = llvm::dyn_cast<BinaryExpr>(A->getValue());
  if (!E)
    return false;
  auto IsV = [&](Expr *X) { return PinAnalysis::plainAssignTarget(X) == V; };
  if (E->getOp() == BinaryOp::Add) {
    if (IsV(E->getLHS()) && constInt(E->getRHS(), Step))
      return true;
    return IsV(E->getRHS()) && constInt(E->getLHS(), Step);
  }
  if (E->getOp() == BinaryOp::Sub && IsV(E->getLHS()) &&
      constInt(E->getRHS(), Step) && Step != INT64_MIN) {
    Step = -Step;
    return true;
  }
  return false;
}


/// Does every write of V in S (a for loop's body) add a constant to it,
/// each at most once per iteration (not inside an inner loop)? Adds the
/// possible change per iteration to [DLo, DHi]; false if that overflows
/// int64 (as everywhere in this rule: the self-hosted compiler has no
/// wider integers, and must come to the same answer).
bool CodeGenModule::accumulatorSteps(Stmt *S, VarDecl *V, int64_t &DLo, int64_t &DHi){
  if (!S)
    return true;
  if (auto *C = llvm::dyn_cast<CompoundStmt>(S)) {
    for (Stmt *X : C->getBody())
      if (!accumulatorSteps(X, V, DLo, DHi))
        return false;
    return true;
  }
  if (auto *A = llvm::dyn_cast<AssignStmt>(S)) {
    if (PinAnalysis::plainAssignTarget(A->getTarget()) != V)
      return true; // (V is only written by assignments to it)
    int64_t Step;
    if (!accumulatorStep(A, V, Step))
      return false;
    return !__builtin_add_overflow(Step < 0 ? DLo : DHi, Step,
                                   Step < 0 ? &DLo : &DHi);
  }
  if (auto *I = llvm::dyn_cast<IfStmt>(S))
    return accumulatorSteps(I->getThen(), V, DLo, DHi) &&
           accumulatorSteps(I->getElse(), V, DLo, DHi);
  if (auto *C = llvm::dyn_cast<CaseStmt>(S)) {
    for (CaseArm *A : C->getArms())
      if (!accumulatorSteps(A->getBody(), V, DLo, DHi))
        return false;
    for (Stmt *X : C->getElseBody())
      if (!accumulatorSteps(X, V, DLo, DHi))
        return false;
    return true;
  }
  // Anything else (an inner loop, a try statement...): V not written.
  VarSet W;
  PinAnalysis(CurRoutine).writtenBy(S, W);
  return !W.count(V);
}


/// The bounds of a signed integer type of Bits bits.
bool CodeGenModule::signedRange(unsigned Bits, ValueRange &R){
  if (Bits == 0 || Bits > 64)
    return false;
  if (Bits == 64) {
    R = {true, INT64_MIN, INT64_MAX};
    return true;
  }
  R = {true, -(int64_t(1) << (Bits - 1)), (int64_t(1) << (Bits - 1)) - 1};
  return true;
}


bool CodeGenModule::inInt64(__int128 V){
  return V >= INT64_MIN && V <= INT64_MAX;
}


/// The range of A op B (add, sub or mul), when it fits int64.
bool CodeGenModule::combineRange(BinaryOp Op, ValueRange A, ValueRange B,
                         ValueRange &R){
  if (!A.Known || !B.Known)
    return false;
  __int128 Lo, Hi;
  switch (Op) {
  case BinaryOp::Add:
    Lo = (__int128)A.Lo + B.Lo;
    Hi = (__int128)A.Hi + B.Hi;
    break;
  case BinaryOp::Sub:
    Lo = (__int128)A.Lo - B.Hi;
    Hi = (__int128)A.Hi - B.Lo;
    break;
  case BinaryOp::Mul: {
    __int128 P1 = (__int128)A.Lo * B.Lo, P2 = (__int128)A.Lo * B.Hi;
    __int128 P3 = (__int128)A.Hi * B.Lo, P4 = (__int128)A.Hi * B.Hi;
    Lo = std::min(std::min(P1, P2), std::min(P3, P4));
    Hi = std::max(std::max(P1, P2), std::max(P3, P4));
    break;
  }
  default: return false;
  }
  if (!inInt64(Lo) || !inInt64(Hi))
    return false;
  R = {true, (int64_t)Lo, (int64_t)Hi};
  return true;
}


/// X < Lo or X > Hi, for an i64 X.
///
/// Throughout this file, instructions are created one statement at a
/// time, never as two arguments of one call: C++ leaves the order of
/// argument evaluation unspecified, and the IR must not depend on the
/// compiler that built p3 (the self-hosted compiler writes the same IR,
/// SELFHOST.md S10).
llvm::Value *CodeGenModule::outsideRange(llvm::Value *X, int64_t Lo, int64_t Hi){
  // One unsigned comparison: (uint64)(X - Lo) > (uint64)(Hi - Lo). The
  // subtraction wraps, so X < Lo becomes a huge offset.
  llvm::Value *Off = B.CreateSub(X, B.getInt64(Lo));
  llvm::Value *Span = B.CreateSub(B.getInt64(Hi), B.getInt64(Lo));
  return B.CreateICmpUGT(Off, Span);
}


/// Checks that ordinal value V (of type From) lies within Target's bounds.
void CodeGenModule::emitRangeCheck(llvm::Value *V, Type *From, Type *Target,
                    SourceLocation L,
                    llvm::StringRef Msg){
  // An integer converted to an integer type (not a subrange): wraps in an
  // unchecked block, is checked (EIntOverflow) in a checked one whatever
  // {$R} says.
  bool IntConv = llvm::isa<IntegerType>(Target) && From->isIntegerLike();
  bool Checked = IntConv && checksConversions(L);
  if ((!rangeChecks(L) && !Checked) || !Target->isOrdinal())
    return;
  if (IntConv && wraps(L))
    return;
  auto Fail = [&](llvm::Value *Cond) {
    if (Checked)
      emitOverflowCheck(Cond, "integer overflow", L);
    else
      emitRuntimeCheck(Cond, Msg, L);
  };
  OrdinalBounds TB = getOrdinalBounds(Target);
  OrdinalBounds FB = getOrdinalBounds(From);
  if (isSignedOrdinal(From) && isSignedOrdinal(Target)) {
    ValueRange R = rangeOfValue(V);
    if (R.Known && R.Lo >= TB.Lo && R.Hi <= TB.Hi)
      return;
  }
  bool FromQWord = isQWord(From->getBase()), ToQWord = isQWord(Target);
  if (FromQWord) { // its values, unsigned: those above Target's
    if (ToQWord)
      return;
    llvm::Value *X = toI64(V, From);
    llvm::Value *Above = B.CreateICmpUGT(X, B.getInt64(TB.Hi));
    if (TB.Lo > 0) {
      llvm::Value *Below = B.CreateICmpULT(X, B.getInt64(TB.Lo));
      Above = B.CreateOr(Below, Above);
    }
    Fail(Above);
    return;
  }
  // (to qword, TB is 0..INT64_MAX: the negative values are caught)
  if (FB.Lo >= TB.Lo && FB.Hi <= TB.Hi)
    return; // every value of From fits
  llvm::Value *X = toI64(V, From);
  Fail(outsideRange(X, TB.Lo, TB.Hi));
}


/// Converts a value for assignment / parameter passing: integer -> real,
/// and range-checks ordinal values (and set members) against the target.
llvm::Value *CodeGenModule::convertForAssign(llvm::Value *V, Type *From, Type *To,
                              SourceLocation L){
  if (auto *I = llvm::dyn_cast<InterfaceType>(To)) {
    if (From->isNil())
      return llvm::Constant::getNullValue(memType(To));
    if (From->isClass())
      return toInterface(V, I);
    return V; // an interface (the same or a descendant)
  }
  if (To->isReal() && !From->isReal())
    return toDouble(V, From);
  if (To->isOrdinal() && From->isOrdinal()) {
    if (From->getBase()->isWideChar() && To->getBase()->isChar()) {
      V = wideToChar(V); // then checked as a char
      From = To->getBase();
    }
    emitRangeCheck(V, From, To, L);
    return convertOrdinal(V, From, To);
  }
  if (auto *TS = llvm::dyn_cast<SetType>(To); TS && rangeChecks(L)) {
    OrdinalBounds TB = getOrdinalBounds(TS->getElementType());
    auto *FS = llvm::cast<SetType>(From);
    OrdinalBounds FB = FS->isEmptySet()
                           ? TB
                           : getOrdinalBounds(FS->getElementType()->getBase());
    if (FS->isEmptySet() || (FB.Lo >= TB.Lo && FB.Hi <= TB.Hi))
      return convertSet(V, From, To);
    // (in From's representation: a member beyond it cannot be there)
    unsigned Bits = setBits(From);
    llvm::Value *Outside =
        B.CreateAnd(V, B.CreateNot(constRangeMask(TB.Lo, TB.Hi, Bits)));
    emitRuntimeCheck(
        B.CreateICmpNE(Outside, llvm::ConstantInt::get(B.getIntNTy(Bits), 0)),
        "set member out of range", L);
  }
  if (To->isSet())
    return convertSet(V, From, To);
  return V;
}


/// L op R (Add, Sub or Mul) in a signed or unsigned integer type, with
/// an overflow check if enabled.
llvm::Value *CodeGenModule::emitCheckedArith(BinaryOp Op, bool Signed, llvm::Value *L,
                              llvm::Value *R, SourceLocation Loc,
                              const llvm::Twine &Name){
  llvm::Intrinsic::ID ID;
  switch (Op) {
  case BinaryOp::Add:
    ID = Signed ? llvm::Intrinsic::sadd_with_overflow
                : llvm::Intrinsic::uadd_with_overflow;
    break;
  case BinaryOp::Sub:
    ID = Signed ? llvm::Intrinsic::ssub_with_overflow
                : llvm::Intrinsic::usub_with_overflow;
    break;
  default:
    ID = Signed ? llvm::Intrinsic::smul_with_overflow
                : llvm::Intrinsic::umul_with_overflow;
    break;
  }
  return emitCheckedArith(ID, L, R, Loc, Name);
}


/// L op R with an overflow check (s/u add/sub/mul.with.overflow).
llvm::Value *CodeGenModule::emitCheckedArith(llvm::Intrinsic::ID ID, llvm::Value *L,
                              llvm::Value *R, SourceLocation Loc,
                              const llvm::Twine &Name){
  BinaryOp Op = ID == llvm::Intrinsic::sadd_with_overflow ||
                        ID == llvm::Intrinsic::uadd_with_overflow
                    ? BinaryOp::Add
                : ID == llvm::Intrinsic::ssub_with_overflow ||
                          ID == llvm::Intrinsic::usub_with_overflow
                    ? BinaryOp::Sub
                    : BinaryOp::Mul;
  bool Signed = ID == llvm::Intrinsic::sadd_with_overflow ||
                ID == llvm::Intrinsic::ssub_with_overflow ||
                ID == llvm::Intrinsic::smul_with_overflow;
  ValueRange ResultRange;
  bool Safe = Signed &&
              combineRange(Op, rangeOfValue(L), rangeOfValue(R), ResultRange);
  if (Safe) {
    auto *T = llvm::cast<llvm::IntegerType>(L->getType());
    ValueRange TypeRange;
    if (!signedRange(T->getBitWidth(), TypeRange) ||
        ResultRange.Lo < TypeRange.Lo || ResultRange.Hi > TypeRange.Hi)
      Safe = false;
  }
  if (!overflowChecks(Loc) || Safe) {
    // A proven result gets nsw (optimization3 item 4): LLVM may then use
    // it. An unchecked one wraps, as Pascal's unchecked arithmetic does.
    llvm::Value *Result =
        Op == BinaryOp::Add   ? B.CreateAdd(L, R, Name, false, Safe)
        : Op == BinaryOp::Sub ? B.CreateSub(L, R, Name, false, Safe)
                              : B.CreateMul(L, R, Name, false, Safe);
    if (Safe)
      noteRange(Result, ResultRange);
    return Result;
  }
  llvm::Value *Res = B.CreateBinaryIntrinsic(ID, L, R);
  emitOverflowCheck(B.CreateExtractValue(Res, 1), "integer overflow", Loc);
  llvm::Value *Result = B.CreateExtractValue(Res, 0, Name);
  if (Signed) {
    auto *T = llvm::cast<llvm::IntegerType>(L->getType());
    ValueRange TypeRange;
    if (signedRange(T->getBitWidth(), TypeRange))
      noteRange(Result, TypeRange);
  }
  return Result;
}


/// if (Cond) integer overflow (What): a runtime error, or (objfpc) an
/// EIntOverflow exception, as in Free Pascal with SysUtils.
void CodeGenModule::emitOverflowCheck(llvm::Value *Cond, const std::string &What,
                       SourceLocation L){
  if (!LangOpts.ObjKeywords) {
    emitRuntimeCheck(Cond, What, L);
    return;
  }
  auto *ErrBB = newBlock("rt.error");
  auto *ContBB = newBlock("rt.ok");
  llvm::MDBuilder MDB(M.getContext());
  B.CreateCondBr(Cond, ErrBB, ContBB, MDB.createUnlikelyBranchWeights());
  B.SetInsertPoint(ErrBB);
  B.CreateCall(excRuntime("__pas_raise_overflow"), {whereText(L)});
  B.CreateUnreachable();
  B.SetInsertPoint(ContBB);
}


/// The range of the signed x and y: with a non-negative operand, 0 up to
/// it (the other operand's bits can only clear its bits).
CodeGenModule::ValueRange CodeGenModule::andRange(ValueRange X, ValueRange Y) {
  if (X.Known && X.Lo >= 0 && Y.Known && Y.Lo >= 0)
    return {true, 0, std::min(X.Hi, Y.Hi)};
  if (X.Known && X.Lo >= 0)
    return {true, 0, X.Hi};
  if (Y.Known && Y.Lo >= 0)
    return {true, 0, Y.Hi};
  return {};
}


/// The range of the signed l div c, for a constant c > 0: l's range (or
/// its type's) divided by c; division truncates, which is monotonic.
CodeGenModule::ValueRange CodeGenModule::divRange(llvm::Value *L, llvm::Value *R) const {
  auto *C = llvm::dyn_cast<llvm::ConstantInt>(R);
  if (!C || C->getBitWidth() > 64 || C->getSExtValue() <= 0)
    return {};
  int64_t D = C->getSExtValue();
  ValueRange X = rangeOfValue(L);
  if (!X.Known && !signedRange(L->getType()->getIntegerBitWidth(), X))
    return {};
  return {true, X.Lo / D, X.Hi / D};
}


/// The range of the signed l mod c, for a constant c > 0: 0 .. c-1 for a
/// non-negative l (and in ISO mode, where the result is never negative),
/// else -(c-1) .. c-1 (Turbo/FPC: the sign of l). Within l's own range
/// when that is smaller.
CodeGenModule::ValueRange CodeGenModule::modRange(llvm::Value *L, llvm::Value *R) const {
  auto *C = llvm::dyn_cast<llvm::ConstantInt>(R);
  if (!C || C->getBitWidth() > 64 || C->getSExtValue() <= 0)
    return {};
  int64_t M = C->getSExtValue() - 1;
  ValueRange X = rangeOfValue(L);
  if (LangOpts.Mode == LangMode::ISO)
    return {true, 0, M};
  if (X.Known && X.Lo >= 0)
    return {true, 0, std::min(M, X.Hi)};
  if (X.Known)
    return {true, std::max(-M, std::min(X.Lo, int64_t(0))),
            std::min(M, std::max(X.Hi, int64_t(0)))};
  return {true, -M, M};
}

