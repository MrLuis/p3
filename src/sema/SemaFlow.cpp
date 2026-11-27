// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- SemaFlow.cpp - Semantic analysis -------------------------------------===//

#include "Checker.h"

using namespace p3;
using namespace p3::detail;


/// E, if it is an owned variable (in parentheses or not).
VarDecl *Checker::ownedVarOf(Expr *E){
  while (auto *P = llvm::dyn_cast<ParenExpr>(E))
    E = P->getInner();
  auto *N = llvm::dyn_cast<NameRefExpr>(E);
  auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(N->getDecl()) : nullptr;
  return V && V->isOwned() ? V : nullptr;
}


int Checker::ownedIndex(VarDecl *V){
  if (!Flow || !V->isOwned())
    return -1;
  for (unsigned I = 0; I < Flow->N; ++I)
    if (Flow->Vars[I] == V && !Flow->Null[I])
      return static_cast<int>(I);
  return -1; // an enclosing routine's: not followed
}

void Checker::ownedSet(const OwnedState &S){
  if (Flow)
    Flow->S = S;
}

void Checker::ownedJoin(const OwnedState &S){
  if (Flow)
    Flow->S.join(S);
}

/// What follows is not reached (after exit, break, continue, raise).
void Checker::ownedDead(){
  if (Flow)
    Flow->S = OwnedState();
}


/// A use of the owned variable V: an error if it may have been moved.
void Checker::ownedUse(VarDecl *V, SourceRange R){
  int I = ownedIndex(V);
  if (I < 0 || !Flow->S.isLive())
    return;
  uint64_t Bit = uint64_t(1) << I;
  OwnedState &S = Flow->S;
  if (S.Moved & Bit) {
    Diags.error(R.getBegin(),
                "owned variable '" + V->getName().Spelling +
                    ((S.Valid & Bit) ? "' may have been moved"
                                     : "' was moved"),
                {R});
    Diags.note(Flow->MoveLoc[I], "moved here");
    S.Moved &= ~Bit; // reported once
    S.Valid |= Bit;
    return;
  }
  unsigned D = Flow->Depth;
  for (OwnedLoop *L = Flow->Loop; L && D >= S.Entry[I]; L = L->Prev, --D)
    if (!(L->Used & Bit)) {
      L->Used |= Bit;
      L->EntryUse[I] = R.getBegin();
    }
}


void Checker::ownedChanged(int I){
  Flow->S.Entry[I] = NoLoop;
  Flow->Changed[I] = ++Flow->Tick;
  for (OwnedTry *T = Flow->Try; T; T = T->Prev)
    T->Seen.join(Flow->S);
}


/// V := object.
void Checker::ownedAssign(VarDecl *V){
  int I = ownedIndex(V);
  if (I < 0 || !Flow->S.isLive())
    return;
  Flow->S.Valid |= uint64_t(1) << I;
  Flow->S.Moved &= ~(uint64_t(1) << I);
  ownedChanged(I);
}


/// move V.
void Checker::ownedMove(VarDecl *V, SourceLocation Loc){
  int I = ownedIndex(V);
  if (I < 0 || !Flow->S.isLive())
    return;
  Flow->S.Valid &= ~(uint64_t(1) << I);
  Flow->S.Moved |= uint64_t(1) << I;
  Flow->MoveLoc[I] = Loc;
  ownedChanged(I);
}


/// V, an owned variable of the block being checked, is declared.
void Checker::ownedDeclare(VarDecl *V){
  if (!Flow)
    return;
  if (Flow->N == MaxOwned) {
    Diags.error(V->getLocation(),
                "too many owned variables in one routine (at most " +
                    std::to_string(unsigned(MaxOwned)) + ")",
                {V->getName().getRange()});
    return;
  }
  Flow->Vars[Flow->N++] = V;
  ownedAssign(V); // nil: nothing to free, not moved
}


