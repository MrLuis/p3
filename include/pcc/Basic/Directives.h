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
// {$pop} records the state its {$push} saved, which may be the default.
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
public:
  /// A switch's state: off, on, or the default (the command line's).
  enum State : signed char { Off = 0, On = 1, Default = -1 };

private:
  struct Event {
    const char *Where;            ///< pointer into the source buffer
    const char *BufBegin, *BufEnd; ///< the buffer (events apply per file)
    Switch S;
    State St;
  };
  llvm::SmallVector<Event, 4> Events; ///< in source order

public:
  void add(SourceLocation L, const char *BufBegin, const char *BufEnd,
           Switch S, bool IsOn) {
    addState(L, BufBegin, BufEnd, S, IsOn ? On : Off);
  }
  void addState(SourceLocation L, const char *BufBegin, const char *BufEnd,
                Switch S, State St) {
    Events.push_back({L.getPointer(), BufBegin, BufEnd, S, St});
  }

  /// The state of switch S at L as the switches so far set it (in L's
  /// buffer): Default if none did.
  State stateAt(Switch S, SourceLocation L) const {
    State St = Default;
    for (const Event &E : Events)
      if (E.S == S && L.isValid() && E.BufBegin <= L.getPointer() &&
          L.getPointer() <= E.BufEnd && E.Where <= L.getPointer())
        St = E.St;
    return St;
  }

  /// State of switch S at L, starting from Default.
  bool isEnabled(Switch S, SourceLocation L, bool Default) const {
    State St = stateAt(S, L);
    return St == this->Default ? Default : St == On;
  }
  bool empty() const { return Events.empty(); }
};

} // namespace pcc

#endif // PCC_BASIC_DIRECTIVES_H
