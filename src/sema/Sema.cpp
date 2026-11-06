#include "pcc/Sema/Sema.h"
#include "pcc/AST/ASTVisitor.h"
#include "pcc/Basic/Diagnostics.h"

#include "llvm/ADT/StringMap.h"

using namespace pcc;

namespace {

class Checker : public StmtVisitor<Checker>, public ExprVisitor<Checker> {
  DiagnosticsEngine &Diags;

public:
  explicit Checker(DiagnosticsEngine &Diags) : Diags(Diags) {}

  void checkProgram(ProgramDecl &P) {
    llvm::StringMap<SourceLocation> Seen;
    for (const Identifier &Param : P.getParams()) {
      auto [It, Inserted] = Seen.try_emplace(Param.Name, Param.Loc);
      if (!Inserted) {
        Diags.error(Param.Loc, "duplicate program parameter '" +
                                   Param.Spelling + "'",
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
    if (Block *B = P.getBlock())
      visitStmt(B->getBody());
  }

  void visitEmptyStmt(EmptyStmt *) {}

  void visitCompoundStmt(CompoundStmt *S) {
    for (Stmt *Child : S->getBody())
      visitStmt(Child);
  }

  void visitProcCallStmt(ProcCallStmt *S) {
    const Identifier &Callee = S->getCallee();
    bool IsWrite = Callee.Name == "write";
    bool IsWriteln = Callee.Name == "writeln";
    if (!IsWrite && !IsWriteln) {
      Diags.error(Callee.Loc,
                  "use of undeclared identifier '" + Callee.Spelling + "'",
                  {Callee.getRange()});
      return;
    }
    if (IsWrite && S->getArgs().empty())
      Diags.error(Callee.Loc,
                  "'" + Callee.Spelling + "' requires at least one argument",
                  {Callee.getRange()});
    for (Expr *Arg : S->getArgs())
      visitExpr(Arg);
  }

  void visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    if (E->getValue() > static_cast<uint64_t>(PascalMaxInt))
      Diags.error(E->getBeginLoc(),
                  "integer literal exceeds maxint (2147483647)",
                  {E->getRange()});
  }

  void visitStringLiteralExpr(StringLiteralExpr *) {}
};

} // namespace

bool Sema::check(ProgramDecl &P) {
  unsigned ErrorsBefore = Diags.getNumErrors();
  Checker(Diags).checkProgram(P);
  return Diags.getNumErrors() == ErrorsBefore;
}
