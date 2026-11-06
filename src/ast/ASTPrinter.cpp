#include "pcc/AST/ASTPrinter.h"
#include "pcc/AST/ASTVisitor.h"
#include "pcc/Basic/SourceManager.h"

#include "llvm/Support/raw_ostream.h"

using namespace pcc;

namespace {

class ASTPrinter : public StmtVisitor<ASTPrinter>, public ExprVisitor<ASTPrinter> {
  const SourceManager &SM;
  llvm::raw_ostream &OS;
  unsigned Indent = 0;

  struct IndentScope {
    unsigned &I;
    explicit IndentScope(unsigned &I) : I(I) { I += 2; }
    ~IndentScope() { I -= 2; }
  };

  llvm::raw_ostream &line(llvm::StringRef NodeName) {
    return OS.indent(Indent) << NodeName;
  }
  void loc(SourceLocation L) {
    PresumedLoc P = SM.getPresumedLoc(L);
    if (P.isValid())
      OS << " <" << P.Line << ':' << P.Column << '>';
    else
      OS << " <invalid>";
    OS << '\n';
  }
  void quoted(llvm::StringRef S) {
    OS << '"';
    OS.write_escaped(S);
    OS << '"';
  }

public:
  ASTPrinter(const SourceManager &SM, llvm::raw_ostream &OS) : SM(SM), OS(OS) {}

  void printProgram(ProgramDecl &P) {
    line("ProgramDecl '") << P.getName().Name << '\'';
    loc(P.getLocation());
    IndentScope S(Indent);
    for (const Identifier &Param : P.getParams()) {
      line("ProgramParam '") << Param.Name << '\'';
      loc(Param.Loc);
    }
    if (Block *B = P.getBlock())
      if (B->getBody())
        visitStmt(B->getBody());
  }

  // Statements
  void visitEmptyStmt(EmptyStmt *S) {
    line("EmptyStmt");
    loc(S->getBeginLoc());
  }
  void visitCompoundStmt(CompoundStmt *S) {
    line("CompoundStmt");
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    for (Stmt *Child : S->getBody())
      visitStmt(Child);
  }
  void visitProcCallStmt(ProcCallStmt *S) {
    line("ProcCallStmt '") << S->getCallee().Name << '\'';
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    for (Expr *Arg : S->getArgs())
      visitExpr(Arg);
  }

  // Expressions
  void visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    line("IntegerLiteralExpr ") << E->getValue();
    loc(E->getBeginLoc());
  }
  void visitStringLiteralExpr(StringLiteralExpr *E) {
    line(E->isCharLiteral() ? "CharLiteralExpr " : "StringLiteralExpr ");
    quoted(E->getValue());
    loc(E->getBeginLoc());
  }
};

} // namespace

void pcc::printAST(ProgramDecl &P, const SourceManager &SM,
                   llvm::raw_ostream &OS) {
  ASTPrinter(SM, OS).printProgram(P);
}
