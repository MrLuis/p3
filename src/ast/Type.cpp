#include "pcc/AST/Type.h"
#include "pcc/AST/AST.h"

#include "llvm/ADT/StringExtras.h"

using namespace pcc;

bool Type::isIntegerLike() const {
  const Type *T = this;
  while (auto *S = llvm::dyn_cast<SubrangeType>(T))
    T = S->getHost();
  return T->isInteger();
}

Type *Type::getBase() {
  Type *T = this;
  while (auto *S = llvm::dyn_cast<SubrangeType>(T))
    T = S->getHost();
  return T;
}

bool Type::isStringType() const {
  if (isStringLiteral())
    return true;
  auto *A = llvm::dyn_cast<ArrayType>(this);
  if (!A || !A->isPacked() || !A->getElementType()->isChar())
    return false;
  auto *Idx = llvm::dyn_cast<SubrangeType>(A->getIndexType());
  return Idx && Idx->getHost()->isInteger() && Idx->getLo() == 1 &&
         Idx->getHi() >= 1;
}

uint64_t Type::getStringLength() const {
  if (auto *S = llvm::dyn_cast<StringLiteralType>(this))
    return S->getLength();
  auto *A = llvm::cast<ArrayType>(this);
  return static_cast<uint64_t>(
      llvm::cast<SubrangeType>(A->getIndexType())->getHi());
}

FieldDecl *RecordType::findField(llvm::StringRef Name) const {
  for (FieldDecl *F : AllFields)
    if (F->getName().Name == Name)
      return F;
  return nullptr;
}

OrdinalBounds pcc::getOrdinalBounds(Type *T) {
  switch (T->getKind()) {
  case Type::TK_Integer: return {INT32_MIN, INT32_MAX};
  case Type::TK_Char:    return {0, 255};
  case Type::TK_Boolean: return {0, 1};
  case Type::TK_Enum:
    return {0, static_cast<int64_t>(llvm::cast<EnumType>(T)->getValues().size()) - 1};
  case Type::TK_Subrange: {
    auto *S = llvm::cast<SubrangeType>(T);
    return {S->getLo(), S->getHi()};
  }
  default:
    return {0, -1};
  }
}

static std::string formatOrdinal(Type *T, int64_t V) {
  Type *B = T->getBase();
  if (B->isChar())
    return llvm::isPrint(static_cast<char>(V))
               ? "'" + std::string(1, static_cast<char>(V)) + "'"
               : "chr(" + std::to_string(V) + ")";
  if (B->isBoolean())
    return V ? "true" : "false";
  if (auto *E = llvm::dyn_cast<EnumType>(B))
    if (V >= 0 && static_cast<size_t>(V) < E->getValues().size())
      return E->getValues()[V]->getName().Spelling.str();
  return std::to_string(V);
}

std::string Type::getName() const {
  if (!DeclName.empty())
    return DeclName.str();
  switch (Kind) {
  case TK_Error:   return "<error>";
  case TK_Integer: return "integer";
  case TK_Real:    return "real";
  case TK_Boolean: return "boolean";
  case TK_Char:    return "char";
  case TK_String:
    return "string of length " +
           std::to_string(llvm::cast<StringLiteralType>(this)->getLength());
  case TK_Enum: {
    std::string S = "(";
    auto Values = llvm::cast<EnumType>(this)->getValues();
    for (size_t I = 0; I < Values.size(); ++I)
      S += (I ? ", " : "") + Values[I]->getName().Spelling.str();
    return S + ")";
  }
  case TK_Subrange: {
    auto *S = llvm::cast<SubrangeType>(this);
    return formatOrdinal(S->getHost(), S->getLo()) + ".." +
           formatOrdinal(S->getHost(), S->getHi());
  }
  case TK_Array: {
    auto *A = llvm::cast<ArrayType>(this);
    return std::string(A->isPacked() ? "packed " : "") + "array [" +
           A->getIndexType()->getName() + "] of " +
           A->getElementType()->getName();
  }
  case TK_Record:
    return llvm::cast<RecordType>(this)->isPacked() ? "packed record"
                                                    : "record";
  case TK_Pointer: {
    Type *P = llvm::cast<PointerType>(this)->getPointee();
    return "^" + (P ? P->getName() : std::string("?"));
  }
  case TK_Nil:
    return "nil";
  case TK_Set: {
    Type *E = llvm::cast<SetType>(this)->getElementType();
    return E ? "set of " + E->getName() : "empty set";
  }
  }
  return "?";
}
