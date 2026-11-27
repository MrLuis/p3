// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- SemaBuiltins.cpp - Semantic analysis -------------------------------------===//

#include "Checker.h"

using namespace p3;
using namespace p3::detail;


/// Can E's address be passed for a `constref` parameter: a variable (not
/// a bit field or a property)?
bool Checker::isConstRefAddressable(Expr *E){
  if (auto *F = llvm::dyn_cast<FieldExpr>(E);
      F && !llvm::isa_and_nonnull<FieldDecl>(F->getMember()) &&
      !llvm::isa_and_nonnull<VarDecl>(F->getMember()))
    return false;
  return isVariableReference(E);
}


//===--------------------------------------------------------------------===//
// Built-in procedure calls
//===--------------------------------------------------------------------===//

bool Checker::isWritable(Type *T){
  return T->isIntegerLike() || T->isReal() || T->isCharLike() ||
         T->getBase()->isBoolean() || T->isStringType() || T->isShortString() ||
         T->isAnsiString();
}


/// If the first argument of read/write/... is a file, checks and returns
/// it; otherwise returns null.
FileType *Checker::checkFileArg(ProcCallStmt *S){
  if (S->getArgs().empty() || llvm::isa<FormattedExpr>(S->getArgs()[0]))
    return nullptr;
  Expr *First = S->getArgs()[0];
  Type *T = check(First);
  auto *F = llvm::dyn_cast<FileType>(T);
  if (F && !isVariableReference(First)) {
    Diags.error(First->getBeginLoc(), "file argument must be a variable",
                {First->getRange()});
    return nullptr;
  }
  return F;
}


/// write/read of an untyped file: an error (they use blockwrite and
/// blockread). True if reported.
bool Checker::untypedFileIO(ProcCallStmt *S, FileType *File, bool Write){
  if (!File || !File->isUntyped())
    return false;
  Diags.error(S->getCallee().Loc,
              "'" + S->getCallee().Spelling + "' cannot be used on an untyped " +
                  "file; use '" + (Write ? "blockwrite" : "blockread") + "'",
              {S->getCallee().getRange()});
  return true;
}


void Checker::checkWrite(ProcCallStmt *S, bool IsWriteln){
  const Identifier &Callee = S->getCallee();
  FileType *File = checkFileArg(S);
  if (untypedFileIO(S, File, /*Write=*/true))
    return;
  auto Args = S->getArgs().drop_front(File ? 1 : 0);
  if (!IsWriteln && Args.empty())
    Diags.error(Callee.Loc,
                "'" + Callee.Spelling + "' requires at least one argument",
                {Callee.getRange()});
  if (File && !File->isText()) {
    if (IsWriteln)
      Diags.error(Callee.Loc, "'writeln' requires a text file",
                  {Callee.getRange()});
    for (Expr *Arg : Args) {
      if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
        Diags.error(F->getWidth()->getBeginLoc(),
                    "field widths are only allowed for text files",
                    {F->getWidth()->getRange()});
        continue;
      }
      Type *T = check(Arg);
      if (!T->isError() && !isAssignable(File->getElementType(), T))
        Diags.error(Arg->getBeginLoc(),
                    "cannot write a value of type " + quoted(T) +
                        " to a " + quoted(File),
                    {Arg->getRange()});
    }
    return;
  }

  for (Expr *Arg : Args)
    checkWritten(Arg, /*Template=*/false);
}


/// A value written to a text file, or put in a template string, with its
/// field width and precision if it has them (a FormattedExpr).
void Checker::checkWritten(Expr *Arg, bool Template){
  Expr *Value = Arg, *Width = nullptr, *Precision = nullptr;
  auto *F = llvm::dyn_cast<FormattedExpr>(Arg);
  if (F) {
    Value = F->getValue();
    Width = F->getWidth();
    Precision = F->getPrecision();
  }

  Type *T = check(Value);
  if (F)
    F->setType(T);
  // Turbo/FPC: an enumeration's value is written as its name, a pchar
  // as the C string it points to.
  if (!T->isError() && !isWritable(T) &&
      (isISO() || !(T->getBase()->isEnum() || isPChar(T)))) {
    if (Template)
      Diags.error(Value->getBeginLoc(),
                  "a value of type " + quoted(T) +
                      " cannot be put in a template string",
                  {Value->getRange()});
    else
      Diags.error(Value->getBeginLoc(),
                  "cannot write a value of type " + quoted(T),
                  {Value->getRange()});
  }

  for (Expr *W : {Width, Precision}) {
    if (!W)
      continue;
    Type *WT = check(W);
    if (!WT->isError() && !WT->isIntegerLike())
      Diags.error(W->getBeginLoc(),
                  llvm::Twine(W == Width ? "field width" : "precision") +
                      " must be an integer, not " + quoted(WT),
                  {W->getRange()});
  }
  // ISO 6.9.3.4: only real values take a fraction-digits argument.
  if (Precision && !T->isError() && !T->isReal())
    Diags.error(Precision->getBeginLoc(),
                "a precision (':n') is only allowed for real values",
                {Precision->getRange()});
}


void Checker::checkRead(ProcCallStmt *S, bool IsReadln){
  const Identifier &Callee = S->getCallee();
  FileType *File = checkFileArg(S);
  if (untypedFileIO(S, File, /*Write=*/false))
    return;
  auto Args = S->getArgs().drop_front(File ? 1 : 0);
  if (!IsReadln && Args.empty())
    Diags.error(Callee.Loc,
                "'" + Callee.Spelling + "' requires at least one argument",
                {Callee.getRange()});
  if (File && !File->isText()) {
    if (IsReadln)
      Diags.error(Callee.Loc, "'readln' requires a text file",
                  {Callee.getRange()});
    for (Expr *Arg : Args) {
      Type *T = check(Arg);
      if (T->isError())
        continue;
      if (!isVariableReference(Arg))
        Diags.error(Arg->getBeginLoc(),
                    "argument to '" + Callee.Spelling + "' must be a variable",
                    {Arg->getRange()});
      else if (!isAssignable(T, File->getElementType()))
        Diags.error(Arg->getBeginLoc(),
                    "cannot read a component of " + quoted(File) +
                        " into a variable of type " + quoted(T),
                    {Arg->getRange()});
      else
        checkNotForControlVar(Arg);
    }
    return;
  }

  for (Expr *Arg : Args) {
    if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
      Diags.error(F->getWidth()->getBeginLoc(),
                  "field widths are only allowed in 'write' and 'writeln'",
                  {F->getWidth()->getRange()});
      continue;
    }
    Type *T = check(Arg);
    if (T->isError())
      continue;
    if (!isVariableReference(Arg)) {
      Diags.error(Arg->getBeginLoc(),
                  "argument to '" + Callee.Spelling + "' must be a variable",
                  {Arg->getRange()});
      continue;
    }
    if (!T->isIntegerLike() && !T->getBase()->isChar() && !T->isReal() &&
        !T->isShortString() && !T->isAnsiString())
      Diags.error(Arg->getBeginLoc(),
                  "cannot read a value of type " + quoted(T) +
                      " (only integer, real and char)",
                  {Arg->getRange()});
    else if (checkModifiable(Arg))
      checkNotForControlVar(Arg);
  }
}


