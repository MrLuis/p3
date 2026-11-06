#include "pcc/Sema/Sema.h"
#include "pcc/AST/ASTContext.h"
#include "pcc/AST/ASTVisitor.h"
#include "pcc/Basic/Diagnostics.h"
#include "pcc/Basic/LangOptions.h"
#include "pcc/Sema/Scope.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

using namespace pcc;

namespace {

/// Predeclared names from PLAN.md 4.5 that later milestones implement. Using
/// one gives "not supported yet" instead of "undeclared identifier".
bool isFutureBuiltin(llvm::StringRef Name) {
  static const llvm::StringSet<> Names = {
      "real", "text", "input", "output",                       // types/files
      "reset", "rewrite", "get", "put", "page", "eof", "eoln", // files (M10)
      "sin", "cos", "exp", "ln", "sqrt", "arctan", "trunc", "round", // M7
      "new", "dispose",                                        // M9
      "pack", "unpack"};                                       // M10
  return Names.contains(Name);
}

class Checker : public StmtVisitor<Checker>,
                public ExprVisitor<Checker, Type *> {
  ASTContext &Ctx;
  DiagnosticsEngine &Diags;
  [[maybe_unused]] const LangOptions &LangOpts; // dialect rules from M7 on

  Scope Standard;              ///< predeclared entities
  Scope ProgramScope{&Standard};
  Scope *Cur = &ProgramScope;

  /// Variables declared together (`a, b: T`) share one TypeSpec; resolve it
  /// once so a bad type is reported once.
  llvm::DenseMap<TypeSpec *, Type *> ResolvedTypeSpecs;

  Type *errorType() { return Ctx.getErrorType(); }

  //===--------------------------------------------------------------------===//
  // Standard scope
  //===--------------------------------------------------------------------===//

  Identifier predeclaredName(llvm::StringRef N) {
    llvm::StringRef Interned = Ctx.getStrings().intern(N);
    return {Interned, Interned, SourceLocation()};
  }

  void declareStandardScope() {
    auto Ty = [&](llvm::StringRef N, Type *T) {
      Standard.insert(N, Ctx.create<TypeDecl>(SourceLocation(), predeclaredName(N), T));
    };
    Ty("integer", Ctx.getIntegerType());
    Ty("boolean", Ctx.getBooleanType());
    Ty("char", Ctx.getCharType());

    auto Const = [&](llvm::StringRef N, Type *T, int64_t V) {
      Standard.insert(N, Ctx.create<ConstDecl>(SourceLocation(), predeclaredName(N), T, V));
    };
    Const("false", Ctx.getBooleanType(), 0);
    Const("true", Ctx.getBooleanType(), 1);
    Const("maxint", Ctx.getIntegerType(), PascalMaxInt);

#define BUILTIN_PROC(ID, NAME)                                                 \
  Standard.insert(NAME, Ctx.create<BuiltinDecl>(predeclaredName(NAME),         \
                                                BuiltinKind::ID, true));
#define BUILTIN_FUNC(ID, NAME)                                                 \
  Standard.insert(NAME, Ctx.create<BuiltinDecl>(predeclaredName(NAME),         \
                                                BuiltinKind::ID, false));
#include "pcc/AST/Builtins.def"
  }

  //===--------------------------------------------------------------------===//
  // Helpers
  //===--------------------------------------------------------------------===//

  /// Diagnoses a name that resolves to nothing.
  void diagUndeclared(const Identifier &Id) {
    if (isFutureBuiltin(Id.Name))
      Diags.error(Id.Loc, "'" + Id.Spelling + "' is not supported yet",
                  {Id.getRange()});
    else
      Diags.error(Id.Loc, "use of undeclared identifier '" + Id.Spelling + "'",
                  {Id.getRange()});
  }

  void noteDeclaredHere(Decl *D) {
    if (!D->isPredeclared())
      Diags.note(D->getLocation(),
                 "'" + D->getName().Spelling + "' declared here");
  }

  Type *check(Expr *E) {
    Type *T = visitExpr(E);
    E->setType(T);
    return T;
  }

  static std::string quoted(Type *T) { return ("'" + T->getName() + "'").str(); }

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  Type *resolveTypeSpec(TypeSpec *TS) {
    auto [It, Inserted] = ResolvedTypeSpecs.try_emplace(TS, nullptr);
    if (!Inserted)
      return It->second;

    Type *Result = errorType();
    auto *Named = llvm::cast<NamedTypeSpec>(TS);
    const Identifier &Id = Named->getName();
    Decl *D = Cur->lookup(Id.Name);
    if (!D) {
      if (isFutureBuiltin(Id.Name))
        Diags.error(Id.Loc, "type '" + Id.Spelling + "' is not supported yet",
                    {Id.getRange()});
      else
        Diags.error(Id.Loc, "unknown type name '" + Id.Spelling + "'",
                    {Id.getRange()});
    } else if (auto *TD = llvm::dyn_cast<TypeDecl>(D)) {
      Result = TD->getType();
    } else {
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' is a " + D->getKindName() +
                      ", not a type",
                  {Id.getRange()});
      noteDeclaredHere(D);
    }
    ResolvedTypeSpecs[TS] = Result;
    return Result;
  }

