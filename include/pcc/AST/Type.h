//===- Type.h - Semantic types ------------------------------------*- C++ -*-===//
//
// Types as Sema understands them (as opposed to TypeSpec, which is how a type
// is *written*). Predeclared types are singletons owned by the ASTContext, so
// type identity is pointer identity.
//
// M4: integer, boolean, char. M7 adds real, enumerations, subranges;
// M8 arrays/records; M9 pointers/sets; M10 files.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_AST_TYPE_H
#define PCC_AST_TYPE_H

#include "llvm/ADT/StringRef.h"

namespace pcc {

class Type {
public:
  enum TypeKind {
    TK_Error,   ///< result of a failed check; suppresses follow-up errors
    TK_Integer,
    TK_Boolean,
    TK_Char,
    TK_String,  ///< a string literal of length != 1 (only usable in write)
  };

private:
  TypeKind Kind;

public:
  explicit Type(TypeKind K) : Kind(K) {}

  TypeKind getKind() const { return Kind; }
  bool isError() const { return Kind == TK_Error; }
  bool isInteger() const { return Kind == TK_Integer; }
  bool isBoolean() const { return Kind == TK_Boolean; }
  bool isChar() const { return Kind == TK_Char; }
  bool isString() const { return Kind == TK_String; }
  /// ISO 7185 6.4.2.1: ordinal types have succ/pred/ord.
  bool isOrdinal() const { return isInteger() || isBoolean() || isChar(); }

  /// Name for diagnostics, e.g. "integer".
  llvm::StringRef getName() const {
    switch (Kind) {
    case TK_Error:   return "<error>";
    case TK_Integer: return "integer";
    case TK_Boolean: return "boolean";
    case TK_Char:    return "char";
    case TK_String:  return "string";
    }
    return "?";
  }
};

} // namespace pcc

#endif // PCC_AST_TYPE_H