/// reset/rewrite/get/put(f) and page[(f)].
/// reset, rewrite (an untyped file's: with an optional record size),
/// get, put, page, close, closefile, flush.
void Checker::checkFileProc(ProcCallStmt *S, BuiltinKind K){
  const Identifier &Callee = S->getCallee();
  auto Args = S->getArgs();
  bool IsPage = K == BuiltinKind::Page;
  if (IsPage && Args.empty())
    return; // page(output)
  bool Sized = K == BuiltinKind::Reset || K == BuiltinKind::Rewrite;
  if (Args.size() != 1 && !(Sized && Args.size() == 2)) {
    Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 1 file argument",
                {S->getRange()});
    return;
  }
  Type *T = check(Args[0]);
  if (T->isError())
    return;
  auto *F = llvm::dyn_cast<FileType>(T);
  if (!F || !isVariableReference(Args[0]))
    Diags.error(Args[0]->getBeginLoc(),
                "'" + Callee.Spelling + "' expects a file variable, not a " +
                    "value of type " + quoted(T),
                {Args[0]->getRange()});
  else if ((IsPage || K == BuiltinKind::Flush) && !T->isTextFile())
    Diags.error(Args[0]->getBeginLoc(),
                "'" + Callee.Spelling + "' requires a text file",
                {Args[0]->getRange()});
  else if ((K == BuiltinKind::Get || K == BuiltinKind::Put) && F->isUntyped())
    Diags.error(Args[0]->getBeginLoc(),
                "'" + Callee.Spelling + "' cannot be used on an untyped file",
                {Args[0]->getRange()});
  else if (Args.size() == 2 && !F->isUntyped())
    Diags.error(Args[1]->getBeginLoc(),
                "only an untyped file has a record size",
                {Args[1]->getRange()});
  else if (Args.size() == 2)
    checkIntegerArg(Args[1], Callee, "record size");
}


/// An integer argument of built-in Callee (What: its role).
void Checker::checkIntegerArg(Expr *E, const Identifier &Callee, llvm::StringRef What){
  Type *T = check(E);
  if (!T->isError() && !T->isIntegerLike())
    Diags.error(E->getBeginLoc(),
                "the " + What + " of '" + Callee.Spelling +
                    "' must be an integer, not " + quoted(T),
                {E->getRange()});
}


/// A file variable argument of Callee: the file type, or null after a
/// diagnostic. NotText: text files are not allowed.
FileType *Checker::checkFileVarArg(Expr *E, const Identifier &Callee,
                          bool NotText, bool Untyped){
  Type *T = check(E);
  if (T->isError())
    return nullptr;
  auto *F = llvm::dyn_cast<FileType>(T);
  if (!F || !isVariableReference(E)) {
    Diags.error(E->getBeginLoc(),
                "'" + Callee.Spelling + "' expects " +
                    (Untyped ? "an untyped " : "a ") + "file variable, not a " +
                    "value of type " + quoted(T),
                {E->getRange()});
    return nullptr;
  }
  if ((Untyped && !F->isUntyped()) || (NotText && F->isText())) {
    Diags.error(E->getBeginLoc(),
                "'" + Callee.Spelling + "' expects " +
                    (Untyped ? "an untyped file" : "a typed or untyped file") +
                    ", not " + quoted(T),
                {E->getRange()});
    return nullptr;
  }
  return F;
}


/// assign(f, name) / assignfile: f any file variable, name a string, a
/// char or a pchar.
void Checker::checkAssignFile(ProcCallStmt *S){
  const Identifier &Callee = S->getCallee();
  auto Args = S->getArgs();
  if (Args.size() != 2) {
    Diags.error(Callee.Loc,
                "'" + Callee.Spelling + "' expects a file variable and a name",
                {S->getRange()});
    return;
  }
  checkFileVarArg(Args[0], Callee);
  Type *T = check(Args[1]);
  if (!T->isError() && !T->isStringish() && T != PCharTy)
    Diags.error(Args[1]->getBeginLoc(),
                "the file name of '" + Callee.Spelling +
                    "' must be a string, not " + quoted(T),
                {Args[1]->getRange()});
}


/// blockread(f, buf, count [, done]) / blockwrite: f an untyped file, buf
/// any variable, count an integer, done an integer variable.
void Checker::checkBlockIO(ProcCallStmt *S){
  const Identifier &Callee = S->getCallee();
  auto Args = S->getArgs();
  if (Args.size() != 3 && Args.size() != 4) {
    Diags.error(Callee.Loc,
                "'" + Callee.Spelling + "' expects 3 or 4 arguments (file, "
                "buffer, count [, count done])",
                {S->getRange()});
    return;
  }
  checkFileVarArg(Args[0], Callee, /*NotText=*/true, /*Untyped=*/true);
  if (!checkAllowingUntyped(Args[1])->isError() &&
      !isVariableReference(Args[1]))
    Diags.error(Args[1]->getBeginLoc(),
                "the buffer of '" + Callee.Spelling + "' must be a variable",
                {Args[1]->getRange()});
  else if (isBitField(Args[1]))
    diagBitFieldAddress(Args[1]);
  else if (S->getCallee().Name == "blockread")
    checkModifiable(Args[1]);
  checkIntegerArg(Args[2], Callee, "count");
  if (Args.size() == 4) {
    Type *T = check(Args[3]);
    if (T->isError())
      return;
    if (!isVariableReference(Args[3]))
      Diags.error(Args[3]->getBeginLoc(),
                  "the count done by '" + Callee.Spelling +
                      "' goes in a variable",
                  {Args[3]->getRange()});
    else if (!T->isIntegerLike())
      Diags.error(Args[3]->getBeginLoc(),
                  "the count done by '" + Callee.Spelling +
                      "' goes in an integer variable, not " + quoted(T),
                  {Args[3]->getRange()});
    else
      checkModifiable(Args[3]);
  }
}


