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

#include <cstdint>

namespace pcc {

class ASTContext;
class DiagnosticsEngine;
class ProgramDecl;
struct LangOptions;

/// Largest value of the predeclared type `integer` (we lower it to i32).
constexpr int64_t PascalMaxInt = 2147483647;

class Sema {
  ASTContext &Ctx;
  DiagnosticsEngine &Diags;
  const LangOptions &LangOpts;

public:
  Sema(ASTContext &Ctx, DiagnosticsEngine &Diags, const LangOptions &LangOpts)
      : Ctx(Ctx), Diags(Diags), LangOpts(LangOpts) {}

  /// Returns true if the program is valid.
  bool check(ProgramDecl &P);
};

} // namespace pcc

#endif // PCC_SEMA_SEMA_H
