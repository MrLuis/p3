#include "pcc/AST/AST.h"

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
  }
  return "?";
}

bool pcc::isRelational(BinaryOp Op) {
  switch (Op) {
  case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt: case BinaryOp::Le:
  case BinaryOp::Gt: case BinaryOp::Ge: case BinaryOp::In:
    return true;
  default:
    return false;
  }
}

const char *Decl::getKindName() const {
  switch (Kind) {
  case DK_Program: return "program";
  case DK_Var:     return "variable";
  case DK_Const:   return "constant";
  case DK_Type:    return "type";
  case DK_Builtin:
    return llvm::cast<BuiltinDecl>(this)->isProcedure() ? "procedure"
                                                        : "function";
  }
  return "declaration";
}