/// seek(f, n): f a typed or untyped file, n an integer.
void Checker::checkSeek(ProcCallStmt *S){
  const Identifier &Callee = S->getCallee();
  auto Args = S->getArgs();
  if (Args.size() != 2) {
    Diags.error(Callee.Loc,
                "'seek' expects a file variable and a record number",
                {S->getRange()});
    return;
  }
  checkFileVarArg(Args[0], Callee, /*NotText=*/true);
  checkIntegerArg(Args[1], Callee, "record number");
}


/// pack(a, i, z): z := a[i..]   unpack(z, a, i): a[i..] := z
void Checker::checkPackUnpack(ProcCallStmt *S, bool IsPack){
  const Identifier &Callee = S->getCallee();
  auto Args = S->getArgs();
  if (Args.size() != 3) {
    Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 3 arguments",
                {S->getRange()});
    return;
  }
  Expr *Unpacked = IsPack ? Args[0] : Args[1];
  Expr *Index = IsPack ? Args[1] : Args[2];
  Expr *Packed = IsPack ? Args[2] : Args[0];
  Type *UT = check(Unpacked), *IT = check(Index), *PT = check(Packed);
  if (UT->isError() || IT->isError() || PT->isError())
    return;
  auto *UA = llvm::dyn_cast<ArrayType>(UT);
  auto *PA = llvm::dyn_cast<ArrayType>(PT);
  if (!UA || UA->isPacked() || !isVariableReference(Unpacked)) {
    Diags.error(Unpacked->getBeginLoc(), "expected an unpacked array variable",
                {Unpacked->getRange()});
    return;
  }
  if (!PA || !PA->isPacked() || !isVariableReference(Packed)) {
    Diags.error(Packed->getBeginLoc(), "expected a packed array variable",
                {Packed->getRange()});
    return;
  }
  if (UA->getElementType() != PA->getElementType())
    Diags.error(Packed->getBeginLoc(),
                "packed and unpacked arrays must have the same component type",
                {Packed->getRange()});
  if (!areCompatible(IT, UA->getIndexType()))
    Diags.error(Index->getBeginLoc(),
                "index of type " + quoted(IT) + " does not match " +
                    quoted(UA->getIndexType()),
                {Index->getRange()});
}


/// new(p [, tag constants]) / dispose(p [, tag constants]). The tags
/// select record variants (ISO 6.6.5.3); storage is always allocated for
/// the largest variant, so they are only checked.
/// delete(var s, index, count) / insert(source, var s, index)
void Checker::checkDeleteInsert(ProcCallStmt *S, bool IsInsert){
  const Identifier &Callee = S->getCallee();
  auto Args = S->getArgs();
  if (Args.size() != 3) {
    Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 3 arguments",
                {S->getRange()});
    return;
  }
  Expr *Str = IsInsert ? Args[1] : Args[0];
  for (Expr *A : Args) {
    Type *T = check(A);
    if (T->isError())
      continue;
    if (A == Str) {
      if (!(T->isShortString() || T->isAnsiString()) || !isVariableReference(A))
        Diags.error(A->getBeginLoc(), "expected a string variable",
                    {A->getRange()});
      else
        checkModifiable(A);
    } else if (IsInsert && A == Args[0]) {
      if (!T->isStringish())
        Diags.error(A->getBeginLoc(), "expected a string", {A->getRange()});
    } else if (!T->isIntegerLike()) {
      Diags.error(A->getBeginLoc(), "expected an integer", {A->getRange()});
    }
  }
}


/// setlength(a, n, ...): a a dynamic array variable, one length for each
/// level of arrays of arrays that is set.
void Checker::checkDynSetLength(ProcCallStmt *S){
  auto Args = S->getArgs();
  Type *T = Args[0]->getType();
  if (!isVariableReference(Args[0]))
    Diags.error(Args[0]->getBeginLoc(), "expected a dynamic array variable",
                {Args[0]->getRange()});
  else
    checkModifiable(Args[0]);
  if (Args.size() < 2) {
    Diags.error(S->getCallee().Loc,
                "'" + S->getCallee().Spelling + "' expects a length",
                {S->getRange()});
    return;
  }
  for (size_t I = 1; I < Args.size(); ++I) {
    Type *N = check(Args[I]);
    if (!N->isError() && !N->isIntegerLike())
      Diags.error(Args[I]->getBeginLoc(), "expected an integer",
                  {Args[I]->getRange()});
    if (!T->isDynArray()) {
      Diags.error(Args[I]->getBeginLoc(),
                  "more lengths than levels of dynamic arrays",
                  {Args[I]->getRange()});
      return;
    }
    T = llvm::cast<DynArrayType>(T)->getElementType();
  }
}


/// setlength(s, n): s a string variable, n an integer.
void Checker::checkSetLength(ProcCallStmt *S){
  auto Args = S->getArgs();
  if (!Args.empty() && check(Args[0])->isDynArray())
    return checkDynSetLength(S);
  if (Args.size() != 2) {
    Diags.error(S->getCallee().Loc,
                "'" + S->getCallee().Spelling + "' expects 2 arguments",
                {S->getRange()});
    return;
  }
  Type *T = check(Args[0]);
  Type *N = check(Args[1]);
  if (!T->isError()) {
    if (!(T->isShortString() || T->isAnsiString()) ||
        !isVariableReference(Args[0]))
      Diags.error(Args[0]->getBeginLoc(), "expected a string variable",
                  {Args[0]->getRange()});
    else
      checkModifiable(Args[0]);
  }
  if (!N->isError() && !N->isIntegerLike())
    Diags.error(Args[1]->getBeginLoc(), "expected an integer",
                {Args[1]->getRange()});
}


