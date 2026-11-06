//===- SourceLocation.h - Positions in Pascal source --------------*- C++ -*-===//
//
// A SourceLocation is a cheap, copyable handle to a position inside a buffer
// owned by the SourceManager. It wraps llvm::SMLoc (a pointer into the buffer),
// so every token / AST node can carry one for free.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_BASIC_SOURCELOCATION_H
#define PCC_BASIC_SOURCELOCATION_H

#include "llvm/Support/SMLoc.h"

namespace pcc {

/// Identifies a buffer registered with the SourceManager. 0 = invalid.
using FileID = unsigned;

class SourceLocation {
  llvm::SMLoc Loc;

public:
  SourceLocation() = default;
  explicit SourceLocation(llvm::SMLoc L) : Loc(L) {}

  static SourceLocation fromPointer(const char *Ptr) {
    return SourceLocation(llvm::SMLoc::getFromPointer(Ptr));
  }

  bool isValid() const { return Loc.isValid(); }
  const char *getPointer() const { return Loc.getPointer(); }
  llvm::SMLoc getSMLoc() const { return Loc; }

  SourceLocation getLocWithOffset(int Offset) const {
    return fromPointer(getPointer() + Offset);
  }

  friend bool operator==(SourceLocation A, SourceLocation B) {
    return A.Loc == B.Loc;
  }
  friend bool operator!=(SourceLocation A, SourceLocation B) {
    return !(A == B);
  }
};

/// Half-open character range [Begin, End).
class SourceRange {
  SourceLocation Begin, End;

public:
  SourceRange() = default;
  SourceRange(SourceLocation B, SourceLocation E) : Begin(B), End(E) {}
  explicit SourceRange(SourceLocation L) : Begin(L), End(L) {}

  SourceLocation getBegin() const { return Begin; }
  SourceLocation getEnd() const { return End; }
  bool isValid() const { return Begin.isValid() && End.isValid(); }

  llvm::SMRange getSMRange() const {
    return llvm::SMRange(Begin.getSMLoc(), End.getSMLoc());
  }
};

} // namespace pcc

#endif // PCC_BASIC_SOURCELOCATION_H
