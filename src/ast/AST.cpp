#include "pcc/AST/AST.h"
#include "pcc/AST/Type.h"

using namespace pcc;

const char *pcc::getOpSpelling(UnaryOp Op) {
  switch (Op) {
  case UnaryOp::Plus:  return "+";
  case UnaryOp::Minus: return "-";
  case UnaryOp::Not:   return "not";
  }
  return "?";
}

const char *pcc::getOpSpelling(BinaryOp Op) {
  switch (Op) {
  case BinaryOp::Add:     return "+";
  case BinaryOp::Sub:     return "-";
  case BinaryOp::Or:      return "or";
  case BinaryOp::Xor:     return "xor";
  case BinaryOp::Shl:     return "shl";
  case BinaryOp::Shr:     return "shr";
  case BinaryOp::Mul:     return "*";
  case BinaryOp::RealDiv: return "/";
  case BinaryOp::IntDiv:  return "div";
  case BinaryOp::Mod:     return "mod";
  case BinaryOp::And:     return "and";
  case BinaryOp::Eq:      return "=";
  case BinaryOp::Ne:      return "<>";
  case BinaryOp::Lt:      return "<";
  case BinaryOp::Le:      return "<=";
  case BinaryOp::Gt:      return ">";
  case BinaryOp::Ge:      return ">=";
  case BinaryOp::In:      return "in";
  case BinaryOp::Is:      return "is";
  case BinaryOp::As:      return "as";
  }
  return "?";
}

bool pcc::isRelational(BinaryOp Op) {
  switch (Op) {
  case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt: case BinaryOp::Le:
  case BinaryOp::Gt: case BinaryOp::Ge: case BinaryOp::In: case BinaryOp::Is:
    return true;
  default:
    return false;
  }
}

const char *Decl::getKindName() const {
  switch (Kind) {
  case DK_Program: return "program";
  case DK_Var:
    switch (llvm::cast<VarDecl>(this)->getVarKind()) {
    case VarDecl::Variable: return "variable";
    case VarDecl::Result:   return "function result";
    default:                return "parameter";
    }
  case DK_Const:   return "constant";
  case DK_Type:    return "type";
  case DK_Field:   return "field";
  case DK_Label:   return "label";
  case DK_Unit:    return "unit";
  case DK_Property: return "property";
  case DK_Routine: {
    auto *R = llvm::cast<RoutineDecl>(this);
    if (R->isConstructor())
      return "constructor";
    if (R->isDestructor())
      return "destructor";
    if (R->isOperator())
      return "operator";
    return R->isFunction() ? "function" : "procedure";
  }
  case DK_Builtin:
    return llvm::cast<BuiltinDecl>(this)->isProcedure() ? "procedure"
                                                        : "function";
  }
  return "declaration";
}

bool pcc::isBitPackedComponent(Expr *E) {
  if (auto *I = llvm::dyn_cast<IndexExpr>(E)) {
    auto *A = I->getProperty() || !I->getBase()->getType()
                  ? nullptr
                  : llvm::dyn_cast<ArrayType>(I->getBase()->getType());
    return A && A->hasBitElements();
  }
  FieldDecl *F = nullptr;
  Type *RT = nullptr;
  if (auto *FE = llvm::dyn_cast<FieldExpr>(E)) {
    F = llvm::dyn_cast_or_null<FieldDecl>(FE->getMember());
    RT = FE->getBase()->getType();
  } else if (auto *N = llvm::dyn_cast<NameRefExpr>(E); N && N->getWith()) {
    F = llvm::dyn_cast_or_null<FieldDecl>(N->getDecl());
    RT = N->getWith()->Record->getType();
  }
  auto *R = llvm::dyn_cast_or_null<RecordType>(RT);
  return F && R && R->isBitPacked() && F->getType() && F->getType()->isOrdinal();
}
