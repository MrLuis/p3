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
  bool ParseTree = false; ///< --emit-parse-tree: also type specs, units, ...

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
      if (ParseTree) {
        IndentScope I(Indent);
        printTypeSpec(T->getTypeSpec());
      }
      return;
    }
    case Decl::DK_Var: {
      auto *V = llvm::cast<VarDecl>(D);
      const char *Kind = V->getVarKind() == VarDecl::VarParam ? "ParamDecl var '"
                         : V->getVarKind() == VarDecl::ConstParam ? "ParamDecl const '"
                         : V->getVarKind() == VarDecl::ValueParam ? "ParamDecl '"
                                                                  : "VarDecl '";
      line(Kind) << V->getName().Name << '\'';
      if (!ParseTree)
        typeSpec(V->getTypeSpec(), V->getType());
      loc(V->getLocation());
      if (ParseTree) {
        IndentScope I(Indent);
        printTypeSpec(V->getTypeSpec());
        if (V->getInit()) {
          line("Init");
          loc(V->getInit()->getBeginLoc());
          IndentScope J(Indent);
          visitExpr(V->getInit());
        }
      }
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
      if (R->isExternal())
        OS << " external '"
           << (R->getExternalName().empty() ? R->getName().Spelling
                                            : R->getExternalName())
           << '\'';
      if (R->isRedeclaration())
        OS << " definition";
      loc(R->getLocation());
      IndentScope I(Indent);
      if (!R->isRedeclaration())
        for (VarDecl *P : R->getParams())
          printDecl(P);
      if (ParseTree && R->getResultTypeSpec()) {
        line("Result");
        loc(R->getResultTypeSpec()->getBeginLoc());
        IndentScope J(Indent);
        printTypeSpec(R->getResultTypeSpec());
      }
      if (Block *B = R->getBody(); B && !R->isForward())
        printBlock(*B);
      return;
    }
    case Decl::DK_Label:
      line("LabelDecl ") << D->getName().Name;
      loc(D->getLocation());
      return;
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

  void printTypeSpec(TypeSpec *TS) {
    if (!TS)
      return;
    switch (TS->getKind()) {
    case TypeSpec::TS_Named:
      line("NamedTypeSpec '") << llvm::cast<NamedTypeSpec>(TS)->getName().Name << '\'';
      loc(TS->getBeginLoc());
      return;
    case TypeSpec::TS_Enum:
      line("EnumTypeSpec");
      loc(TS->getBeginLoc());
      {
        IndentScope I(Indent);
        for (const Identifier &N : llvm::cast<EnumTypeSpec>(TS)->getNames()) {
          line("EnumValue '") << N.Name << '\'';
          loc(N.Loc);
        }
      }
      return;
    case TypeSpec::TS_Subrange: {
      auto *S = llvm::cast<SubrangeTypeSpec>(TS);
      line("SubrangeTypeSpec");
      loc(TS->getBeginLoc());
      IndentScope I(Indent);
      visitExpr(S->getLo());
      visitExpr(S->getHi());
      return;
    }
    case TypeSpec::TS_Array: {
      auto *A = llvm::cast<ArrayTypeSpec>(TS);
      line(A->isPacked() ? "ArrayTypeSpec packed" : "ArrayTypeSpec");
      loc(TS->getBeginLoc());
      IndentScope I(Indent);
      for (TypeSpec *Idx : A->getIndexes())
        printTypeSpec(Idx);
      printTypeSpec(A->getElement());
      return;
    }
    case TypeSpec::TS_Record: {
      auto *R = llvm::cast<RecordTypeSpec>(TS);
      line(R->isPacked() ? "RecordTypeSpec packed" : "RecordTypeSpec");
      loc(TS->getBeginLoc());
      IndentScope I(Indent);
      printFieldList(R->getFields());
      return;
    }
    case TypeSpec::TS_Pointer:
      line("PointerTypeSpec '") << llvm::cast<PointerTypeSpec>(TS)->getTarget().Name
                                << '\'';
      loc(TS->getBeginLoc());
      return;
    case TypeSpec::TS_Set:
    case TypeSpec::TS_File: {
      bool IsSet = TS->getKind() == TypeSpec::TS_Set;
      line(IsSet ? "SetTypeSpec" : "FileTypeSpec");
      loc(TS->getBeginLoc());
      IndentScope I(Indent);
      printTypeSpec(IsSet ? llvm::cast<SetTypeSpec>(TS)->getElement()
                          : llvm::cast<FileTypeSpec>(TS)->getElement());
      return;
    }
    case TypeSpec::TS_Routine: {
      auto *R = llvm::cast<RoutineTypeSpec>(TS);
      line(R->isFunction() ? "RoutineTypeSpec function" : "RoutineTypeSpec procedure");
      loc(TS->getBeginLoc());
      IndentScope I(Indent);
      for (VarDecl *P : R->getParams())
        printDecl(P);
      if (R->getResult()) {
        line("Result");
        loc(R->getResult()->getBeginLoc());
        IndentScope J(Indent);
        printTypeSpec(R->getResult());
      }
      return;
    }
    case TypeSpec::TS_String: {
      auto *S = llvm::cast<StringTypeSpec>(TS);
      line("StringTypeSpec");
      loc(TS->getBeginLoc());
      if (S->getMaxLength()) {
        IndentScope I(Indent);
        visitExpr(S->getMaxLength());
      }
      return;
    }
    }
  }

  void printFieldList(FieldList *FL) {
    for (FieldDecl *F : FL->Fixed) {
      line("FieldDecl '") << F->getName().Name << '\'';
      loc(F->getLocation());
      IndentScope I(Indent);
      printTypeSpec(F->getTypeSpec());
    }
    VariantPart *VP = FL->Variant;
    if (!VP)
      return;
    line("VariantPart");
    loc(VP->CaseLoc);
    IndentScope I(Indent);
    if (VP->Tag) {
      line("TagField '") << VP->Tag->getName().Name << '\'';
      loc(VP->Tag->getLocation());
    }
    printTypeSpec(VP->TagTypeSpec);
    for (Variant *V : VP->Variants) {
      line("Variant");
      loc(V->Labels.empty() ? VP->CaseLoc : V->Labels[0]->Lo->getBeginLoc());
      IndentScope J(Indent);
      for (CaseLabel *L : V->Labels) {
        line("CaseLabel");
        loc(L->Lo->getBeginLoc());
        IndentScope K(Indent);
        visitExpr(L->Lo);
      }
      printFieldList(V->Fields);
    }
  }

  void printUses(llvm::ArrayRef<Identifier> Uses) {
    for (const Identifier &U : Uses) {
      line("Uses '") << U.Name << '\'';
      loc(U.Loc);
    }
  }

  void printBlock(Block &B) {
    for (Decl *D : B.getDecls())
      printDecl(D);
    if (B.getBody())
      visitStmt(B.getBody());
  }

