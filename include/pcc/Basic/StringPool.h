//===- StringPool.h - Interned, stable strings --------------------*- C++ -*-===//
//
// Identifier names and literal values live here for the whole compilation, so
// tokens and AST nodes can hold plain StringRefs. Interning also makes
// identifier comparison a pointer comparison.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_BASIC_STRINGPOOL_H
#define PCC_BASIC_STRINGPOOL_H

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Allocator.h"

namespace pcc {

class StringPool {
  llvm::StringSet<llvm::BumpPtrAllocator> Set;

public:
  StringPool() = default;
  StringPool(const StringPool &) = delete;
  StringPool &operator=(const StringPool &) = delete;

  /// Returns a StringRef with the same contents whose storage lives as long
  /// as the pool. Equal strings yield identical data() pointers.
  llvm::StringRef intern(llvm::StringRef S) {
    return Set.insert(S).first->getKey();
  }
};

} // namespace pcc

#endif // PCC_BASIC_STRINGPOOL_H
