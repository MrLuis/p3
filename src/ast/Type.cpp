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

bool Type::isSignedIntegerLike() const {
  const Type *T = this;
  while (auto *S = llvm::dyn_cast<SubrangeType>(T))
    T = S->getHost();
  auto *I = llvm::dyn_cast<IntegerType>(T);
  return I && I->isSigned();
}

bool pcc::isQWord(const Type *T) {
  auto *I = llvm::dyn_cast<IntegerType>(T);
  return I && I->getBits() == 64 && !I->isSigned();
}

bool pcc::haveSameOrdinalHost(Type *A, Type *B) {
  Type *X = A->getBase(), *Y = B->getBase();
  return X == Y || (X->isInteger() && Y->isInteger()) ||
         (A->isCharLike() && B->isCharLike()); // char and widechar
}

bool Type::isCharLike() const {
  Type *B = const_cast<Type *>(this)->getBase();
  return B->isChar() || B->isWideChar();
}

Type *Type::getBase() {
  Type *T = this;
  while (auto *S = llvm::dyn_cast<SubrangeType>(T))
    T = S->getHost();
  return T;
}

static unsigned bitLength(uint64_t V) {
  unsigned N = 0;
  while (V) {
    ++N;
    V >>= 1;
  }
  return N;
}

unsigned pcc::bitPackedWidth(Type *T) {
  switch (T->getKind()) {
  case Type::TK_Integer:
    return llvm::cast<IntegerType>(T)->getBits();
  case Type::TK_Boolean:
    return 1;
  case Type::TK_Char:
    return 8;
  case Type::TK_WideChar:
    return 16;
  case Type::TK_Enum: {
    size_t N = llvm::cast<EnumType>(T)->getValues().size();
    return std::max(1u, bitLength(N ? N - 1 : 0));
  }
  case Type::TK_Subrange: {
    auto *S = llvm::cast<SubrangeType>(T);
    int64_t Lo = S->getLo(), Hi = S->getHi();
    if (Lo >= 0)
      return std::max(1u, bitLength(static_cast<uint64_t>(Hi)));
    unsigned Neg = bitLength(static_cast<uint64_t>(-(Lo + 1)));
    unsigned Pos = Hi > 0 ? bitLength(static_cast<uint64_t>(Hi)) : 0;
    return std::min(64u, std::max(Neg, Pos) + 1);
  }
  default:
    return 0;
  }
}