  void declareVar(VarDecl *V) {
    V->setType(resolveTypeSpec(V->getTypeSpec()));
    const Identifier &Id = V->getName();
    if (Decl *Prev = Cur->insert(Id.Name, V)) {
      Diags.error(Id.Loc, "redefinition of '" + Id.Spelling + "'",
                  {Id.getRange()});
      Diags.note(Prev->getLocation(), "previous definition is here");
    }
  }

  void checkProgramParams(ProgramDecl &P) {
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
      // ISO 7185 6.10: other parameters must be declared as file variables
      // in the program block (supported from M10).
      if (Param.Name != "input" && Param.Name != "output")
        Diags.error(Param.Loc,
                    "program parameter '" + Param.Spelling +
                        "' is not declared (only 'input' and 'output' are "
                        "supported yet)",
                    {Param.getRange()});
    }
  }

  //===--------------------------------------------------------------------===//
  // Built-in procedure calls
  //===--------------------------------------------------------------------===//

  void checkWrite(ProcCallStmt *S, bool IsWriteln) {
    const Identifier &Callee = S->getCallee();
    if (!IsWriteln && S->getArgs().empty())
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' requires at least one argument",
                  {Callee.getRange()});

    for (Expr *Arg : S->getArgs()) {
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

      if (Width) {
        Type *WT = check(Width);
        if (!WT->isError() && !WT->isInteger())
          Diags.error(Width->getBeginLoc(),
                      "field width must be an integer, not " + quoted(WT),
                      {Width->getRange()});
      }
      if (Precision) {
        check(Precision);
        // ISO 6.9.3.4: only real values take a fraction-digits argument.
        Diags.error(Precision->getBeginLoc(),
                    "a precision (':n') is only allowed for real values",
                    {Precision->getRange()});
      }
    }
  }

  void checkRead(ProcCallStmt *S, bool IsReadln) {
    const Identifier &Callee = S->getCallee();
    if (!IsReadln && S->getArgs().empty())
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' requires at least one argument",
                  {Callee.getRange()});

    for (Expr *Arg : S->getArgs()) {
      if (auto *F = llvm::dyn_cast<FormattedExpr>(Arg)) {
        Diags.error(F->getWidth()->getBeginLoc(),
                    "field widths are only allowed in 'write' and 'writeln'",
                    {F->getWidth()->getRange()});
        continue;
      }
      Type *T = check(Arg);
      auto *Ref = llvm::dyn_cast<NameRefExpr>(Arg);
      if (!Ref || (Ref->getDecl() && !llvm::isa<VarDecl>(Ref->getDecl()))) {
        if (!T->isError())
          Diags.error(Arg->getBeginLoc(),
                      "argument to '" + Callee.Spelling +
                          "' must be a variable",
                      {Arg->getRange()});
        continue;
      }
      if (!T->isError() && !T->isInteger() && !T->isChar())
        Diags.error(Arg->getBeginLoc(),
                    "cannot read a value of type " + quoted(T) +
                        " (only integer and char are supported yet)",
                    {Arg->getRange()});
    }
  }

  //===--------------------------------------------------------------------===//
  // Built-in function calls
  //===--------------------------------------------------------------------===//

  Type *checkBuiltinFunction(CallExpr *E, BuiltinDecl *BD,
                             llvm::ArrayRef<Type *> ArgTypes) {
    const Identifier &Callee = E->getCallee();
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
    case BuiltinKind::Sqr: // real versions: M7
      return Expect(T->isInteger(), "an integer") ? Ctx.getIntegerType()
                                                  : errorType();
    case BuiltinKind::Odd:
      return Expect(T->isInteger(), "an integer") ? Ctx.getBooleanType()
                                                  : errorType();
    case BuiltinKind::Ord:
      return Expect(T->isOrdinal(), "an ordinal") ? Ctx.getIntegerType()
                                                  : errorType();
    case BuiltinKind::Chr:
      return Expect(T->isInteger(), "an integer") ? Ctx.getCharType()
                                                  : errorType();
    case BuiltinKind::Succ:
    case BuiltinKind::Pred:
      return Expect(T->isOrdinal(), "an ordinal") ? T : errorType();
    default:
      llvm_unreachable("not a builtin function");
    }
  }