public:
  ASTPrinter(const SourceManager &SM, llvm::raw_ostream &OS, bool ParseTree = false)
      : SM(SM), OS(OS), ParseTree(ParseTree) {}

  void printUnit(UnitDecl &U) {
    line("UnitDecl '") << U.getName().Name << '\'';
    loc(U.getLocation());
    IndentScope S(Indent);
    line("Interface");
    loc(U.getLocation());
    {
      IndentScope I(Indent);
      printUses(U.getInterfaceUses());
      for (Decl *D : U.getInterface()->getDecls())
        printDecl(D);
    }
    line("Implementation");
    loc(U.getLocation());
    IndentScope I(Indent);
    printUses(U.getImplementationUses());
    printBlock(*U.getImplementation());
  }

  void printProgram(ProgramDecl &P) {
    line("ProgramDecl '") << P.getName().Name << '\'';
    loc(P.getLocation());
    IndentScope S(Indent);
    for (const Identifier &Param : P.getParams()) {
      line("ProgramParam '") << Param.Name << '\'';
      loc(Param.Loc);
    }
    if (ParseTree)
      printUses(P.getUses());
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
    if (ParseTree && S->hasParens())
      OS << " ()";
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    for (Expr *Arg : S->getArgs())
      visitExpr(Arg);
  }
  void visitIndirectCallStmt(IndirectCallStmt *S) {
    line("IndirectCallStmt");
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    visitExpr(S->getCall());
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
  void visitLabeledStmt(LabeledStmt *S) {
    line("LabeledStmt ") << S->getLabel().Name;
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    visitStmt(S->getBody());
  }
  void visitGotoStmt(GotoStmt *S) {
    line("GotoStmt ") << S->getLabel().Name;
    if (S->getDecl() && S->getDecl()->isNonLocal())
      OS << " non-local";
    loc(S->getBeginLoc());
  }

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

  void visitAggregateExpr(AggregateExpr *E) {
    line("AggregateExpr");
    end(E);
    IndentScope I(Indent);
    for (AggregateElement *El : E->getElements()) {
      if (El->Field.isValid()) {
        line("Field '") << El->Field.Name << '\'';
        loc(El->Field.Loc);
        IndentScope J(Indent);
        visitExpr(El->Value);
      } else {
        visitExpr(El->Value);
      }
    }
  }

  void visitAddrOfExpr(AddrOfExpr *E) {
    line("AddrOfExpr");
    end(E);
    IndentScope I(Indent);
    visitExpr(E->getOperand());
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
  void visitIndirectCallExpr(IndirectCallExpr *E) {
    line("IndirectCallExpr");
    end(E);
    IndentScope I(Indent);
    visitExpr(E->getCallee());
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

void pcc::printParseTree(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units,
                         const SourceManager &SM, llvm::raw_ostream &OS) {
  ASTPrinter Printer(SM, OS, /*ParseTree=*/true);
  for (UnitDecl *U : Units)
    Printer.printUnit(*U);
  Printer.printProgram(P);
}