void Checker::ownedLoopBegin(OwnedLoop &L){
  if (!Flow)
    return;
  L.Prev = Flow->Loop;
  Flow->Loop = &L;
  ++Flow->Depth;
  if (Flow->S.isLive())
    for (unsigned I = 0; I < Flow->N; ++I)
      if (Flow->S.Entry[I] > Flow->Depth)
        Flow->S.Entry[I] = static_cast<uint8_t>(Flow->Depth);
}


/// The loop L ends: Before is the state in which it runs no (more)
/// iterations. A variable that an iteration may leave moved, and that an
/// iteration uses as it finds it, is used after a move.
void Checker::ownedLoopEnd(OwnedLoop &L, const OwnedState &Before){
  if (!Flow)
    return;
  OwnedState Back = Flow->S;
  Back.join(L.Continues);
  for (unsigned I = 0; I < Flow->N; ++I)
    if ((Back.Moved & L.Used) & (uint64_t(1) << I)) {
      if (Flow->Null[I]) {
        Diags.error(L.EntryUse[I],
                    "'" + Flow->Vars[I]->getName().Spelling +
                        "' may be nil here: the loop may set it to nil "
                        "before its next iteration; check it in the loop");
        continue;
      }
      Diags.error(L.EntryUse[I],
                  "owned variable '" + Flow->Vars[I]->getName().Spelling +
                      "' may have been moved by an earlier iteration of "
                      "the loop");
      Diags.note(Flow->MoveLoc[I], "moved here");
    }
  Back.join(Before);
  Back.join(L.Breaks);
  for (unsigned I = 0; I < Flow->N; ++I)
    if (Back.Entry[I] != NoLoop && Back.Entry[I] >= Flow->Depth)
      Back.Entry[I] = NoLoop;
  Flow->Loop = L.Prev;
  --Flow->Depth;
  Flow->S = Back;
}


void Checker::ownedBreak(bool IsContinue){
  if (!Flow || !Flow->Loop)
    return;
  (IsContinue ? Flow->Loop->Continues : Flow->Loop->Breaks).join(Flow->S);
  ownedDead();
}


void Checker::ownedTryBegin(OwnedTry &T){
  if (!Flow)
    return;
  T.Seen = Flow->S;
  T.Prev = Flow->Try;
  Flow->Try = &T;
}

void Checker::ownedTryEnd(OwnedTry &T){
  if (Flow)
    Flow->Try = T.Prev;
}


bool Checker::isNullRef(Type *T){
  return T && (T->isClass() || T->isInterface() || T->isClassRef());
}


bool Checker::isNullTrackedType(Type *T) const {
  return (NullSafeHere && isNullRef(T)) ||
         (NilPointersHere && T && T->isPointer());
}


/// The type written for D (a variable, field, property or function's
/// result), if any.
TypeSpec *Checker::nullSpec(Decl *D){
  if (auto *V = llvm::dyn_cast<VarDecl>(D))
    return V->getTypeSpec();
  if (auto *F = llvm::dyn_cast<FieldDecl>(D))
    return F->getTypeSpec();
  if (auto *P = llvm::dyn_cast<PropertyDecl>(D))
    return P->getTypeSpec();
  if (auto *R = llvm::dyn_cast<RoutineDecl>(D))
    return R->getResultTypeSpec();
  return nullptr;
}

/// Is D declared T? (its value may be nil)?
bool Checker::declNullable(Decl *D){
  TypeSpec *TS = nullSpec(D);
  return TS && TS->isNullable();
}

/// Is D a reference that must not be given nil (written without `?` in
/// null-safe code; not a lambda's result, whose type is not written)?
bool Checker::declNonNull(Decl *D){
  TypeSpec *TS = nullSpec(D);
  Type *T = nullTypeOf(D);
  if (!TS || TS->isNullable())
    return false;
  if (isNullRef(T))
    return TS->isNullSafe();
  if (T && T->isPointer())
    return TS->isNilPointers();
  return false;
}


