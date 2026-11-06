//===- ASTPrinter.cpp - --emit-ast ----------------------------------------===//
//
// One node per line, indented by depth:
//
//   BinaryExpr '+' : integer <3:10>
//   ^node       ^detail ^type  ^location
//
// Types appear once Sema has run (i.e. when the program has no syntax
// errors).
//
//===----------------------------------------------------------------------===//

#include "pcc/AST/ASTPrinter.h"
#include "pcc/AST/ASTVisitor.h"
#include "pcc/AST/Type.h"
#include "pcc/Basic/SourceManager.h"

#include "llvm/Support/raw_ostream.h"

using namespace pcc;

namespace {

class ASTPrinter : public StmtVisitor<ASTPrinter>,
                   public ExprVisitor<ASTPrinter> {
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
  void type(Type *T) {
    if (T)
      OS << " : " << T->getName();
  }
  void loc(SourceLocation L) {
    PresumedLoc P = SM.getPresumedLoc(L);
    if (P.isValid())
      OS << " <" << P.Line << ':' << P.Column << '>';
    else
      OS << " <invalid>";
    OS << '\n';
  }
  void end(Expr *E) {
    type(E->getType());
    loc(E->getBeginLoc());
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
    Block *B = P.getBlock();
    if (!B)
      return;
    for (VarDecl *V : B->getVars()) {
      line("VarDecl '") << V->getName().Name << '\'';
      if (V->getType())
        type(V->getType());
      else if (auto *N = llvm::dyn_cast<NamedTypeSpec>(V->getTypeSpec()))
        OS << " : '" << N->getName().Spelling << '\'';
      loc(V->getLocation());
    }
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
  void visitAssignStmt(AssignStmt *S) {
    line("AssignStmt");
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    visitExpr(S->getTarget());
    visitExpr(S->getValue());
  }

  void visitIfStmt(IfStmt *S) {
    line(S->getElse() ? "IfStmt has_else" : "IfStmt");
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    visitExpr(S->getCond());
    visitStmt(S->getThen());
    if (S->getElse())
      visitStmt(S->getElse());
  }
  void visitWhileStmt(WhileStmt *S) {
    line("WhileStmt");
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    visitExpr(S->getCond());
    visitStmt(S->getBody());
  }
  void visitRepeatStmt(RepeatStmt *S) {
    line("RepeatStmt");
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    for (Stmt *Child : S->getBody())
      visitStmt(Child);
    visitExpr(S->getCond());
  }
  void visitForStmt(ForStmt *S) {
    line(S->isDownto() ? "ForStmt downto" : "ForStmt to");
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    visitExpr(S->getVar());
    visitExpr(S->getInit());
    visitExpr(S->getFinal());
    visitStmt(S->getBody());
  }
  void visitCaseStmt(CaseStmt *S) {
    line("CaseStmt");
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    visitExpr(S->getSelector());
    for (CaseArm *Arm : S->getArms()) {
      line("CaseArm");
      loc(Arm->getLabels().empty() ? Arm->getBody()->getBeginLoc()
                                   : Arm->getLabels()[0]->Lo->getBeginLoc());
      IndentScope J(Indent);
      for (CaseLabel *L : Arm->getLabels()) {
        line(L->isRange() ? "CaseLabelRange" : "CaseLabel");
        loc(L->Lo->getBeginLoc());
        IndentScope K(Indent);
        visitExpr(L->Lo);
        if (L->Hi)
          visitExpr(L->Hi);
      }
      visitStmt(Arm->getBody());
    }
    if (S->hasElse()) {
      line("CaseElse");
      loc(S->getElseLoc());
      IndentScope J(Indent);
      for (Stmt *Child : S->getElseBody())
        visitStmt(Child);
    }
  }

  // Expressions
  void visitIntegerLiteralExpr(IntegerLiteralExpr *E) {
    line("IntegerLiteralExpr ") << E->getValue();
    end(E);
  }
  void visitStringLiteralExpr(StringLiteralExpr *E) {
    line(E->isCharLiteral() ? "CharLiteralExpr " : "StringLiteralExpr ");
    quoted(E->getValue());
    end(E);
  }
  void visitNameRefExpr(NameRefExpr *E) {
    line("NameRefExpr '") << E->getName().Name << '\'';
    if (Decl *D = E->getDecl())
      OS << ' ' << D->getKindName();
    end(E);
  }
  void visitParenExpr(ParenExpr *E) {
    line("ParenExpr");
    end(E);
    IndentScope I(Indent);
    visitExpr(E->getInner());
  }
  void visitUnaryExpr(UnaryExpr *E) {
    line("UnaryExpr '") << getOpSpelling(E->getOp()) << '\'';
    end(E);
    IndentScope I(Indent);
    visitExpr(E->getOperand());
  }
  void visitBinaryExpr(BinaryExpr *E) {
    line("BinaryExpr '") << getOpSpelling(E->getOp()) << '\'';
    end(E);
    IndentScope I(Indent);
    visitExpr(E->getLHS());
    visitExpr(E->getRHS());
  }
  void visitCallExpr(CallExpr *E) {
    line("CallExpr '") << E->getCallee().Name << '\'';
    end(E);
    IndentScope I(Indent);
    for (Expr *Arg : E->getArgs())
      visitExpr(Arg);
  }
  void visitFormattedExpr(FormattedExpr *E) {
    line("FormattedExpr");
    end(E);
    IndentScope I(Indent);
    visitExpr(E->getValue());
    visitExpr(E->getWidth());
    if (E->getPrecision())
      visitExpr(E->getPrecision());
  }
};

} // namespace

void pcc::printAST(ProgramDecl &P, const SourceManager &SM,
                   llvm::raw_ostream &OS) {
  ASTPrinter(SM, OS).printProgram(P);
}
