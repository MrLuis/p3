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
    if (RoutineDecl *Op = E->getConversion()) { // a record's := / Explicit
      OS << " converted by '" << Op->getName().Name << '\'';
      overloadNumber(Op);
    }
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
      hints(C);
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
      hints(T);
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
      if (V->isClassVar())
        OS << " class";
      if (V->isInitializedVar())
        OS << " initialized";
      visibility(V);
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
      line(R->isConstructor()  ? "ConstructorDecl '"
           : R->isDestructor() ? "DestructorDecl '"
           : R->isOperator()   ? "OperatorDecl '"
           : R->isFunction()   ? "FunctionDecl '"
                               : "ProcedureDecl '");
      if (R->getClassName().isValid())
        OS << R->getClassName().Name << '.';
      OS << R->getName().Name << '\'';
      if (R->getResultType())
        type(R->getResultType());
      if (R->isClassMethod())
        OS << " class";
      visibility(R);
      if (R->isVirtual())
        OS << " virtual";
      if (R->isOverride())
        OS << " override";
      if (R->isAbstract())
        OS << " abstract";
      if (R->hasOverloadDirective())
        OS << " overload";
      if (R->isInline())
        OS << " inline";
      if (R->getCallConv() != CallConv::Default)
        OS << ' ' << callConvName(R->getCallConv());
      overloadNumber(R);
      if (R->isForward())
        OS << " forward";
      if (R->isExternal()) {
        OS << " external";
        // (a symbol name given as a constant: known after Sema)
        if (!R->getExternalName().empty() || !R->getNameExpr())
          OS << " '"
             << (R->getExternalName().empty() ? R->getName().Spelling
                                              : R->getExternalName())
             << '\'';
        if (!R->getLibraryName().empty())
          OS << " in '" << R->getLibraryName() << '\'';
        if (R->isVarArgs())
          OS << " varargs";
      }
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
      // external library and name constants (not literals)
      for (auto [What, E] : {std::pair{"ExternalLibrary", R->getLibraryExpr()},
                             std::pair{"ExternalName", R->getNameExpr()}})
        if (ParseTree && E && !llvm::isa<StringLiteralExpr>(E)) {
          line(What);
          loc(E->getBeginLoc());
          IndentScope J(Indent);
          visitExpr(E);
        }
      if (Block *B = R->getBody(); B && !R->isForward())
        printBlock(*B);
      return;
    }
    case Decl::DK_Label:
      line("LabelDecl ") << D->getName().Name;
      loc(D->getLocation());
      return;
    case Decl::DK_Field: { // a field of a class
      auto *F = llvm::cast<FieldDecl>(D);
      line("FieldDecl '") << F->getName().Name << '\'';
      if (!ParseTree)
        typeSpec(F->getTypeSpec(), F->getType());
      visibility(F);
      loc(F->getLocation());
      if (ParseTree) {
        IndentScope I(Indent);
        printTypeSpec(F->getTypeSpec());
      }
      return;
    }
    case Decl::DK_Property: {
      auto *P = llvm::cast<PropertyDecl>(D);
      line("PropertyDecl '") << P->getName().Name << '\'';
      if (!ParseTree)
        typeSpec(P->getTypeSpec(), P->getType());
      if (P->getReadName().isValid())
        OS << " read '" << P->getReadName().Name << '\'';
      if (P->getWriteName().isValid())
        OS << " write '" << P->getWriteName().Name << '\'';
      if (P->isDefault())
        OS << " default";
      if (P->isNoDefault())
        OS << " nodefault";
      visibility(P);
      loc(P->getLocation());
      IndentScope I(Indent);
      for (VarDecl *Param : P->getParams()) // an array property's indexes
        printDecl(Param);
      if (ParseTree)
        printTypeSpec(P->getTypeSpec());
      if (Expr *E = P->getIndexSpec()) {
        line("PropertyIndex");
        loc(E->getBeginLoc());
        IndentScope J(Indent);
        visitExpr(E);
      }
      if (Expr *E = P->getDefaultValue()) {
        line("PropertyDefault");
        loc(E->getBeginLoc());
        IndentScope J(Indent);
        visitExpr(E);
      }
      return;
    }
    default:
      line("Decl '") << D->getName().Name << '\'';
      loc(D->getLocation());
      return;
    }
  }

  /// " #N": which routine of an overloaded name (objfpc) is meant.
  /// A record's class operator implementing an operation (Sema).
  void operatorOf(RoutineDecl *Op, bool Negated) {
    if (!Op)
      return;
    OS << (Negated ? " not " : " ") << "by '" << Op->getName().Name << '\'';
    overloadNumber(Op);
  }

  void overloadNumber(Decl *D) {
    if (auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D); R && R->isOverloaded())
      OS << " #" << R->getOverloadIndex();
  }

  /// Hint directives (deprecated 'message', experimental, ...).
  void hints(Decl *D) {
    unsigned char H = D->getHints();
    if (H & Decl::HintDeprecated) {
      OS << " deprecated";
      if (!D->getHintMessage().empty()) {
        OS << ' ';
        quoted(D->getHintMessage());
      }
    }
    if (H & Decl::HintExperimental)
      OS << " experimental";
    if (H & Decl::HintPlatform)
      OS << " platform";
    if (H & Decl::HintLibrary)
      OS << " library";
    if (H & Decl::HintUnimplemented)
      OS << " unimplemented";
  }

  void visibility(Decl *D) {
    hints(D);
    switch (D->getVisibility()) {
    case Visibility::Public:    break;
    case Visibility::Published: OS << " published"; break;
    case Visibility::Protected: OS << " protected"; break;
    case Visibility::Private:   OS << " private"; break;
    case Visibility::StrictProtected: OS << " strict protected"; break;
    case Visibility::StrictPrivate:   OS << " strict private"; break;
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
    case Type::TK_Class: {
      auto *C = llvm::cast<ClassType>(T);
      std::string S = C->getParent() ? "class(" + C->getParent()->getName()
                                     : std::string("class");
      for (const InterfaceImpl &I : C->getInterfaces())
        S += (S == "class" ? "(" : ", ") + I.Intf->getName();
      return S == "class" ? S : S + ")";
    }
    case Type::TK_Interface: {
      auto *I = llvm::cast<InterfaceType>(T);
      return I->getParent() ? "interface(" + I->getParent()->getName() + ")"
                            : std::string("interface");
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
    case TypeSpec::TS_OpenArray: {
      line("OpenArrayTypeSpec");
      loc(TS->getBeginLoc());
      IndentScope I(Indent);
      printTypeSpec(llvm::cast<OpenArrayTypeSpec>(TS)->getElement());
      return;
    }
    case TypeSpec::TS_ArrayOfConst:
      line("ArrayOfConstTypeSpec");
      loc(TS->getBeginLoc());
      return;
    case TypeSpec::TS_Untyped:
      line("UntypedTypeSpec");
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
      for (Decl *M : R->getMembers()) // an advanced record's
        printDecl(M);
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
      line(std::string(R->isFunction() ? "RoutineTypeSpec function" : "RoutineTypeSpec procedure") +
           (R->isOfObject() ? " of object" : ""));
      if (R->getCallConv() != CallConv::Default)
        OS << ' ' << callConvName(R->getCallConv());
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
    case TypeSpec::TS_Class: {
      auto *C = llvm::cast<ClassTypeSpec>(TS);
      line(C->isForward() ? "ClassTypeSpec forward" : "ClassTypeSpec");
      if (C->getParent().isValid())
        OS << " '" << C->getParent().Name << '\'';
      for (const Identifier &I : C->getInterfaces())
        OS << ", '" << I.Name << '\'';
      loc(TS->getBeginLoc());
      IndentScope I(Indent);
      for (Decl *M : C->getMembers())
        printDecl(M);
      return;
    }
    case TypeSpec::TS_Interface: {
      auto *I = llvm::cast<InterfaceTypeSpec>(TS);
      line(I->isForward() ? "InterfaceTypeSpec forward" : "InterfaceTypeSpec");
      if (!I->isCorba())
        OS << " com";
      if (I->getParent().isValid())
        OS << " '" << I->getParent().Name << '\'';
      if (!I->getGUID().empty()) {
        OS << ' ';
        quoted(I->getGUID());
      }
      loc(TS->getBeginLoc());
      IndentScope J(Indent);
      for (Decl *M : I->getMembers())
        printDecl(M);
      return;
    }
    case TypeSpec::TS_ClassRef:
      line("ClassRefTypeSpec '")
          << llvm::cast<ClassRefTypeSpec>(TS)->getClassName().Name << '\'';
      loc(TS->getBeginLoc());
      return;
    case TypeSpec::TS_String: {
      auto *S = llvm::cast<StringTypeSpec>(TS);
      line(S->isLong() ? "StringTypeSpec long" : "StringTypeSpec");
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
      visibility(F);
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
    overloadNumber(S->getDecl());
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

  // try (objfpc):
  //   TryStmt / TryBody stmts / ExceptPart [OnHandler... ExceptElse] stmts
  //   or FinallyPart stmts
  void visitTryStmt(TryStmt *S) {
    line("TryStmt");
    loc(S->getBeginLoc());
    IndentScope I(Indent);
    line("TryBody");
    loc(S->getBeginLoc());
    {
      IndentScope J(Indent);
      for (Stmt *Child : S->getBody())
        visitStmt(Child);
    }
    line(S->isFinally() ? "FinallyPart" : "ExceptPart");
    loc(S->getPartLoc());
    IndentScope J(Indent);
    for (ExceptHandler *H : S->getHandlers()) {
      line("OnHandler");
      if (H->getVarName().isValid())
        OS << " '" << H->getVarName().Name << "':";
      OS << " '" << H->getClassName().Name << '\'';
      loc(H->getOnLoc());
      IndentScope K(Indent);
      visitStmt(H->getBody());
    }
    if (S->hasElse()) {
      line("ExceptElse");
      loc(S->getElseLoc());
      IndentScope K(Indent);
      for (Stmt *Child : S->getExceptBody())
        visitStmt(Child);
      return;
    }
    for (Stmt *Child : S->getExceptBody())
      visitStmt(Child);
  }
  void visitRaiseStmt(RaiseStmt *S) {
    line("RaiseStmt");
    loc(S->getBeginLoc());
    if (S->getObject()) {
      IndentScope I(Indent);
      visitExpr(S->getObject());
    }
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
    if (PropertyDecl *P = E->getProperty()) // objfpc: an array property
      OS << " property '" << P->getName().Name << '\'';
    end(E);
    IndentScope I(Indent);
    visitExpr(E->getBase());
    visitExpr(E->getIndex());
  }
  void visitFieldExpr(FieldExpr *E) {
    line("FieldExpr '") << E->getField().Name << '\'';
    overloadNumber(E->getMember());
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
    overloadNumber(E->getDecl());
    if (E->getWith())
      OS << (E->getWith()->IsSelf ? " (self)" : " (with)");
    end(E);
  }
  void visitInheritedExpr(InheritedExpr *E) {
    line("InheritedExpr");
    if (!E->isBare())
      OS << " '" << E->getName().Name << '\'';
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
    operatorOf(E->getOperator(), false);
    end(E);
    IndentScope I(Indent);
    visitExpr(E->getOperand());
  }
  void visitBinaryExpr(BinaryExpr *E) {
    line("BinaryExpr '") << getOpSpelling(E->getOp()) << '\'';
    operatorOf(E->getOperator(), E->isNegated());
    end(E);
    IndentScope I(Indent);
    visitExpr(E->getLHS());
    visitExpr(E->getRHS());
  }
  void visitCallExpr(CallExpr *E) {
    line("CallExpr '") << E->getCallee().Name << '\'';
    overloadNumber(E->getDecl());
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