int Checker::nullIndex(Decl *D){
  if (!Flow || !D)
    return -1;
  for (unsigned I = 0; I < Flow->N; ++I)
    if (Flow->Vars[I] == D && Flow->Null[I])
      return static_cast<int>(I);
  return -1;
}


/// D is followed from here (if there is room: the owned variables keep
/// some), with these states.
void Checker::nullTrack(Decl *D, bool MayNil, bool MayNonNil){
  if (!Flow || Flow->N >= MaxOwned - 8 || nullIndex(D) >= 0)
    return;
  unsigned I = Flow->N++;
  Flow->Vars[I] = D;
  Flow->Null[I] = true;
  nullSet(I, MayNil, MayNonNil);
}


void Checker::nullSet(unsigned I, bool MayNil, bool MayNonNil){
  if (!Flow->S.isLive())
    return;
  uint64_t Bit = uint64_t(1) << I;
  Flow->S.Moved = MayNil ? Flow->S.Moved | Bit : Flow->S.Moved & ~Bit;
  Flow->S.Valid = MayNonNil ? Flow->S.Valid | Bit : Flow->S.Valid & ~Bit;
  Flow->MoveLoc[I] = SourceLocation();
  ownedChanged(I);
}


/// D is not nil here (a condition said so).
void Checker::nullNarrow(Decl *D){
  int I = nullIndex(D);
  if (I < 0 || !Flow->S.isLive())
    return;
  uint64_t Bit = uint64_t(1) << I;
  Flow->S.Moved &= ~Bit;
  Flow->S.Valid |= Bit;
  Flow->S.Entry[I] = NoLoop;
}


/// A use of followed D that needs it not nil: may it be nil? (Once
/// reported, a non-null variable not assigned yet counts as assigned.)
bool Checker::nullUse(unsigned I, SourceLocation Loc, bool Once){
  OwnedState &S = Flow->S;
  if (!S.isLive())
    return false;
  uint64_t Bit = uint64_t(1) << I;
  if (S.Moved & Bit) {
    if (Once) {
      S.Moved &= ~Bit;
      S.Valid |= Bit;
    }
    return true;
  }
  unsigned D = Flow->Depth;
  for (OwnedLoop *L = Flow->Loop; L && D >= S.Entry[I]; L = L->Prev, --D)
    if (!(L->Used & Bit)) {
      L->Used |= Bit;
      L->EntryUse[I] = Loc;
    }
  return false;
}


/// The declaration a reference to a followed or declared value is to:
/// a variable or routine (x), a field of Self (F, Self.F), another
/// member (o.F); null otherwise.
Decl *Checker::nullDeclOf(Expr *E){
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
    return N->getDecl();
  if (auto *F = llvm::dyn_cast<FieldExpr>(E))
    return F->getMember();
  return nullptr;
}

/// E is F or Self.F (in a method).
bool Checker::isSelfMember(Expr *E){
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
    return llvm::isa_and_nonnull<FieldDecl>(N->getDecl()) &&
           (!N->getWith() || N->getWith()->IsSelf);
  if (auto *F = llvm::dyn_cast<FieldExpr>(E))
    if (auto *B = llvm::dyn_cast<NameRefExpr>(F->getBase()))
      return B->getName().Name == "self" &&
             llvm::isa_and_nonnull<FieldDecl>(F->getMember());
  return false;
}


/// The followed declaration E refers to, or -1.
int Checker::nullIndexOf(Expr *E){
  while (auto *P = llvm::dyn_cast<ParenExpr>(E))
    E = P->getInner();
  Decl *D = nullDeclOf(E);
  if (!D)
    return -1;
  if (llvm::isa<FieldDecl>(D) && !isSelfMember(E))
    return -1;
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E);
      N && N->getWith() && !N->getWith()->IsSelf)
    return -1;
  return nullIndex(D);
}


