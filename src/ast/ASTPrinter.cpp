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

#include "llvm/Support/Format.h"
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

  void typeSpec(TypeSpec *TS, Type *Resolved) {
    if (Resolved)
      type(Resolved);
    else if (auto *N = llvm::dyn_cast_or_null<NamedTypeSpec>(TS))
      OS << " : '" << N->getName().Spelling << '\'';
  }

  void printConstValue(const ConstValue &V) {
    switch (V.K) {
    case ConstValue::Ordinal: OS << " = " << V.Int; break;
    case ConstValue::Real:    OS << " = " << llvm::format("%g", V.RealVal); break;
    case ConstValue::String:  OS << " = "; quoted(V.Str); break;
    case ConstValue::None:    break;
    }
  }

  void printDecl(Decl *D) {
    switch (D->getKind()) {
    case Decl::DK_Const: {
      auto *C = llvm::cast<ConstDecl>(D);
      line("ConstDecl '") << C->getName().Name << '\'';
      type(C->getType());
      printConstValue(C->getValue());
      loc(C->getLocation());
      if (!C->getType() && C->getInit()) {
        IndentScope I(Indent);
        visitExpr(C->getInit());
      }
      return;
    }
    case Decl::DK_Type: {
      auto *T = llvm::cast<TypeDecl>(D);
      line("TypeDecl '") << T->getName().Name << '\'';
      if (T->getType())
        OS << " = " << describeType(T->getType());
      loc(T->getLocation());
      return;
    }
    case Decl::DK_Var: {
      auto *V = llvm::cast<VarDecl>(D);
      const char *Kind = V->getVarKind() == VarDecl::VarParam ? "ParamDecl var '"
                         : V->getVarKind() == VarDecl::ValueParam ? "ParamDecl '"
                                                                  : "VarDecl '";
      line(Kind) << V->getName().Name << '\'';
      typeSpec(V->getTypeSpec(), V->getType());
      loc(V->getLocation());
      return;
    }
    case Decl::DK_Routine: {
      auto *R = llvm::cast<RoutineDecl>(D);
      line(R->isFunction() ? "FunctionDecl '" : "ProcedureDecl '")
          << R->getName().Name << '\'';
      if (R->getResultType())
        type(R->getResultType());
      if (R->isForward())
        OS << " forward";
      if (R->isRedeclaration())
        OS << " definition";
      loc(R->getLocation());
      IndentScope I(Indent);
      if (!R->isRedeclaration())
        for (VarDecl *P : R->getParams())
          printDecl(P);
      if (Block *B = R->getBody(); B && !R->isForward())
        printBlock(*B);
      return;
    }
    default:
      line("Decl '") << D->getName().Name << '\'';
      loc(D->getLocation());
      return;
    }
  }

  /// Like Type::getName, but spells out a named type's structure.
  static std::string describeType(Type *T) {
    std::string Name = T->getName();
    if (T->getDeclName().empty())
      return Name;
    // Temporarily ignore the declared name to show the structure.
    switch (T->getKind()) {
    case Type::TK_Array: {
      auto *A = llvm::cast<ArrayType>(T);
      return std::string(A->isPacked() ? "packed " : "") + "array [" +
             A->getIndexType()->getName() + "] of " +
             A->getElementType()->getName();
    }
    case Type::TK_Record: {
      std::string S = "record";
      for (FieldDecl *F : llvm::cast<RecordType>(T)->getAllFields())
        S += " " + F->getName().Name.str() + ": " +
             (F->getType() ? F->getType()->getName() : "?") + ";";
      return S + " end";
    }
    case Type::TK_Subrange: {
      auto *S = llvm::cast<SubrangeType>(T);
      return S->getHost()->getName() + " " + std::to_string(S->getLo()) +
             ".." + std::to_string(S->getHi());
    }
    case Type::TK_Enum: {
      std::string S = "(";
      auto Values = llvm::cast<EnumType>(T)->getValues();
      for (size_t I = 0; I < Values.size(); ++I)
        S += (I ? ", " : "") + Values[I]->getName().Name.str();
      return S + ")";
    }
    default:
      return Name;
    }
  }

  void printBlock(Block &B) {
    for (Decl *D : B.getDecls())
      printDecl(D);
    if (B.getBody())
      visitStmt(B.getBody());
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
      printBlock(*B);
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
  void visitWithStmt(WithStmt *S) {
    line("WithStmt");
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    for (WithContext *W : S->getRecords())
      visitExpr(W->Record);
    visitStmt(S->getBody());
  }

  void visitRealLiteralExpr(RealLiteralExpr *E) {
    line("RealLiteralExpr ") << llvm::format("%g", E->getValue());
    end(E);
  }
  void visitIndexExpr(IndexExpr *E) {
    line("IndexExpr");
    end(E);
    IndentScope I(Indent);
    visitExpr(E->getBase());
    visitExpr(E->getIndex());
  }
  void visitFieldExpr(FieldExpr *E) {
    line("FieldExpr '") << E->getField().Name << '\'';
    end(E);
    IndentScope I(Indent);
    visitExpr(E->getBase());
  }

  void visitNilExpr(NilExpr *E) {
    line("NilExpr");
    end(E);
  }
  void visitDerefExpr(DerefExpr *E) {
    line("DerefExpr");
    end(E);
    IndentScope I(Indent);
    visitExpr(E->getBase());
  }
  void visitSetCtorExpr(SetCtorExpr *E) {
    line("SetCtorExpr");
    end(E);
    IndentScope I(Indent);
    for (const SetElement &El : E->getElements()) {
      visitExpr(El.Lo);
      if (El.Hi)
        visitExpr(El.Hi);
    }
  }

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
    if (E->getWith())
      OS << " (with)";
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
