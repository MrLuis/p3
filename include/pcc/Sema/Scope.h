//===- Scope.h - Lexical scopes / symbol table --------------------*- C++ -*-===//
//
// Pascal scopes nest: the standard scope (predeclared integer, writeln, ...)
// encloses the program block, which encloses each routine. An inner
// declaration hides an outer one with the same name -- even predeclared
// ones, e.g. `var integer: char;` is legal (if unwise).
//
// A `with r do` statement opens a scope whose names are r's fields. In
// objfpc, `with obj do` and every method body open a scope whose names are
// the members of a class and its ancestors.
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
  ClassType *WithClass = nullptr;
  WithContext *With = nullptr;
  /// Interface scopes of used units (Turbo/FPC); later ones take precedence.
  llvm::SmallVector<const Scope *, 2> Imports;

public:
  explicit Scope(Scope *Parent = nullptr) : Parent(Parent) {}
  /// A `with` scope exposing Record's fields.
  Scope(Scope *Parent, RecordType *Record, WithContext *W)
      : Parent(Parent), WithRecord(Record), With(W) {}
  /// A scope exposing the members of class C (a method body, or `with`).
  Scope(Scope *Parent, ClassType *C, WithContext *W)
      : Parent(Parent), WithClass(C), With(W) {}

  Scope *getParent() const { return Parent; }
  void addImport(const Scope *S) { Imports.push_back(S); }

  /// Names declared directly in this scope (not imports).
  Decl *lookupOwn(llvm::StringRef Name) const {
    auto It = Symbols.find(Name);
    return It == Symbols.end() ? nullptr : It->second;
  }

  /// Returns the existing declaration if Name is already declared in *this*
  /// scope (and does not insert), or nullptr on success.
  Decl *insert(llvm::StringRef Name, Decl *D) {
    auto [It, Inserted] = Symbols.try_emplace(Name, D);
    return Inserted ? nullptr : It->second;
  }

  /// Calls Fn(name) for each name declared directly in this scope.
  template <typename FnT> void forEachName(FnT Fn) const {
    if (WithRecord) {
      for (FieldDecl *F : WithRecord->getAllFields())
        Fn(F->getName().Name);
      return;
    }
    if (WithClass) {
      for (const ClassType *C = WithClass; C; C = C->getParent())
        for (Decl *D : C->getMembers())
          Fn(D->getName().Name);
      return;
    }
    for (const auto &Entry : Symbols)
      Fn(Entry.getKey());
    for (const Scope *I : Imports)
      for (const auto &Entry : I->Symbols)
        Fn(Entry.getKey());
  }

  Decl *lookupLocal(llvm::StringRef Name) const {
    if (WithRecord)
      return WithRecord->findField(Name);
    if (WithClass)
      return WithClass->findMember(Name);
    if (Decl *D = lookupOwn(Name))
      return D;
    for (auto It = Imports.rbegin(); It != Imports.rend(); ++It)
      if (Decl *D = (*It)->lookupOwn(Name))
        return D;
    return nullptr;
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