/// May E (checked) be nil here? (A followed reference: as the flow has
/// it, without reporting.)
Checker::Nilness Checker::nilness(Expr *E) {
  while (auto *P = llvm::dyn_cast<ParenExpr>(E))
    E = P->getInner();
  Type *T = E->getType();
  if (T && T->isNil())
    return Nilness::IsNil;
  if (llvm::isa<AddrOfExpr>(E))
    return Nilness::NotNil;
  if (!isNullTrackedType(T))
    return Nilness::NotNil;
  if (int I = nullIndexOf(E); I >= 0) {
    uint64_t Bit = uint64_t(1) << I;
    if (!Flow->S.isLive() || !(Flow->S.Moved & Bit))
      return Nilness::NotNil;
    return (Flow->S.Valid & Bit) ? Nilness::MaybeNil : Nilness::IsNil;
  }
  if (llvm::isa<SafeNavExpr>(E))
    return Nilness::MaybeNil;
  if (auto *C = llvm::dyn_cast<CoalesceExpr>(E))
    return nilness(C->getRight());
  if (auto *C = llvm::dyn_cast<CondExpr>(E)) {
    Nilness A = nilness(C->getThen()), B = nilness(C->getElse());
    return A == B ? A : Nilness::MaybeNil;
  }
  if (auto *B = llvm::dyn_cast<BinaryExpr>(E); B && B->getOp() == BinaryOp::As)
    return nilness(B->getLHS());
  if (auto *C = llvm::dyn_cast<CallExpr>(E)) {
    if (C->getDecl() && llvm::isa<TypeDecl>(C->getDecl()) &&
        C->getArgs().size() == 1)
      return nilness(C->getArgs()[0]);
    Decl *D = C->getDecl();
    return D && declNullable(D) ? Nilness::MaybeNil : Nilness::NotNil;
  }
  Decl *D = nullDeclOf(E);
  if (auto *IC = llvm::dyn_cast<IndirectCallExpr>(E))
    if (auto *F = llvm::dyn_cast<FieldExpr>(IC->getCallee()))
      D = F->getMember();
  return D && declNullable(D) ? Nilness::MaybeNil : Nilness::NotNil;
}


/// How E is named in a diagnostic.
std::string Checker::nullDesc(Expr *E){
  while (auto *P = llvm::dyn_cast<ParenExpr>(E))
    E = P->getInner();
  if (auto *N = llvm::dyn_cast<NameRefExpr>(E))
    return "'" + N->getName().Spelling.str() + "'";
  if (auto *F = llvm::dyn_cast<FieldExpr>(E))
    return "'" + F->getField().Spelling.str() + "'";
  if (auto *C = llvm::dyn_cast<CallExpr>(E)) {
    if (C->getDecl() && llvm::isa<TypeDecl>(C->getDecl()) &&
        C->getArgs().size() == 1)
      return nullDesc(C->getArgs()[0]);
    return "the result of '" + C->getCallee().Spelling.str() + "'";
  }
  if (auto *IC = llvm::dyn_cast<IndirectCallExpr>(E))
    if (auto *F = llvm::dyn_cast<FieldExpr>(IC->getCallee()))
      return "the result of '" + F->getField().Spelling.str() + "'";
  if (llvm::isa<SafeNavExpr>(E))
    return "the value of '?.'";
  if (E->getType() && E->getType()->isNil())
    return "nil";
  return "this value";
}