public:
  Checker(ASTContext &Ctx, DiagnosticsEngine &Diags, const LangOptions &LO)
      : Ctx(Ctx), Diags(Diags), LangOpts(LO) {
    declareStandardScope();
  }

  void checkProgram(ProgramDecl &P) {
    checkProgramParams(P);
    Block *B = P.getBlock();
    for (VarDecl *V : B->getVars())
      declareVar(V);
    visitStmt(B->getBody());
  }

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  void visitEmptyStmt(EmptyStmt *) {}

  void visitCompoundStmt(CompoundStmt *S) {
    for (Stmt *Child : S->getBody()) {
      if (Diags.shouldStop())
        return;
      visitStmt(Child);
    }
  }

  void visitProcCallStmt(ProcCallStmt *S) {
    const Identifier &Callee = S->getCallee();
    Decl *D = Cur->lookup(Callee.Name);
    if (!D) {
      diagUndeclared(Callee);
      return;
    }
    S->setDecl(D);

    auto *BD = llvm::dyn_cast<BuiltinDecl>(D);
    if (BD && BD->isProcedure()) {
      switch (BD->getBuiltinKind()) {
      case BuiltinKind::Write:   return checkWrite(S, /*IsWriteln=*/false);
      case BuiltinKind::Writeln: return checkWrite(S, /*IsWriteln=*/true);
      case BuiltinKind::Read:    return checkRead(S, /*IsReadln=*/false);
      case BuiltinKind::Readln:  return checkRead(S, /*IsReadln=*/true);
      default: llvm_unreachable("not a builtin procedure");
      }
    }

    if (BD)
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

  void visitAssignStmt(AssignStmt *S) {
    auto *Target = llvm::cast<NameRefExpr>(S->getTarget());
    const Identifier &Id = Target->getName();
    Type *TargetTy = errorType();

    if (Decl *D = Cur->lookup(Id.Name)) {
      Target->setDecl(D);
      if (auto *V = llvm::dyn_cast<VarDecl>(D)) {
        TargetTy = V->getType() ? V->getType() : errorType();
      } else {
        // Function-result assignment (`f := ...` inside f) arrives in M6.
        Diags.error(Id.Loc,
                    "cannot assign to " + llvm::Twine(D->getKindName()) +
                        " '" + Id.Spelling + "'",
                    {Id.getRange()});
        noteDeclaredHere(D);
      }
    } else {
      diagUndeclared(Id);
    }
    Target->setType(TargetTy);

    Type *ValueTy = check(S->getValue());
    if (TargetTy->isError() || ValueTy->isError())
      return;
    // ISO 7185 6.4.6 assignment compatibility. Until reals/subranges (M7)
    // this means "the same type".
    if (TargetTy != ValueTy)
      Diags.error(S->getAssignLoc(),
                  "assigning to " + quoted(TargetTy) +
                      " from incompatible type " + quoted(ValueTy),
                  {Target->getRange(), S->getValue()->getRange()});
  }

  //===--------------------------------------------------------------------===//
  // Expressions (each returns the expression's type)
  //===--------------------------------------------------------------------===//

  Type *visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    if (E->getValue() > static_cast<uint64_t>(PascalMaxInt))
      Diags.error(E->getBeginLoc(),
                  "integer literal exceeds maxint (2147483647)",
                  {E->getRange()});
    return Ctx.getIntegerType();
  }

  Type *visitStringLiteralExpr(StringLiteralExpr *E) {
    return E->isCharLiteral() ? Ctx.getCharType() : Ctx.getStringType();
  }

  Type *visitNameRefExpr(NameRefExpr *E) {
    const Identifier &Id = E->getName();
    Decl *D = Cur->lookup(Id.Name);
    if (!D) {
      diagUndeclared(Id);
      return errorType();
    }
    E->setDecl(D);

    switch (D->getKind()) {
    case Decl::DK_Var: {
      Type *T = llvm::cast<VarDecl>(D)->getType();
      return T ? T : errorType();
    }
    case Decl::DK_Const:
      return llvm::cast<ConstDecl>(D)->getType();
    case Decl::DK_Builtin:
      if (llvm::cast<BuiltinDecl>(D)->isFunction()) {
        Diags.error(Id.Loc, "missing argument in call to '" + Id.Spelling + "'",
                    {Id.getRange()});
        return errorType();
      }
      Diags.error(Id.Loc,
                  "procedure '" + Id.Spelling + "' does not return a value",
                  {Id.getRange()});
      return errorType();
    default:
      Diags.error(Id.Loc,
                  "'" + Id.Spelling + "' is a " + D->getKindName() +
                      ", not a value",
                  {Id.getRange()});
      noteDeclaredHere(D);
      return errorType();
    }
  }

  Type *visitParenExpr(ParenExpr *E) { return check(E->getInner()); }

  Type *visitUnaryExpr(UnaryExpr *E) {
    Type *T = check(E->getOperand());
    if (T->isError())
      return errorType();
    bool OK = E->getOp() == UnaryOp::Not ? T->isBoolean() : T->isInteger();
    if (!OK) {
      Diags.error(E->getOpLoc(),
                  llvm::Twine("invalid operand to '") +
                      getOpSpelling(E->getOp()) + "' (" + quoted(T) + ")",
                  {E->getOperand()->getRange()});
      return errorType();
    }
    return T;
  }

  Type *visitBinaryExpr(BinaryExpr *E) {
    Type *L = check(E->getLHS());
    Type *R = check(E->getRHS());
    if (L->isError() || R->isError())
      return errorType();

    BinaryOp Op = E->getOp();
    switch (Op) {
    case BinaryOp::Add: case BinaryOp::Sub: case BinaryOp::Mul:
    case BinaryOp::IntDiv: case BinaryOp::Mod:
      if (L->isInteger() && R->isInteger())
        return Ctx.getIntegerType();
      break;
    case BinaryOp::RealDiv:
      if (L->isInteger() && R->isInteger()) {
        Diags.error(E->getOpLoc(),
                    "real division '/' is not supported yet; use 'div' for "
                    "integer division",
                    {E->getLHS()->getRange(), E->getRHS()->getRange()});
        return errorType();
      }
      break;
    case BinaryOp::And: case BinaryOp::Or:
      if (L->isBoolean() && R->isBoolean())
        return Ctx.getBooleanType();
      break;
    case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt:
    case BinaryOp::Le: case BinaryOp::Gt: case BinaryOp::Ge:
      if (L == R && L->isOrdinal())
        return Ctx.getBooleanType();
      if (L->isString() && R->isString()) {
        Diags.error(E->getOpLoc(), "comparing strings is not supported yet",
                    {E->getLHS()->getRange(), E->getRHS()->getRange()});
        return errorType();
      }
      break;
    case BinaryOp::In:
      Diags.error(E->getOpLoc(), "'in' (sets) is not supported yet");
      return errorType();
    }

    Diags.error(E->getOpLoc(),
                llvm::Twine("invalid operands to '") + getOpSpelling(Op) +
                    "' (" + quoted(L) + " and " + quoted(R) + ")",
                {E->getLHS()->getRange(), E->getRHS()->getRange()});
    return errorType();
  }

  Type *visitCallExpr(CallExpr *E) {
    llvm::SmallVector<Type *, 4> ArgTypes;
    for (Expr *Arg : E->getArgs())
      ArgTypes.push_back(check(Arg));

    const Identifier &Callee = E->getCallee();
    Decl *D = Cur->lookup(Callee.Name);
    if (!D) {
      diagUndeclared(Callee);
      return errorType();
    }
    E->setDecl(D);

    if (auto *BD = llvm::dyn_cast<BuiltinDecl>(D)) {
      if (BD->isFunction())
        return checkBuiltinFunction(E, BD, ArgTypes);
      Diags.error(Callee.Loc,
                  "procedure '" + Callee.Spelling +
                      "' does not return a value",
                  {Callee.getRange()});
      return errorType();
    }
    Diags.error(Callee.Loc,
                "'" + Callee.Spelling + "' is a " + D->getKindName() +
                    ", not a function",
                {Callee.getRange()});
    noteDeclaredHere(D);
    return errorType();
  }

  Type *visitFormattedExpr(FormattedExpr *E) {
    check(E->getValue());
    Diags.error(E->getWidth()->getBeginLoc(),
                "field widths are only allowed in 'write' and 'writeln'",
                {E->getWidth()->getRange()});
    return errorType();
  }
};

} // namespace

bool Sema::check(ProgramDecl &P) {
  unsigned ErrorsBefore = Diags.getNumErrors();
  Checker(Ctx, Diags, LangOpts).checkProgram(P);
  return Diags.getNumErrors() == ErrorsBefore && !Diags.shouldStop();
}
