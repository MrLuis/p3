//===- ASTContext.h - Owns all AST nodes and types ----------------*- C++ -*-===//
//
// AST nodes and types are allocated in a bump-pointer arena and freed all at
// once when the context dies. Nodes must therefore be trivially
// destructible: they hold StringRefs (into the StringPool / source buffers),
// ArrayRefs (copied into the arena) and pointers to other nodes.
//
//===----------------------------------------------------------------------===//
#ifndef P3_AST_ASTCONTEXT_H
#define P3_AST_ASTCONTEXT_H

#include "p3/AST/Type.h"
#include "p3/Basic/StringPool.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Allocator.h"

#include <memory>
#include <type_traits>

namespace p3 {

class ASTContext {
  llvm::BumpPtrAllocator Alloc;
  StringPool &Strings;

  // Predeclared types (singletons).
  BuiltinType ErrorTy{Type::TK_Error};
  IntegerType IntegerTy{32, true, "integer"};
  // Turbo/FPC integer types.
  IntegerType Int64Ty{64, true, "int64"};
  IntegerType CardinalTy{32, false, "cardinal"};
  IntegerType QWordTy{64, false, "qword"};
  IntegerType ByteTy{8, false, "byte"};
  IntegerType WordTy{16, false, "word"};
  IntegerType ShortIntTy{8, true, "shortint"};
  IntegerType SmallIntTy{16, true, "smallint"};
  BuiltinType RealTy{Type::TK_Real};
  BuiltinType BooleanTy{Type::TK_Boolean};
  BuiltinType CharTy{Type::TK_Char};
  BuiltinType WideCharTy{Type::TK_WideChar};
  BuiltinType NilTy{Type::TK_Nil};
  BuiltinType UntypedTy{Type::TK_Untyped};
  SetType EmptySetTy{nullptr};
  FileType TextTy{&CharTy, /*IsText=*/true};
  PointerType UntypedPointerTy{nullptr, /*Untyped=*/true};
  llvm::DenseMap<Type *, SetType *> SetTypes;
  llvm::DenseMap<uint64_t, StringLiteralType *> StringLiteralTypes;
  llvm::DenseMap<unsigned, ShortStringType *> ShortStringTypes;
  Type *AnsiString = nullptr;
  llvm::DenseMap<Type *, OpenArrayType *> OpenArrayTypes;
  llvm::DenseMap<Type *, DynArrayType *> DynArrayTypes;
  llvm::DenseMap<Type *, TaskType *> TaskTypes;
  TokenType *TokenTy = nullptr;

public:
  explicit ASTContext(StringPool &Strings) : Strings(Strings) {}
  ASTContext(const ASTContext &) = delete;
  ASTContext &operator=(const ASTContext &) = delete;

  StringPool &getStrings() { return Strings; }

  Type *getErrorType() { return &ErrorTy; }
  Type *getIntegerType() { return &IntegerTy; }
  Type *getInt64Type() { return &Int64Ty; }
  Type *getCardinalType() { return &CardinalTy; }
  /// Unsigned 64-bit (FPC's qword). Values are kept as their 64 bits (in
  /// int64_t), so code that compares or prints them must read them as
  /// unsigned (see isQWord).
  Type *getQWordType() { return &QWordTy; }
  Type *getByteType() { return &ByteTy; }
  Type *getWordType() { return &WordTy; }
  Type *getShortIntType() { return &ShortIntTy; }
  Type *getSmallIntType() { return &SmallIntTy; }
  Type *getRealType() { return &RealTy; }
  Type *getBooleanType() { return &BooleanTy; }
  Type *getCharType() { return &CharTy; }
  Type *getWideCharType() { return &WideCharTy; }
  Type *getNilType() { return &NilTy; }
  /// The type of untyped parameters (var x; const x): usable only through
  /// typecasts, @, and as arguments for untyped parameters.
  Type *getUntypedType() { return &UntypedTy; }
  Type *getTextType() { return &TextTy; }
  Type *getUntypedPointerType() { return &UntypedPointerTy; }
  /// The type of a set constructor with elements of (host) type Element.
  Type *getSetConstructorType(Type *Element) {
    if (!Element)
      return &EmptySetTy;
    SetType *&T = SetTypes[Element];
    if (!T)
      T = create<SetType>(Element);
    return T;
  }
  /// The type of a string literal with Length (> 1) characters.
  Type *getStringLiteralType(uint64_t Length) {
    StringLiteralType *&T = StringLiteralTypes[Length];
    if (!T)
      T = create<StringLiteralType>(Length);
    return T;
  }

  /// string[MaxLen]; all short string types are compatible (Turbo/FPC).
  Type *getDynArrayType(Type *Element) {
    DynArrayType *&T = DynArrayTypes[Element];
    if (!T)
      T = create<DynArrayType>(Element);
    return T;
  }
  /// async T (Result null: async).
  Type *getTaskType(Type *Result) {
    TaskType *&T = TaskTypes[Result];
    if (!T)
      T = create<TaskType>(Result);
    return T;
  }
  Type *getTokenType() {
    if (!TokenTy)
      TokenTy = create<TokenType>();
    return TokenTy;
  }
  Type *getOpenArrayType(Type *Element) {
    OpenArrayType *&T = OpenArrayTypes[Element];
    if (!T)
      T = create<OpenArrayType>(Element);
    return T;
  }

  Type *getAnsiStringType() {
    if (!AnsiString)
      AnsiString = create<AnsiStringType>();
    return AnsiString;
  }

  Type *getShortStringType(unsigned MaxLen = 255) {
    ShortStringType *&T = ShortStringTypes[MaxLen];
    if (!T)
      T = create<ShortStringType>(MaxLen);
    return T;
  }

  template <typename T, typename... Args> T *create(Args &&...A) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "AST nodes live in an arena and are never destroyed");
    return new (Alloc.Allocate(sizeof(T), alignof(T)))
        T(std::forward<Args>(A)...);
  }

  /// Copies an array into the arena (e.g. from a SmallVector built while
  /// parsing).
  template <typename T> llvm::ArrayRef<T> copyArray(llvm::ArrayRef<T> A) {
    static_assert(std::is_trivially_destructible_v<T>);
    if (A.empty())
      return {};
    T *Mem = static_cast<T *>(Alloc.Allocate(sizeof(T) * A.size(), alignof(T)));
    std::uninitialized_copy(A.begin(), A.end(), Mem);
    return {Mem, A.size()};
  }
};

} // namespace p3

#endif // P3_AST_ASTCONTEXT_H