/// Where E must not be nil (Why: what needs it, "but ..."; Alt: another
/// way out, ", or ..."): an error if it may be.
void Checker::requireNotNil(Expr *E, const std::string &Why,
                   const std::string &Alt){
  if (!anyNullSafety() || !E->getType() || E->getType()->isError())
    return;
  Expr *X = E;
  while (auto *P = llvm::dyn_cast<ParenExpr>(X))
    X = P->getInner();
  if (isUnsafe(X->getBeginLoc()) &&
      (!NullSafeHere || X->getType()->isPointer()))
    return;
  if (X->getType()->isNil()) {
    llvm::StringRef W(Why), A(Alt);
    W.consume_front("but ");
    A.consume_front(", or ");
    Diags.error(X->getBeginLoc(),
                "nil cannot be used here: " + W.str() +
                    (A.empty() ? "" : " (" + A.str() + " if it may be nil)"),
                {X->getRange()});
    return;
  }
  if (!isNullTrackedType(X->getType()))
    return;
  std::string What = nullDesc(X);
  if (int I = nullIndexOf(X); I >= 0) {
    Decl *D = Flow->Vars[I];
    auto *V = llvm::dyn_cast<VarDecl>(D);
    bool Freed = Flow->MoveLoc[I].isValid();
    bool Unassigned = !declNullable(D) && !Freed &&
                      !(V && V->isParam() && !V->getTypeSpec());
    bool IsNil = !(Flow->S.Valid & (uint64_t(1) << I));
    if (!nullUse(static_cast<unsigned>(I), X->getBeginLoc(), Unassigned))
      return;
    if (Unassigned && (!V || V->getTypeSpec()))
      Diags.error(X->getBeginLoc(),
                  What + " may not have been assigned yet (a reference of a "
                         "non-null type must be assigned before it is used)",
                  {X->getRange()});
    else if (IsNil)
      Diags.error(X->getBeginLoc(), What + " is nil here, " + Why,
                  {X->getRange()});
    else
      Diags.error(X->getBeginLoc(),
                  What + " may be nil here, " + Why +
                      "; check it first (if " + What.substr(1, What.size() - 2) +
                      " <> nil then ...)" + Alt,
                  {X->getRange()});
    return;
  }
  Nilness N = nilness(X);
  if (N == Nilness::NotNil)
    return;
  if (N == Nilness::IsNil) {
    llvm::StringRef W(Why), A(Alt);
    W.consume_front("but ");
    A.consume_front(", or ");
    Diags.error(X->getBeginLoc(),
                "nil cannot be used here: " + W.str() +
                    (A.empty() ? "" : " (" + A.str() + " if it may be nil)"),
                {X->getRange()});
    return;
  }
  Diags.error(X->getBeginLoc(),
              What + " may be nil here, " + Why +
                  "; check it first in a local variable" + Alt,
              {X->getRange()});
}


/// Target := Value (or the initialization of a variable): Value must not
/// be nil if Target is a non-null reference; a followed Target gets its
/// nil-ness.
void Checker::nullAssign(Expr *Target, Decl *TD, Expr *Value){
  if (!anyNullSafety() || !TD)
    return;
  Type *T = Value->getType();
  if (!T || T->isError())
    return;
  Type *TT = nullTypeOf(TD);
  if (!isNullTrackedType(TT))
    return;
  if (declNonNull(TD) && !(TT->isPointer() && isUnsafe(Value->getBeginLoc())))
    requireNotNil(Value,
                  "but '" + TD->getName().Spelling.str() +
                      "' is of the non-null type " + quoted(TT),
                  ", or declare '" + TD->getName().Spelling.str() + "' as " +
                      ("'" + TT->getName() + "?'"));
  int I = Target ? nullIndexOf(Target) : nullIndex(TD);
  if (I < 0)
    return;
  Nilness N = nilness(Value);
  nullSet(static_cast<unsigned>(I), N != Nilness::NotNil, N != Nilness::IsNil);
}


Type *Checker::nullTypeOf(Decl *D){
  if (auto *V = llvm::dyn_cast<VarDecl>(D))
    return V->getType();
  if (auto *F = llvm::dyn_cast<FieldDecl>(D))
    return F->getType();
  if (auto *P = llvm::dyn_cast<PropertyDecl>(D))
    return P->getType();
  return nullptr;
}


