//===- ASTVisitor.h - CRTP dispatch over AST nodes ----------------*- C++ -*-===//
//
//   class MyPass : public StmtVisitor<MyPass>, public ExprVisitor<MyPass, llvm::Value *> {
//     void visitCompoundStmt(CompoundStmt *S);
//     llvm::Value *visitIntegerLiteralExpr(IntegerLiteralExpr *E);
//     ...
//   };
//
// A derived class must provide visit<Node> for every node in ASTNodes.def, so
// adding a node is a compile error until all passes handle it.
//
//===----------------------------------------------------------------------===//
#ifndef P3_AST_ASTVISITOR_H
#define P3_AST_ASTVISITOR_H

#include "p3/AST/AST.h"
#include "llvm/Support/ErrorHandling.h"

namespace p3 {

template <typename Derived, typename RetTy = void> class ExprVisitor {
public:
  RetTy visitExpr(Expr *E) {
    switch (E->getKind()) {
#define EXPR(CLASS)                                                            \
  case Expr::EK_##CLASS:                                                       \
    return static_cast<Derived *>(this)->visit##CLASS##Expr(                   \
        llvm::cast<CLASS##Expr>(E));
#include "p3/AST/ASTNodes.def"
    }
    llvm_unreachable("unknown expression kind");
  }
};

template <typename Derived, typename RetTy = void> class StmtVisitor {
public:
  RetTy visitStmt(Stmt *S) {
    switch (S->getKind()) {
#define STMT(CLASS)                                                            \
  case Stmt::SK_##CLASS:                                                       \
    return static_cast<Derived *>(this)->visit##CLASS##Stmt(                   \
        llvm::cast<CLASS##Stmt>(S));
#include "p3/AST/ASTNodes.def"
    }
    llvm_unreachable("unknown statement kind");
  }
};

} // namespace p3

#endif // P3_AST_ASTVISITOR_H
