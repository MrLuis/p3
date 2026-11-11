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
  /// The nested constants and types of a class (and its ancestors): in its
  /// declaration, and in the methods of the classes and records it declares.
  ClassType *NestedOf = nullptr;
  WithContext *With = nullptr;
  /// Interface scopes of used units (Turbo/FPC); later ones take precedence.
  llvm::SmallVector<const Scope *, 2> Imports;
  /// The outermost scope of a routine: a name found beyond it belongs to
  /// an enclosing routine (or to the program).
  bool RoutineBoundary = false;

  /// D was found in an enclosing routine's scope S: a variable of that
  /// routine is captured, and so is the Self of the method whose members
  /// S exposes.
  static void noteCaptured(const Scope *S, Decl *D) {
    if (auto *V = llvm::dyn_cast<VarDecl>(D); V && V->getOwner())
      for (; V; V = V->getAbsoluteRoot()) // an overlay: what it overlays
        V->setCaptured();
    if (S->With)
      if (auto *N = llvm::dyn_cast<NameRefExpr>(S->With->Record))
        if (auto *V = llvm::dyn_cast_or_null<VarDecl>(N->getDecl());
            V && V->getOwner())
          V->setCaptured();
  }

public:
  explicit Scope(Scope *Parent = nullptr) : Parent(Parent) {}
  /// A `with` scope exposing Record's fields.
  Scope(Scope *Parent, RecordType *Record, WithContext *W)
      : Parent(Parent), WithRecord(Record), With(W) {}
  /// A scope exposing the members of class C (a method body, or `with`); for
  /// an advanced record's static method (W null) its static members.
  Scope(Scope *Parent, ClassType *C, WithContext *W)
      : Parent(Parent), WithClass(C), With(W) {}

  /// A scope exposing the nested constants and types of class C.
  static Scope nestedIn(Scope *Parent, ClassType *C) {
    Scope S(Parent);
    S.NestedOf = C;
    return S;
  }
  /// What a class declares for the classes it contains: constants, types
  /// and class variables.
  static bool isNestedDecl(const Decl *D) {
    if (auto *V = llvm::dyn_cast_or_null<VarDecl>(D))
      return V->isClassVar();
    return D && (llvm::isa<TypeDecl>(D) || llvm::isa<ConstDecl>(D));
  }

  Scope *getParent() const { return Parent; }
  void setRoutineBoundary() { RoutineBoundary = true; }
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

  /// Declares (or redeclares) Name as D in this scope.
  void set(llvm::StringRef Name, Decl *D) { Symbols[Name] = D; }

  /// Calls Fn(name) for each name declared directly in this scope.
  template <typename FnT> void forEachName(FnT Fn) const {
    if (WithRecord) {
      for (FieldDecl *F : WithRecord->getAllFields())
        Fn(F->getName().Name);
      return;
    }
    if (WithClass || NestedOf) {
      for (const ClassType *C = WithClass ? WithClass : NestedOf; C;
           C = C->getParent())
        for (Decl *D : C->getMembers())
          if ((WithClass && With) || isNestedDecl(D) ||
              (WithClass && llvm::isa<RoutineDecl>(D) &&
               llvm::cast<RoutineDecl>(D)->isStatic()))
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
    if (NestedOf) {
      Decl *D = NestedOf->findMember(Name);
      return isNestedDecl(D) ? D : nullptr;
    }
    if (WithClass && !With) {
      // A record's static method: only its class methods and operators
      // (static), constants and types are usable without a record.
      Decl *D = WithClass->findMember(Name);
      auto *R = llvm::dyn_cast_or_null<RoutineDecl>(D);
      return (R && R->isStatic()) || isNestedDecl(D) ? D : nullptr;
    }
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
  /// found through a `with`, *W receives that with's context. A variable
  /// found in an enclosing routine is marked as captured (whatever the
  /// caller then does with it: every use of such a variable starts here).
  Decl *lookup(llvm::StringRef Name, WithContext **W = nullptr) const {
    bool Outside = false;
    for (const Scope *S = this; S; S = S->Parent) {
      if (Decl *D = S->lookupLocal(Name)) {
        if (W)
          *W = S->With;
        if (Outside)
          noteCaptured(S, D);
        return D;
      }
      Outside |= S->RoutineBoundary;
    }
    return nullptr;
  }
};

} // namespace pcc

#endif // PCC_SEMA_SCOPE_H