/// The followed declarations a condition shows to be non-nil when it is
/// true (WhenTrue) or false.
void Checker::nullFacts(Expr *C, bool WhenTrue, llvm::SmallVectorImpl<Decl *> &Out){
  while (auto *P = llvm::dyn_cast<ParenExpr>(C))
    C = P->getInner();
  auto Var = [&](Expr *E) -> Decl * {
    int I = nullIndexOf(E);
    return I >= 0 ? Flow->Vars[I] : nullptr;
  };
  auto IsNil = [](Expr *E) {
    while (auto *P = llvm::dyn_cast<ParenExpr>(E))
      E = P->getInner();
    return E->getType() && E->getType()->isNil();
  };
  if (auto *U = llvm::dyn_cast<UnaryExpr>(C); U && U->getOp() == UnaryOp::Not) {
    nullFacts(U->getOperand(), !WhenTrue, Out);
    return;
  }
  if (auto *B = llvm::dyn_cast<BinaryExpr>(C)) {
    BinaryOp Op = B->getOp();
    if (Op == BinaryOp::Ne || Op == BinaryOp::Eq) {
      if (WhenTrue != (Op == BinaryOp::Ne))
        return;
      Decl *D = IsNil(B->getRHS()) ? Var(B->getLHS())
                : IsNil(B->getLHS()) ? Var(B->getRHS()) : nullptr;
      if (D)
        Out.push_back(D);
      return;
    }
    if (Op == BinaryOp::Is) {
      if (Decl *D = Var(B->getLHS()); D && WhenTrue)
        Out.push_back(D);
      return;
    }
    if (Op == BinaryOp::And || Op == BinaryOp::Or) {
      bool Both = (Op == BinaryOp::And) == WhenTrue; // each side holds
      llvm::SmallVector<Decl *, 4> L, R;
      nullFacts(B->getLHS(), WhenTrue, L);
      nullFacts(B->getRHS(), WhenTrue, R);
      if (Both) {
        Out.append(L.begin(), L.end());
        Out.append(R.begin(), R.end());
      } else {
        for (Decl *D : L)
          if (llvm::is_contained(R, D))
            Out.push_back(D);
      }
      return;
    }
    return;
  }
  if (auto *Call = llvm::dyn_cast<CallExpr>(C); Call && WhenTrue &&
                                                  Call->getArgs().size() == 1)
    if (auto *BD = llvm::dyn_cast_or_null<BuiltinDecl>(Call->getDecl());
        BD && BD->getBuiltinKind() == BuiltinKind::Assigned)
      if (Decl *D = Var(Call->getArgs()[0]))
        Out.push_back(D);
}


void Checker::nullNarrowBy(Expr *C, bool WhenTrue){
  if (!anyNullSafety() || !Flow)
    return;
  llvm::SmallVector<Decl *, 4> Facts;
  nullFacts(C, WhenTrue, Facts);
  for (Decl *D : Facts)
    nullNarrow(D);
}


/// Where the routine being checked ends (its end, or exit): its non-null
/// result, and a constructor's non-null fields, must be set.
void Checker::nullCheckEnd(SourceLocation Loc, bool AtExit){
  if (!anyNullSafety() || !Flow || !Flow->S.isLive() || !CurRoutine)
    return;
  for (unsigned I = 0; I < Flow->N; ++I) {
    if (!Flow->Null[I] || !(Flow->S.Moved & (uint64_t(1) << I)))
      continue;
    Decl *D = Flow->Vars[I];
    auto *V = llvm::dyn_cast<VarDecl>(D);
    bool IsResult = V && V->getVarKind() == VarDecl::Result;
    if (!IsResult && !llvm::isa<FieldDecl>(D))
      continue;
    if (!declNonNull(D))
      continue;
    Type *T = nullTypeOf(D);
    std::string Fix = " (assign it on every path, or declare it " +
                      ("'" + T->getName() + "?'") + ")";
    if (IsResult)
      Diags.error(Loc,
                  "function '" + CurRoutine->getName().Spelling +
                      "' may return nil" + (AtExit ? " here" : "") +
                      ": its result of the non-null type " + quoted(T) +
                      " may not be assigned" + Fix);
    else
      Diags.error(Loc,
                  "constructor '" + CurRoutine->getName().Spelling +
                      "' may leave field '" + D->getName().Spelling +
                      "' of the non-null type " + quoted(T) + " nil" +
                      (AtExit ? " here" : "") + Fix);
    Flow->S.Moved &= ~(uint64_t(1) << I); // reported once
  }
}


