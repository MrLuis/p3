//===- Sema.h - Semantic analysis ---------------------------------*- C++ -*-===//
//
// Resolves names, computes and checks types, and annotates the AST
// (Expr::setType, NameRefExpr::setDecl, ...). Code generation may assume a
// program accepted by Sema is valid.
//
// M4 scope: predeclared types integer/boolean/char, constants
// true/false/maxint, the procedures write/writeln/read/readln, the functions
// abs/sqr/odd/ord/chr/succ/pred, program-level variables, assignment, and
// the integer/boolean/char operators.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_SEMA_SEMA_H
#define PCC_SEMA_SEMA_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <functional>

namespace pcc {

class ASTContext;
class DiagnosticsEngine;
class ProgramDecl;
class RoutineDecl;
class UnitDecl;
struct LangOptions;

/// Largest value of the predeclared type `integer` (we lower it to i32).
constexpr int64_t PascalMaxInt = 2147483647;

class Sema {
public:
  /// Parses a generic routine's declaration again, for a specialization.
  using ReparseFn = std::function<RoutineDecl *(const RoutineDecl *Generic)>;

private:
  ASTContext &Ctx;
  DiagnosticsEngine &Diags;
  const LangOptions &LangOpts;
  ReparseFn Reparse;

public:
  Sema(ASTContext &Ctx, DiagnosticsEngine &Diags, const LangOptions &LangOpts,
       ReparseFn Reparse = nullptr)
      : Ctx(Ctx), Diags(Diags), LangOpts(LangOpts), Reparse(std::move(Reparse)) {}

  /// Returns true if the program is valid. Units must be in dependency
  /// order (a unit before the units and program that use it).
  /// SeparateUnits: the units were compiled separately; only their
  /// interfaces are checked.
  bool check(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units = {},
             bool SeparateUnits = false);
  /// Checks unit U, compiled on its own; Deps (dependency order) are the
  /// units it uses, directly or not, of which only the interfaces matter.
  bool checkUnit(UnitDecl &U, llvm::ArrayRef<UnitDecl *> Deps);
};

} // namespace pcc

#endif // PCC_SEMA_SEMA_H