/// inc(x [, n]) / dec(x [, n]): x an ordinal variable (Turbo/FPC: or a
/// pointer, moved by n of what it points to), n an integer.
void Checker::checkIncDec(ProcCallStmt *S){
  const Identifier &Callee = S->getCallee();
  auto Args = S->getArgs();
  if (Args.empty() || Args.size() > 2) {
    Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 1 or 2 arguments",
                {S->getRange()});
    return;
  }
  Type *T = check(Args[0]);
  if (!T->isError() &&
      (!(T->isOrdinal() || T->isPointer()) || !isVariableReference(Args[0])))
    Diags.error(Args[0]->getBeginLoc(),
                "'" + Callee.Spelling + "' expects an ordinal or pointer variable",
                {Args[0]->getRange()});
  else if (!T->isError() && checkModifiable(Args[0])) {
    checkNotForControlVar(Args[0]);
    if (T->isPointer() && isPointerMathOff(S->getBeginLoc()))
      Diags.error(Callee.Loc,
                  "pointer arithmetic is disabled under {$pointermath off}",
                  {S->getRange()});
    else if (T->isPointer()) {
      if (NilPointersHere)
        requireNotNil(Args[0], "but pointer arithmetic is performed on it");
      if (!isPointerMathChecked(S->getBeginLoc()))
        warnUnsafe(Callee.Loc, "unchecked pointer arithmetic outside 'unsafe'",
                   {S->getRange()});
    }
  }
  if (Args.size() == 2) {
    Type *N = check(Args[1]);
    if (!N->isError() && !N->isIntegerLike())
      Diags.error(Args[1]->getBeginLoc(), "step must be an integer",
                  {Args[1]->getRange()});
  }
}


void Checker::checkNewDispose(ProcCallStmt *S, bool IsNew){
  const Identifier &Callee = S->getCallee();
  if (S->getArgs().empty()) {
    Diags.error(Callee.Loc,
                "'" + Callee.Spelling + "' expects a pointer argument",
                {S->getRange()});
    return;
  }
  Expr *Arg = S->getArgs()[0];
  Type *T = check(Arg);
  if (!T->isError()) {
    if (!T->isPointer())
      Diags.error(Arg->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects a pointer " +
                      (IsNew ? "variable" : "") + ", not a value of type " +
                      quoted(T),
                  {Arg->getRange()});
    else if (IsNew && !isVariableReference(Arg))
      Diags.error(Arg->getBeginLoc(), "argument to 'new' must be a variable",
                  {Arg->getRange()});
    else if (IsNew) {
      if (checkModifiable(Arg) && NilPointersHere)
        if (int I = nullIndexOf(Arg); I >= 0)
          nullSet(static_cast<unsigned>(I), false, true);
    } else if (NilPointersHere) {
      requireNotNil(Arg, "but it is passed to '" + Callee.Spelling.str() + "'");
      if (int I = nullIndexOf(Arg); I >= 0) {
        nullSet(static_cast<unsigned>(I), true, false);
        Flow->MoveLoc[I] = Arg->getBeginLoc();
      }
    }
  }
  for (Expr *Tag : S->getArgs().drop_front())
    evaluateConstant(Tag, "variant selector");
}


//===--------------------------------------------------------------------===//
// Built-in function calls
//===--------------------------------------------------------------------===//

Type *Checker::checkStringFunction(CallExpr *E, BuiltinDecl *BD,
                          llvm::ArrayRef<Type *> ArgTypes){
  const Identifier &Callee = E->getCallee();
  auto Args = E->getArgs();
  for (Type *T : ArgTypes)
    if (T->isError())
      return errorType();
  auto Bad = [&](Expr *A, llvm::StringRef What) {
    Diags.error(A->getBeginLoc(),
                "'" + Callee.Spelling + "' expects " + What, {A->getRange()});
    return errorType();
  };
  auto Arity = [&](size_t N) {
    if (Args.size() == N)
      return true;
    Diags.error(Callee.Loc,
                "'" + Callee.Spelling + "' expects " + llvm::Twine(N) +
                    " argument" + (N == 1 ? "" : "s"),
                {E->getRange()});
    return false;
  };
  switch (BD->getBuiltinKind()) {
  case BuiltinKind::Length:
    if (!Arity(1))
      return errorType();
    return ArgTypes[0]->isStringish() || ArgTypes[0]->isArrayOfConst() ||
                   ArgTypes[0]->isOpenArray() || ArgTypes[0]->isDynArray()
               ? Ctx.getIntegerType()
               : Bad(Args[0], "a string");
  case BuiltinKind::UpCase:
    if (!Arity(1))
      return errorType();
    if (ArgTypes[0]->getBase()->isWideChar())
      return Ctx.getWideCharType(); // only 'a'..'z' change
    return ArgTypes[0]->getBase()->isChar() ? Ctx.getCharType()
                                            : Bad(Args[0], "a char");
  case BuiltinKind::Pos:
    if (!Arity(2))
      return errorType();
    for (size_t I = 0; I < 2; ++I)
      if (!ArgTypes[I]->isStringish())
        return Bad(Args[I], "strings");
    return Ctx.getIntegerType();
  case BuiltinKind::Copy:
    if (!ArgTypes.empty() && ArgTypes[0]->isDynArray()) {
      // copy(a), copy(a, start), copy(a, start, count)
      if (Args.size() > 3) {
        Diags.error(Callee.Loc, "'copy' expects at most 3 arguments", {E->getRange()});
        return errorType();
      }
      for (size_t I = 1; I < Args.size(); ++I)
        if (!ArgTypes[I]->isIntegerLike())
          return Bad(Args[I], "integer index and count");
      return ArgTypes[0];
    }
    if (!Arity(3))
      return errorType();
    if (!ArgTypes[0]->isStringish())
      return Bad(Args[0], "a string");
    if (!ArgTypes[1]->isIntegerLike() || !ArgTypes[2]->isIntegerLike())
      return Bad(Args[1], "integer index and count");
    return ArgTypes[0]->isAnsiString() ? Ctx.getAnsiStringType()
                                       : Ctx.getShortStringType();
  default: // Concat
    if (Args.empty()) {
      Diags.error(Callee.Loc, "'concat' expects strings", {E->getRange()});
      return errorType();
    }
    for (size_t I = 0; I < Args.size(); ++I)
      if (!ArgTypes[I]->isStringish())
        return Bad(Args[I], "strings");
    for (Type *T : ArgTypes)
      if (T->isAnsiString())
        return Ctx.getAnsiStringType();
    return Ctx.getShortStringType();
  }
}