/// The block's references that are followed from its start: its
/// variables, its routine's T? parameters, non-null result and (a
/// constructor) own non-null fields.
void Checker::nullTrackBlock(Block &B){
  if (RoutineDecl *R = CurRoutine; R && R->getBody() == &B) {
    for (VarDecl *P : R->getParams())
      if (isNullTrackedType(P->getType()) && declNullable(P))
        nullTrack(P, true, true);
    if (VarDecl *Res = R->getResultVar();
        Res && isNullTrackedType(Res->getType()) && !R->isLambda() &&
        (declNonNull(Res) || declNullable(Res)))
      nullTrack(Res, true, false); // (nil until assigned)
    if (R->isConstructor())
      if (ClassType *C = R->getClass())
        for (Decl *M : C->getMembers())
          if (auto *F = llvm::dyn_cast<FieldDecl>(M);
              F && isNullTrackedType(F->getType()) && declNonNull(F))
            nullTrack(F, true, false);
  }
  for (Decl *D : B.getDecls())
    if (auto *V = llvm::dyn_cast<VarDecl>(D);
        V && V->getVarKind() == VarDecl::Variable &&
        isNullTrackedType(V->getType()) &&
        !V->isTypedConst() && !V->isClassVar() && !V->getAbsolute() &&
        !V->isExternal() && V->getTypeSpec() &&
        (isNullRef(V->getType()) ? V->getTypeSpec()->isNullSafe()
                                 : V->getTypeSpec()->isNilPointers()))
      nullTrack(V, true, false); // nil until assigned
}


/// x passed for var or out parameter P: the callee may rely on it not
/// being nil (var P: T), and leaves it as P's type says.
void Checker::nullVarArg(Expr *Arg, VarDecl *P){
  if (!anyNullSafety() || !isNullTrackedType(P->getType()))
    return;
  if (!P->isOutParam() && declNonNull(P) &&
      !(P->getType()->isPointer() && isUnsafe(Arg->getBeginLoc())))
    requireNotNil(Arg, "but parameter '" + P->getName().Spelling.str() +
                           "' is of the non-null type " +
                           quoted(P->getType()));
  if (int I = nullIndexOf(Arg); I >= 0) {
    bool NonNull = declNonNull(P);
    nullSet(static_cast<unsigned>(I), !NonNull, true);
  }
}


/// Can a value of type T be nil, and be tested for it (?., ??)?
bool Checker::isNilable(Type *T){
  return T->isClass() || T->isInterface() || T->isClassRef() ||
         T->isPointer();
}


/// A reference to hidden variable V (of type T) at Loc.
NameRefExpr *Checker::hiddenRef(VarDecl *V, SourceLocation Loc){
  Identifier Id = V->getName();
  Id.Loc = Loc;
  auto *N = Ctx.create<NameRefExpr>(Id);
  N->setDecl(V);
  N->setType(V->getType());
  return N;
}


