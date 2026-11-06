//===- Directives.h - Compiler directives in comments -------------*- C++ -*-===//
//
// `{$R-}`, `{$Q+}`, `(*$R+,Q-*)` switch runtime checks from that point of the
// source onward (Turbo/FPC convention, also accepted in ISO mode):
//
//   R  range checks     (subranges, array indices, chr/succ/pred, ...)
//   Q  overflow checks  (integer + - * abs sqr, unary minus)
//
// The lexer records each switch with its location; code generation asks
// whether a check is enabled at the location of the construct it emits.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_BASIC_DIRECTIVES_H
#define PCC_BASIC_DIRECTIVES_H

#include "pcc/Basic/SourceLocation.h"
#include "llvm/ADT/SmallVector.h"

namespace pcc {

class CheckDirectives {
public:
  enum Switch { Range, Overflow };

private:
  struct Event {
    const char *Where;            ///< pointer into the source buffer
    const char *BufBegin, *BufEnd; ///< the buffer (events apply per file)
    Switch S;
    bool On;
  };
  llvm::SmallVector<Event, 4> Events; ///< in source order

public:
  void add(SourceLocation L, const char *BufBegin, const char *BufEnd,
           Switch S, bool On) {
    Events.push_back({L.getPointer(), BufBegin, BufEnd, S, On});
  }

  /// State of switch S at L, starting from Default.
  bool isEnabled(Switch S, SourceLocation L, bool Default) const {
    bool On = Default;
    for (const Event &E : Events)
      if (E.S == S && L.isValid() && E.BufBegin <= L.getPointer() &&
          L.getPointer() <= E.BufEnd && E.Where <= L.getPointer())
        On = E.On;
    return On;
  }
  bool empty() const { return Events.empty(); }
};

} // namespace pcc

#endif // PCC_BASIC_DIRECTIVES_H