bool Checker::isSliceCall(Expr *E){
  auto *C = llvm::dyn_cast<CallExpr>(E);
  return C && C->getCallee().Name == "slice" &&
         llvm::isa_and_nonnull<BuiltinDecl>(Cur->lookup("slice"));
}


Type *Checker::checkSlice(CallExpr *E, llvm::ArrayRef<Type *> ArgTypes){
  auto Args = E->getArgs();
  const Identifier &Callee = E->getCallee();
  bool Allowed = AllowSlice;
  AllowSlice = false;
  if (Args.size() != 2) {
    Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 2 arguments",
                {E->getRange()});
    return errorType();
  }
  if (ArgTypes[0]->isError() || ArgTypes[1]->isError())
    return errorType();
  if (!Allowed) {
    Diags.error(Callee.Loc,
                "'" + Callee.Spelling +
                    "' can only be passed to an open array parameter",
                {E->getRange()});
    return errorType();
  }
  Type *Elem = nullptr;
  if (auto *A = llvm::dyn_cast<ArrayType>(ArgTypes[0]))
    Elem = A->getElementType();
  else if (auto *O = llvm::dyn_cast<OpenArrayType>(ArgTypes[0]))
    Elem = O->getElementType();
  if (!Elem) {
    Diags.error(Args[0]->getBeginLoc(),
                "'" + Callee.Spelling + "' expects an array, not " +
                    quoted(ArgTypes[0]),
                {Args[0]->getRange()});
    return errorType();
  }
  if (!ArgTypes[1]->isIntegerLike()) {
    Diags.error(Args[1]->getBeginLoc(), "expected an integer",
                {Args[1]->getRange()});
    return errorType();
  }
  return Ctx.getOpenArrayType(Elem);
}


Type *Checker::checkBuiltinFunction(CallExpr *E, BuiltinDecl *BD,
                           llvm::ArrayRef<Type *> ArgTypes){
  if (BD->getBuiltinKind() == BuiltinKind::Slice)
    return checkSlice(E, ArgTypes);
  const Identifier &Callee = E->getCallee();
  switch (BD->getBuiltinKind()) {
  case BuiltinKind::Length: case BuiltinKind::Copy: case BuiltinKind::Pos:
  case BuiltinKind::Concat: case BuiltinKind::UpCase:
    return checkStringFunction(E, BD, ArgTypes);
  default:
    break;
  }
  if (ArgTypes.size() != 1) {
    Diags.error(Callee.Loc,
                "'" + Callee.Spelling + "' expects 1 argument, got " +
                    llvm::Twine(ArgTypes.size()),
                {E->getRange()});
    return errorType();
  }
  Type *T = ArgTypes[0];
  if (T->isError())
    return errorType();
  Expr *Arg = E->getArgs()[0];

  if (BD->getBuiltinKind() == BuiltinKind::FilePos ||
      BD->getBuiltinKind() == BuiltinKind::FileSize) {
    if (!checkFileVarArg(Arg, Callee, /*NotText=*/true))
      return errorType();
    return Ctx.getInt64Type(); // (in records)
  }
  if (BD->getBuiltinKind() == BuiltinKind::GetFileHandle) {
    // any file variable, text too; a THandle (longint, as in FPC on Unix)
    if (!checkFileVarArg(Arg, Callee))
      return errorType();
    return Ctx.getIntegerType();
  }
  if (BD->getBuiltinKind() == BuiltinKind::Eof ||
      BD->getBuiltinKind() == BuiltinKind::Eoln) {
    bool IsEoln = BD->getBuiltinKind() == BuiltinKind::Eoln;
    if (!T->isFile() || !isVariableReference(Arg) ||
        (IsEoln && !T->isTextFile())) {
      Diags.error(Arg->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects a " +
                      (IsEoln ? "text " : "") + "file variable, not " +
                      quoted(T),
                  {Arg->getRange()});
      return errorType();
    }
    return Ctx.getBooleanType();
  }

  auto Expect = [&](bool OK, llvm::StringRef What) {
    if (!OK)
      Diags.error(Arg->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects " + What +
                      " argument, not " + quoted(T),
                  {Arg->getRange()});
    return OK;
  };

  switch (BD->getBuiltinKind()) {
  case BuiltinKind::Abs:
  case BuiltinKind::Sqr:
    if (!Expect(T->isNumeric(), "a numeric"))
      return errorType();
    return T->isReal() ? Ctx.getRealType() : promoteInteger(T);
  case BuiltinKind::Odd:
    return Expect(T->isIntegerLike(), "an integer") ? Ctx.getBooleanType()
                                                    : errorType();
  case BuiltinKind::Ord:
    if (!Expect(T->isOrdinal(), "an ordinal"))
      return errorType();
    return T->isIntegerLike() ? promoteInteger(T) : Ctx.getIntegerType();
  case BuiltinKind::Chr:
    return Expect(T->isIntegerLike(), "an integer") ? Ctx.getCharType()
                                                    : errorType();
  case BuiltinKind::Succ:
  case BuiltinKind::Pred:
    return Expect(T->isOrdinal(), "an ordinal") ? T->getBase() : errorType();
  case BuiltinKind::Sin: case BuiltinKind::Cos: case BuiltinKind::Exp:
  case BuiltinKind::Ln: case BuiltinKind::Sqrt: case BuiltinKind::Arctan:
    return Expect(T->isNumeric(), "a numeric") ? Ctx.getRealType()
                                               : errorType();
  case BuiltinKind::Trunc:
  case BuiltinKind::Round:
    if (!Expect(T->isNumeric(), "a real"))
      return errorType();
    // Turbo/FPC: the result is an int64.
    return isISO() ? Ctx.getIntegerType() : Ctx.getInt64Type();
  default:
    llvm_unreachable("not a builtin function");
  }
}