/// x?.m: m's type (null for a statement's call).
Type *Checker::checkSafeNav(SafeNavExpr *E){
  Type *BT = check(E->getBase());
  if (BT->isError())
    return BT;
  if (!BT->isClass() && !BT->isInterface() && !BT->isClassRef()) {
    Diags.error(E->getHole()->getBeginLoc(),
                "'?.' needs a class or interface reference, not a value of "
                "type " + quoted(BT),
                {E->getBase()->getRange()});
    return errorType();
  }
  VarDecl *Tmp = makeHiddenVar(BT, E->getBeginLoc());
  if (!Tmp) {
    Diags.error(E->getHole()->getBeginLoc(),
                "'?.' can only be written in the statements of a routine "
                "or program",
                {E->getRange()});
    return errorType();
  }
  Scope NavScope(Cur);
  NavScope.insert(E->getHole()->getName().Name, Tmp);
  Scope *Saved = Cur;
  Cur = &NavScope;
  if (E->isStatement()) { // the call, where x is not nil
    Expr *A = E->getAccess();
    Stmt *Call;
    if (auto *IC = llvm::dyn_cast<IndirectCallExpr>(A))
      Call = Ctx.create<IndirectCallStmt>(A->getRange(), IC);
    else
      Call = Ctx.create<ExprStmt>(A->getRange(), A);
    visitStmt(Call);
    Cur = Saved;
    E->setLowered(Tmp, nullptr, nullptr, Call);
    return nullptr;
  }
  Type *AT = check(E->getAccess());
  Cur = Saved;
  if (AT->isError())
    return AT;
  if (AT->isStructured() || AT->isStringLiteral()) {
    if (AT->isStringLiteral())
      AT = E->getAccess()->getType();
    if (AT->isStructured()) {
      Diags.error(E->getHole()->getBeginLoc(),
                  "'?.' gives a value of a simple type, a string or a "
                  "reference, not of type " + quoted(AT),
                  {E->getAccess()->getRange()});
      return errorType();
    }
  }
  VarDecl *Res = makeHiddenVar(AT, E->getBeginLoc());
  E->setLowered(Tmp, Res, hiddenRef(Res, E->getBeginLoc()), nullptr);
  return AT;
}


/// a ?? b: a, or b where a is nil. The type: a's, b's value being
/// assignable to it (or b's, if a's is).
Type *Checker::visitCoalesceExpr(CoalesceExpr *E){
  Expr *L = E->getLeft();
  while (auto *P = llvm::dyn_cast<ParenExpr>(L))
    L = P->getInner();
  auto *Nav = llvm::dyn_cast<SafeNavExpr>(L);
  Type *LT = check(E->getLeft());
  if (LT->isError()) {
    check(E->getRight());
    return LT;
  }
  bool ByNil = isNilable(LT);
  if (!ByNil && !Nav) {
    Diags.error(E->getOpLoc(),
                "the left side of '??' must be a reference that may be nil "
                "(a class, interface or pointer) or a '?.' chain, not a "
                "value of type " + quoted(LT),
                {E->getLeft()->getRange()});
    check(E->getRight());
    return errorType();
  }
  Type *RT = checkValueFor(LT, E->getRight());
  if (RT->isError())
    return RT;
  Type *T = nullptr;
  if (isAssignable(LT, RT) || convertImplicitly(E->getRight(), LT))
    T = LT;
  else if (isInferable(RT) && isAssignable(RT, LT))
    T = RT;
  if (!T) {
    Diags.error(E->getOpLoc(),
                "the right side of '??' must be assignable to " + quoted(LT) +
                    ", not a value of type " + quoted(RT),
                {E->getRight()->getRange()});
    return errorType();
  }
  VarDecl *Res = makeHiddenVar(T, E->getBeginLoc());
  if (!Res) {
    Diags.error(E->getOpLoc(),
                "'??' can only be written in the statements of a routine "
                "or program",
                {E->getRange()});
    return errorType();
  }
  if (!ByNil) { // the last `?.` of the chain tells whether it got there
    while (auto *Inner = llvm::dyn_cast_or_null<SafeNavExpr>(Nav->getAccess()))
      Nav = Inner;
    Nav->setOk(makeHiddenVar(Ctx.getBooleanType(), E->getBeginLoc()));
  }
  E->setLowered(Res, hiddenRef(Res, E->getBeginLoc()));
  return T;
}

