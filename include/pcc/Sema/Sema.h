//===- Sema.h - Semantic analysis ---------------------------------*- C++ -*-===//
//
// Checks a parsed program before code generation. Codegen may assume that a
// program accepted by Sema is valid.
//
// M3 scope: the only callable procedures are the predeclared write/writeln,
// integer literals must fit in `integer` (maxint), and program parameters must
// be the standard files input/output.
// M4 replaces this with real scopes, symbol tables and type checking.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_SEMA_SEMA_H
#define PCC_SEMA_SEMA_H

#include <cstdint>

namespace pcc {

class DiagnosticsEngine;
class ProgramDecl;

/// Largest value of the predeclared type `integer` (we lower it to i32).
constexpr int64_t PascalMaxInt = 2147483647;

class Sema {
  DiagnosticsEngine &Diags;

public:
  explicit Sema(DiagnosticsEngine &Diags) : Diags(Diags) {}

  /// Returns true if the program is valid.
  bool check(ProgramDecl &P);
};

} // namespace pcc

#endif // PCC_SEMA_SEMA_H