/// T(x) (Turbo/FPC): between ordinal types (integer types wrap, nothing
/// is range-checked) and between pointer types.
Type *Checker::checkTypecast(CallExpr *E, TypeDecl *TD){
  Type *T = TD->getType() ? TD->getType() : errorType();
  auto Args = E->getArgs();
  if (Args.size() != 1) {
    Diags.error(E->getCallee().Loc,
                "a typecast to " + quoted(T) + " takes exactly one value",
                {E->getRange()});
    for (Expr *A : Args)
      check(A);
    return errorType();
  }
  Type *AT = T == TMethodTy ? checkComparand(Args[0])
                            : checkAllowingUntyped(Args[0]);
  if (T->isError() || AT->isError())
    return errorType();
  if (T == TMethodTy && isMethodPointer(AT)) { // its code and object
    E->setMethodCast(isVariableReference(Args[0]));
    return T;
  }
  if (isMethodPointer(T) && AT == TMethodTy) // TEv(m)
    return T;
  if (AT->isUntyped()) { // Turbo/FPC: the untyped parameter seen as a T
    E->setVarCast();
    warnUnsafe(E->getBeginLoc(), "typecast of untyped parameter outside 'unsafe'",
               {E->getRange()});
    return T;
  }
  if (T->isAnsiString() && (AT->isStringish() || AT->isPointer()))
    return T; // an untyped pointer is taken to be an ansistring
  if (T->isShortString() && AT->isStringish())
    return T;
  // vector[N] of T: lane by lane, between vectors of one length, from
  // ordinal lanes to ordinal or real ones (a mask's booleans become 0 and
  // 1), or between real ones
  if (auto *TV = llvm::dyn_cast<VectorType>(T))
    if (auto *AV = llvm::dyn_cast<VectorType>(AT)) {
      Type *TE = TV->getElementType(), *AE = AV->getElementType();
      if (TV->getCount() == AV->getCount() &&
          ((AE->isOrdinal() && (TE->isOrdinal() || TE->isReal())) ||
           (AE->isReal() && TE->isReal())))
        return T;
    }
  if ((T->isOrdinal() && AT->isOrdinal()) ||
      (T->isPointer() &&
       (AT->isPointer() || AT->isNil() || AT->isClass() || AT->isAnsiString() ||
        AT->isArrayOfConst() || AT->isDynArray())) ||
      (T->isDynArray() && (AT->isDynArray() || AT->isPointer() || AT->isNil())) ||
      (T->isClass() && (AT->isClass() || AT->isPointer() || AT->isNil())) ||
      (T->isClassRef() && (AT->isClassRef() || AT->isNil())) ||
      (T->isInterface() &&
       (AT->isInterface() || AT->isNil() ||
        (AT->isClass() &&
         llvm::cast<ClassType>(AT)->findInterface(llvm::cast<InterfaceType>(T)))))) {
    if ((T->isPointer() &&
         (AT->isClass() || AT->isDynArray() || AT->isArrayOfConst())) ||
        ((T->isClass() || T->isDynArray()) && AT->isPointer()))
      warnUnsafe(E->getBeginLoc(),
                 "unsafe cast from " + quoted(AT) + " to " + quoted(T) +
                     " outside 'unsafe'",
                 {E->getRange()});
    else if (T->isPointer() && AT->isPointer() && !T->isUntyped() &&
             !areCompatible(T, AT))
      warnUnsafe(E->getBeginLoc(),
                 "unsafe pointer cast from " + quoted(AT) + " to " + quoted(T) +
                     " outside 'unsafe'",
                 {E->getRange()});
    return T;
  }
  if (RoutineDecl *Op = findConversion(AT, T, /*Explicit=*/true)) {
    Args[0]->setConversion(Op, T); // a record's Explicit or := operator
    return T;
  }
  Diags.error(E->getBeginLoc(),
              "invalid typecast from " + quoted(AT) + " to " + quoted(T) +
                  " (only between ordinal types and between pointer types)",
              {E->getRange()});
  return errorType();
}


/// A type name or an expression as the argument of sizeof/low/high:
/// returns its type (also stored on the argument).
Type *Checker::checkTypeOrValue(Expr *Arg){
  if (llvm::isa<FieldExpr>(Arg)) // TOuter.TInner
    if (TypeDecl *TD = typeNamedBy(Arg)) {
      Type *T = TD->getType() ? TD->getType() : errorType();
      Arg->setType(T);
      return T;
    }
  if (auto *N = llvm::dyn_cast<NameRefExpr>(Arg))
    if (auto *TD = llvm::dyn_cast_or_null<TypeDecl>(Cur->lookup(N->getName().Name))) {
      N->setDecl(TD);
      Type *T = TD->getType() ? TD->getType() : errorType();
      N->setType(T);
      return T;
    }
  return check(Arg);
}


