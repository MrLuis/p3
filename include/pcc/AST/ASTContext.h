//===- ASTContext.h - Owns all AST nodes --------------------------*- C++ -*-===//
//
// AST nodes are allocated in a bump-pointer arena and freed all at once when
// the context dies. Nodes must therefore be trivially destructible: they hold
// StringRefs (into the StringPool / source buffers), ArrayRefs (copied into
// the arena) and pointers to other nodes.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_AST_ASTCONTEXT_H
#define PCC_AST_ASTCONTEXT_H

#include "pcc/AST/Type.h"
#include "pcc/Basic/StringPool.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Allocator.h"

#include <memory>
#include <type_traits>

namespace pcc {

class ASTContext {
  llvm::BumpPtrAllocator Alloc;
  StringPool &Strings;

  // Predeclared types (singletons: type identity == pointer identity).
  Type ErrorTy{Type::TK_Error};
  Type IntegerTy{Type::TK_Integer};
  Type BooleanTy{Type::TK_Boolean};
  Type CharTy{Type::TK_Char};
  Type StringTy{Type::TK_String};

public:
  explicit ASTContext(StringPool &Strings) : Strings(Strings) {}
  ASTContext(const ASTContext &) = delete;
  ASTContext &operator=(const ASTContext &) = delete;

  StringPool &getStrings() { return Strings; }

  Type *getErrorType() { return &ErrorTy; }
  Type *getIntegerType() { return &IntegerTy; }
  Type *getBooleanType() { return &BooleanTy; }
  Type *getCharType() { return &CharTy; }
  Type *getStringType() { return &StringTy; }

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

} // namespace pcc

#endif // PCC_AST_ASTCONTEXT_H
