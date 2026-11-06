//===- Scope.h - Lexical scopes / symbol table --------------------*- C++ -*-===//
//
// Pascal scopes nest: the standard scope (predeclared integer, writeln, ...)
// encloses the program block, which encloses each routine. An inner
// declaration hides an outer one with the same name -- even predeclared
// ones, e.g. `var integer: char;` is legal (if unwise).
//
// A `with r do` statement opens a scope whose names are r's fields.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_SEMA_SCOPE_H
#define PCC_SEMA_SCOPE_H

#include "pcc/AST/AST.h"
#include "pcc/AST/Type.h"
#include "llvm/ADT/StringMap.h"

namespace pcc {

class Scope {
  Scope *Parent;
  llvm::StringMap<Decl *> Symbols; ///< keyed by canonical (lowercase) name
  RecordType *WithRecord = nullptr;
  WithContext *With = nullptr;

public:
  explicit Scope(Scope *Parent = nullptr) : Parent(Parent) {}
  /// A `with` scope exposing Record's fields.
  Scope(Scope *Parent, RecordType *Record, WithContext *W)
      : Parent(Parent), WithRecord(Record), With(W) {}

  Scope *getParent() const { return Parent; }

  /// Returns the existing declaration if Name is already declared in *this*
  /// scope (and does not insert), or nullptr on success.
  Decl *insert(llvm::StringRef Name, Decl *D) {
    auto [It, Inserted] = Symbols.try_emplace(Name, D);
    return Inserted ? nullptr : It->second;
  }

  Decl *lookupLocal(llvm::StringRef Name) const {
    if (WithRecord)
      return WithRecord->findField(Name);
    auto It = Symbols.find(Name);
    return It == Symbols.end() ? nullptr : It->second;
  }

  /// Innermost declaration of Name visible from this scope. If it is a field
  /// found through a `with`, *W receives that with's context.
  Decl *lookup(llvm::StringRef Name, WithContext **W = nullptr) const {
    for (const Scope *S = this; S; S = S->Parent)
      if (Decl *D = S->lookupLocal(Name)) {
        if (W)
          *W = S->With;
        return D;
      }
    return nullptr;
  }
};

} // namespace pcc

#endif // PCC_SEMA_SCOPE_H