/// Built-in functions whose arguments are not ordinary values (type
/// names, procedural variables) or that take no arguments. Returns
/// nullopt for the others.
std::optional<Type *> Checker::checkSpecialBuiltin(CallExpr *E, BuiltinDecl *BD){
  const Identifier &Callee = E->getCallee();
  auto Args = E->getArgs();
  auto Arity = [&](size_t N) {
    if (Args.size() == N)
      return true;
    Diags.error(Callee.Loc,
                "'" + Callee.Spelling + "' expects " + llvm::Twine(N) +
                    " argument" + (N == 1 ? "" : "s"),
                {E->getRange()});
    for (Expr *A : Args)
      check(A);
    return false;
  };
  switch (BD->getBuiltinKind()) {
  case BuiltinKind::Wait:
    return checkWait(Args, Callee, E->getRange(), /*NeedValue=*/true);
  case BuiltinKind::WhenAll:
  case BuiltinKind::WhenAny: {
    if (!Arity(1))
      return errorType();
    TaskType *TT = checkTaskArray(Args[0], Callee);
    if (!TT)
      return errorType();
    if (BD->getBuiltinKind() == BuiltinKind::WhenAny)
      return Ctx.getTaskType(Ctx.getIntegerType());
    return Ctx.getTaskType(TT->getResult() ? Ctx.getDynArrayType(TT->getResult())
                                           : nullptr);
  }
  case BuiltinKind::IteratorNext:
  case BuiltinKind::IteratorValue: { // (it: an iterator)
    if (!Arity(1))
      return errorType();
    Type *T = check(Args[0]);
    if (T->isError())
      return T;
    auto *IT = llvm::dyn_cast<IteratorType>(T);
    if (!IT) {
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling + "' needs an iterator, not a value "
                  "of type " + quoted(T),
                  {Args[0]->getRange()});
      return errorType();
    }
    markUsesManaged();
    return BD->getBuiltinKind() == BuiltinKind::IteratorNext
               ? Ctx.getBooleanType()
               : IT->getElementType();
  }
  case BuiltinKind::VectorMask: { // the lanes of a vector of booleans
    if (!Arity(1))
      return errorType();
    Type *T = check(Args[0]);
    if (T->isError())
      return T;
    auto *VT = llvm::dyn_cast<VectorType>(T);
    if (!VT || !VT->getElementType()->getBase()->isBoolean()) {
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling +
                      "' expects a vector of booleans (a comparison of "
                      "vectors), not " + quoted(T),
                  {Args[0]->getRange()});
      return errorType();
    }
    return Ctx.getQWordType();
  }
  case BuiltinKind::VectorLoad: { // VectorLoad(TVector, address)
    if (!Arity(2))
      return errorType();
    Type *T = checkTypeOrValue(Args[0]);
    Type *P = check(Args[1]);
    if (T->isError() || P->isError())
      return errorType();
    if (!T->isVector()) {
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects a vector type, not " +
                      quoted(T),
                  {Args[0]->getRange()});
      return errorType();
    }
    if (!P->isPointer()) {
      Diags.error(Args[1]->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects an address, not " +
                      quoted(P),
                  {Args[1]->getRange()});
      return errorType();
    }
    return T;
  }
  case BuiltinKind::PopCnt:
  case BuiltinKind::CountTrailingZeros:
  case BuiltinKind::CountLeadingZeros: { // the bits of an integer
    if (!Arity(1))
      return errorType();
    Type *T = check(Args[0]);
    if (T->isError())
      return T;
    if (!T->isIntegerLike()) {
      Diags.error(Args[0]->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects an integer, not " +
                      quoted(T),
                  {Args[0]->getRange()});
      return errorType();
    }
    return Ctx.getIntegerType();
  }
  case BuiltinKind::AddChecked:
  case BuiltinKind::SubChecked:
  case BuiltinKind::MulChecked: { // (a op b, ok) in the type of a op b
    if (!Arity(2))
      return errorType();
    Type *L = check(Args[0]), *R = check(Args[1]);
    if (L->isError() || R->isError())
      return errorType();
    if (!L->isIntegerLike() || !R->isIntegerLike()) {
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' expects integer arguments, not " +
                      quoted(L) + " and " + quoted(R),
                  {E->getRange()});
      return errorType();
    }
    Type *T = integerOperandType(Args[0], L, Args[1], R);
    return getTupleType({T, Ctx.getBooleanType()}, {});
  }
  case BuiltinKind::SizeOf: {
    if (!Arity(1))
      return errorType();
    Type *T = checkTypeOrValue(Args[0]);
    if (T->isError())
      return errorType();
    if (containsFile(T) || T->isStringLiteral() || T->isNil() ||
        (llvm::isa<SetType>(T) && llvm::cast<SetType>(T)->isEmptySet())) {
      Diags.error(Args[0]->getBeginLoc(),
                  "'sizeof' cannot be applied to " + quoted(T),
                  {Args[0]->getRange()});
      return errorType();
    }
    return Ctx.getInt64Type();
  }
  case BuiltinKind::Low:
  case BuiltinKind::High: {
    if (!Arity(1))
      return errorType();
    Type *T = checkTypeOrValue(Args[0]);
    if (T->isError())
      return errorType();
    if (T->isDynArray()) { // of a variable: its length is known at run time
      auto *N = llvm::dyn_cast<NameRefExpr>(Args[0]);
      if (N && llvm::isa_and_nonnull<TypeDecl>(Cur->lookup(N->getName().Name))) {
        Diags.error(Args[0]->getBeginLoc(),
                    "'" + Callee.Spelling + "' needs a dynamic array variable, not a type",
                    {Args[0]->getRange()});
        return errorType();
      }
      return Ctx.getIntegerType();
    }
    if (T->isShortString() || T->isArrayOfConst() || T->isOpenArray())
      return Ctx.getIntegerType();
    if (auto *A = llvm::dyn_cast<ArrayType>(T))
      return A->getIndexType();
    if (T->isOrdinal())
      return T;
    Diags.error(Args[0]->getBeginLoc(),
                "'" + Callee.Spelling +
                    "' expects an ordinal type, an array or a string, not " +
                    quoted(T),
                {Args[0]->getRange()});
    return errorType();
  }
  case BuiltinKind::Assigned: {
    if (!Arity(1))
      return errorType();
    Type *T;
    // A procedural variable is tested, not called.
    auto *N = llvm::dyn_cast<NameRefExpr>(Args[0]);
    auto *V = N ? llvm::dyn_cast_or_null<VarDecl>(Cur->lookup(N->getName().Name))
                : nullptr;
    if (V && llvm::isa_and_nonnull<RoutineType>(V->getType())) {
      N->setDecl(V);
      N->setType(V->getType());
      Used.insert(V);
      T = V->getType();
    } else {
      T = check(Args[0]);
    }
    if (T->isError())
      return errorType();
    if (!T->isPointer() && !T->isClass() && !T->isInterface() &&
        !T->isDynArray() && !llvm::isa<RoutineType>(T)) {
      Diags.error(Args[0]->getBeginLoc(),
                  "'assigned' expects a pointer or a procedural variable, not " +
                      quoted(T),
                  {Args[0]->getRange()});
      return errorType();
    }
    return Ctx.getBooleanType();
  }
  case BuiltinKind::ParamCount:
    if (!Arity(0))
      return errorType();
    return Ctx.getIntegerType();
  case BuiltinKind::ExceptAddr: // FPC's CodePointer
    if (!Arity(0))
      return errorType();
    return Ctx.getUntypedPointerType();
  case BuiltinKind::ParamStr: {
    if (!Arity(1))
      return errorType();
    Type *T = check(Args[0]);
    if (!T->isError() && !T->isIntegerLike()) {
      Diags.error(Args[0]->getBeginLoc(),
                  "'paramstr' expects an integer, not " + quoted(T),
                  {Args[0]->getRange()});
      return errorType();
    }
    return Ctx.getShortStringType();
  }
  default:
    return std::nullopt;
  }
}


