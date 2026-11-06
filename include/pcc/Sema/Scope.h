//===- Scope.h - Lexical scopes / symbol table --------------------*- C++ -*-===//
//
// Pascal scopes nest: the standard scope (predeclared integer, writeln, ...)
// encloses the program block, which encloses each routine (M6). An inner
// declaration hides an outer one with the same name -- even predeclared ones,
// e.g. `var integer: char;` is legal (if unwise).
//
//===----------------------------------------------------------------------===//
#ifndef PCC_SEMA_SCOPE_H
#define PCC_SEMA_SCOPE_H

#include "llvm/ADT/StringMap.h"

namespace pcc {

class Decl;

class Scope {
  Scope *Parent;
  llvm::StringMap<Decl *> Symbols; ///< keyed by canonical (lowercase) name

public:
  explicit Scope(Scope *Parent = nullptr) : Parent(Parent) {}

  Scope *getParent() const { return Parent; }

  /// Returns the existing declaration if Name is already declared in *this*
  /// scope (and does not insert), or nullptr on success.
  Decl *insert(llvm::StringRef Name, Decl *D) {
    auto [It, Inserted] = Symbols.try_emplace(Name, D);
    return Inserted ? nullptr : It->second;
  }

  Decl *lookupLocal(llvm::StringRef Name) const {
    auto It = Symbols.find(Name);
    return It == Symbols.end() ? nullptr : It->second;
  }

  /// Innermost declaration of Name visible from this scope.
  Decl *lookup(llvm::StringRef Name) const {
    for (const Scope *S = this; S; S = S->Parent)
      if (Decl *D = S->lookupLocal(Name))
        return D;
    return nullptr;
  }
};

} // namespace pcc

#endif // PCC_SEMA_SCOPE_H