bool pcc::bitPackedSigned(Type *T) {
  if (auto *I = llvm::dyn_cast<IntegerType>(T))
    return I->isSigned();
  if (auto *S = llvm::dyn_cast<SubrangeType>(T))
    return S->getLo() < 0;
  return false;
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

bool Type::isStringish() const {
  return isShortString() || isAnsiString() || isStringType() || isCharLike();
}

bool Type::isTextFile() const {
  auto *F = llvm::dyn_cast<FileType>(this);
  return F && F->isText();
}

bool pcc::containsFile(Type *T) {
  if (T->isFile())
    return true;
  if (auto *A = llvm::dyn_cast<ArrayType>(T))
    return containsFile(A->getElementType());
  if (auto *R = llvm::dyn_cast<RecordType>(T)) {
    for (FieldDecl *F : R->getAllFields())
      if (F->getType() && containsFile(F->getType()))
        return true;
  }
  return false;
}

bool pcc::containsRoutineType(Type *T) {
  if (llvm::isa<RoutineType>(T))
    return true;
  if (auto *A = llvm::dyn_cast<ArrayType>(T))
    return containsRoutineType(A->getElementType());
  if (auto *R = llvm::dyn_cast<RecordType>(T)) {
    for (FieldDecl *F : R->getAllFields())
      if (F->getType() && containsRoutineType(F->getType()))
        return true;
  }
  return false;
}

bool pcc::containsInterface(Type *T) {
  if (auto *I = llvm::dyn_cast<InterfaceType>(T))
    return I->isCOM();
  if (auto *A = llvm::dyn_cast<ArrayType>(T))
    return containsInterface(A->getElementType());
  if (auto *R = llvm::dyn_cast<RecordType>(T)) {
    for (FieldDecl *F : R->getAllFields())
      if (F->getType() && containsInterface(F->getType()))
        return true;
  }
  return false;
}

bool Type::isClosureType() const {
  auto *R = llvm::dyn_cast<RoutineType>(this);
  return R && R->isReference();
}

bool pcc::isManaged(Type *T) {
  if (T->isAnsiString() || T->isDynArray())
    return true;
  if (auto *R = llvm::dyn_cast<RoutineType>(T))
    return R->isReference();
  if (auto *O = llvm::dyn_cast<OpenArrayType>(T))
    return isManaged(O->getElementType());
  if (auto *I = llvm::dyn_cast<InterfaceType>(T))
    return I->isCOM();
  if (auto *A = llvm::dyn_cast<ArrayType>(T))
    return isManaged(A->getElementType());
  if (auto *R = llvm::dyn_cast<RecordType>(T)) {
    for (FieldDecl *F : R->getAllFields())
      if (F->getType() && isManaged(F->getType()))
        return true;
  }
  return false;
}

Decl *ClassType::findOwnMember(llvm::StringRef Name) const {
  for (Decl *D : Members)
    if (D->getName().Name == Name)
      return D;
  return nullptr;
}

Decl *ClassType::findMember(llvm::StringRef Name,
                            const ClassType **Owner) const {
  for (const ClassType *C = this; C; C = C->Parent)
    if (Decl *D = C->findOwnMember(Name)) {
      if (Owner)
        *Owner = C;
      return D;
    }
  return nullptr;
}

bool ClassType::inheritsFrom(const ClassType *C) const {
  for (const ClassType *X = this; X; X = X->Parent)
    if (X == C)
      return true;
  return false;
}

FieldDecl *RecordType::findField(llvm::StringRef Name) const {
  for (FieldDecl *F : AllFields)
    if (F->getName().Name == Name)
      return F;
  return nullptr;
}

unsigned SetType::getStorageBits() const {
  if (!Element)
    return 32;
  int64_t Hi = getOrdinalBounds(Element).Hi;
  return Hi <= 31 ? 32 : Hi <= 63 ? 64 : 256;
}

OrdinalBounds pcc::getOrdinalBounds(Type *T) {
  switch (T->getKind()) {
  case Type::TK_Integer: {
    auto *I = llvm::cast<IntegerType>(T);
    unsigned Bits = I->getBits();
    if (I->isSigned())
      return {Bits == 64 ? INT64_MIN : -(int64_t(1) << (Bits - 1)),
              Bits == 64 ? INT64_MAX : (int64_t(1) << (Bits - 1)) - 1};
    if (Bits == 64) // qword: its whole range does not fit (see isQWord)
      return {0, INT64_MAX};
    return {0, (int64_t(1) << Bits) - 1};
  }
  case Type::TK_Char:    return {0, 255};
  case Type::TK_WideChar: return {0, 65535};
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
  if (B->isWideChar() && (V > 127 || !llvm::isPrint(static_cast<char>(V))))
    return "#" + std::to_string(V);
  if (B->isChar() || B->isWideChar())
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
  case TK_Integer:
    return llvm::cast<IntegerType>(this)->getIntegerName().str();
  case TK_Real:    return "real";
  case TK_Boolean: return "boolean";
  case TK_Char:    return "char";
  case TK_WideChar: return "widechar";
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
    return std::string(A->isBitPacked() ? "bitpacked "
                       : A->isPacked() ? "packed "
                                       : "") +
           "array [" + A->getIndexType()->getName() + "] of " +
           A->getElementType()->getName();
  }
  case TK_Record:
    if (llvm::cast<RecordType>(this)->isTuple()) { // (integer, Max: real)
      std::string S = "(";
      unsigned I = 0;
      for (FieldDecl *F : llvm::cast<RecordType>(this)->getAllFields()) {
        if (I++)
          S += ", ";
        if (F->getName().Name != "item" + std::to_string(I))
          S += F->getName().Spelling.str() + ": ";
        S += F->getType() ? F->getType()->getName() : std::string("?");
      }
      return S + ")";
    }
    return llvm::cast<RecordType>(this)->isBitPacked() ? "bitpacked record"
           : llvm::cast<RecordType>(this)->isPacked()  ? "packed record"
                                                       : "record";
  case TK_Pointer: {
    if (llvm::cast<PointerType>(this)->isUntyped())
      return "pointer";
    Type *P = llvm::cast<PointerType>(this)->getPointee();
    return "^" + (P ? P->getName() : std::string("?"));
  }
  case TK_Nil:
    return "nil";
  case TK_Untyped:
    return "untyped";
  case TK_DynArray:
    return "array of " + llvm::cast<DynArrayType>(this)->getElementType()->getName();
  case TK_OpenArray:
    return "array of " + llvm::cast<OpenArrayType>(this)->getElementType()->getName();
  case TK_ArrayOfConst:
    return "array of const";
  case TK_AnsiString:
    return "ansistring";
  case TK_ShortString: {
    unsigned N = llvm::cast<ShortStringType>(this)->getMaxLength();
    return N == 255 ? "string" : "string[" + std::to_string(N) + "]";
  }
  case TK_Routine: {
    auto *R = llvm::cast<RoutineType>(this);
    std::string S = R->isReference() ? "reference to " : "";
    S += R->isFunction() ? "function(" : "procedure(";
    for (size_t I = 0; I < R->getParams().size(); ++I) {
      VarDecl *P = R->getParams()[I];
      S += (I ? "; " : "") +
           std::string(P->isOutParam()     ? "out "
                       : P->isVarParam()   ? "var "
                       : P->isConstRefParam() ? "constref "
                       : P->isConstParam() ? "const "
                                           : "") +
           (P->getType() ? P->getType()->getName() : "?");
    }
    S += ")";
    if (R->isFunction())
      S += ": " + R->getResult()->getName();
    if (R->isOfObject())
      S += " of object";
    if (R->isCDecl())
      S += "; cdecl";
    return S;
  }
  case TK_File: {
    auto *F = llvm::cast<FileType>(this);
    return F->isText()      ? "text"
           : F->isUntyped() ? "file"
                            : "file of " + F->getElementType()->getName();
  }
  case TK_Set: {
    Type *E = llvm::cast<SetType>(this)->getElementType();
    return E ? "set of " + E->getName() : "empty set";
  }
  case TK_Class:
    return "class";
  case TK_ClassRef:
    return "class of " + llvm::cast<ClassRefType>(this)->getClass()->getName();
  case TK_Interface:
    return "interface";
  }
  return "?";
}