/// getmem(p, n) freemem(p) reallocmem(p, n) fillchar(x, n, v)
/// move(src, dst, n) halt[(code)]  (Turbo/FPC)
void Checker::checkMemoryProc(ProcCallStmt *S, BuiltinKind K){
  const Identifier &Callee = S->getCallee();
  auto Args = S->getArgs();
  size_t Want = K == BuiltinKind::FreeMem ? 1
                : K == BuiltinKind::GetMem || K == BuiltinKind::ReallocMem ? 2
                                                                          : 3;
  if (K == BuiltinKind::Halt) {
    if (Args.size() > 1)
      Diags.error(Callee.Loc, "'halt' expects at most 1 argument",
                  {S->getRange()});
    else if (Args.size() == 1) {
      Type *T = check(Args[0]);
      if (!T->isError() && !T->isIntegerLike())
        Diags.error(Args[0]->getBeginLoc(),
                    "'halt' expects an integer exit code, not " + quoted(T),
                    {Args[0]->getRange()});
    }
    return;
  }
  if (Args.size() != Want) {
    Diags.error(Callee.Loc,
                "'" + Callee.Spelling + "' expects " + llvm::Twine(Want) +
                    " argument" + (Want == 1 ? "" : "s"),
                {S->getRange()});
    for (Expr *A : Args)
      check(A);
    return;
  }
  auto Expect = [&](Expr *A, bool OK, llvm::StringRef What) {
    if (!OK)
      Diags.error(A->getBeginLoc(),
                  "'" + Callee.Spelling + "' expects " + What,
                  {A->getRange()});
  };
  llvm::SmallVector<Type *, 3> T;
  for (size_t I = 0; I < Args.size(); ++I) // (variables: untyped ones too)
    T.push_back((K == BuiltinKind::FillChar && I == 0) ||
                        (K == BuiltinKind::Move && I < 2)
                    ? checkAllowingUntyped(Args[I])
                    : check(Args[I]));
  for (Type *X : T)
    if (X->isError())
      return;
  warnUnsafe(Callee.Loc, "'" + Callee.Spelling + "' used outside 'unsafe'",
             {S->getRange()});
  switch (K) {
  case BuiltinKind::GetMem:
  case BuiltinKind::ReallocMem:
    Expect(Args[0], T[0]->isPointer() && isVariableReference(Args[0]),
           "a pointer variable");
    Expect(Args[1], T[1]->isIntegerLike(), "an integer size");
    if (T[0]->isPointer() && isVariableReference(Args[0]) &&
        checkModifiable(Args[0]) && NilPointersHere)
      if (int I = nullIndexOf(Args[0]); I >= 0) {
        if (K == BuiltinKind::GetMem) {
          nullSet(static_cast<unsigned>(I), false, true);
        } else if (auto C = fold(Args[1]);
                   C && C->Value.K == ConstValue::Ordinal) {
          if (C->Value.Int > 0)
            nullSet(static_cast<unsigned>(I), false, true);
          else {
            nullSet(static_cast<unsigned>(I), true, false);
            Flow->MoveLoc[I] = Args[0]->getBeginLoc();
          }
        } else {
          nullSet(static_cast<unsigned>(I), true, true);
        }
      }
    return;
  case BuiltinKind::FreeMem:
    Expect(Args[0], T[0]->isPointer(), "a pointer");
    if (T[0]->isPointer() && NilPointersHere) {
      requireNotNil(Args[0],
                    "but it is passed to '" + Callee.Spelling.str() + "'");
      if (int I = nullIndexOf(Args[0]); I >= 0) {
        nullSet(static_cast<unsigned>(I), true, false);
        Flow->MoveLoc[I] = Args[0]->getBeginLoc();
      }
    }
    return;
  case BuiltinKind::FillChar:
    Expect(Args[0], isVariableReference(Args[0]) && !containsFile(T[0]),
           "a variable");
    Expect(Args[1], T[1]->isIntegerLike(), "an integer count");
    Expect(Args[2], T[2]->isOrdinal(), "an ordinal fill value");
    if (isVariableReference(Args[0]))
      checkModifiable(Args[0]);
    return;
  default: // Move
    Expect(Args[0], isVariableReference(Args[0]) && !containsFile(T[0]),
           "a source variable");
    Expect(Args[1], isVariableReference(Args[1]) && !containsFile(T[1]),
           "a destination variable");
    Expect(Args[2], T[2]->isIntegerLike(), "an integer count");
    if (isVariableReference(Args[1]))
      checkModifiable(Args[1]);
    return;
  }
}


/// VectorStore(address, vector)
void Checker::checkVectorStore(ProcCallStmt *S){
  const Identifier &Callee = S->getCallee();
  auto Args = S->getArgs();
  if (Args.size() != 2) {
    Diags.error(Callee.Loc, "'" + Callee.Spelling + "' expects 2 arguments",
                {S->getRange()});
    for (Expr *A : Args)
      check(A);
    return;
  }
  Type *P = check(Args[0]);
  Type *V = check(Args[1]);
  if (P->isError() || V->isError())
    return;
  if (!P->isPointer())
    Diags.error(Args[0]->getBeginLoc(),
                "'" + Callee.Spelling + "' expects an address, not " +
                    quoted(P),
                {Args[0]->getRange()});
  if (!V->isVector())
    Diags.error(Args[1]->getBeginLoc(),
                "'" + Callee.Spelling + "' expects a vector, not " + quoted(V),
                {Args[1]->getRange()});
}

