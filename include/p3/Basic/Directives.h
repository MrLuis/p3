//===- Directives.h - Compiler directives in comments -------------*- C++ -*-===//
//
// `{$R-}`, `{$Q+}`, `(*$R+,Q-*)` switch runtime checks from that point of the
// source onward (Turbo/FPC convention, also accepted in ISO mode):
//
//   R  range checks     (subranges, array indices, chr/succ/pred, ...)
//   Q  overflow checks  (integer + - * abs sqr, unary minus)
//   Wrap (no directive; unchecked blocks) integer values converted to a
//      narrower integer type wrap instead of being range-checked
//
// The lexer records each switch with its location; code generation asks
// whether a check is enabled at the location of the construct it emits.
// {$pop} records the state its {$push} saved, which may be the default.
// The parser records checked / unchecked blocks (objfpc) the same way: Q
// on or off (and Wrap off or on) at the block's start, the states before
// it at its `end`.
//
//===----------------------------------------------------------------------===//
#ifndef P3_BASIC_DIRECTIVES_H
#define P3_BASIC_DIRECTIVES_H

#include "p3/Basic/SourceLocation.h"
#include "llvm/ADT/SmallVector.h"

namespace p3 {

class CheckDirectives {
public:
  enum Switch { Range, Overflow, Wrap };

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
  /// buffer): that of the last event at or before L (of those at the same
  /// place, the last recorded); Default if none.
  State stateAt(Switch S, SourceLocation L) const {
    State St = Default;
    const char *At = nullptr;
    for (const Event &E : Events)
      if (E.S == S && L.isValid() && E.BufBegin <= L.getPointer() &&
          L.getPointer() <= E.BufEnd && E.Where <= L.getPointer() &&
          (!At || E.Where >= At)) {
        St = E.St;
        At = E.Where;
      }
    return St;
  }

  /// State of switch S at L, starting from Default.
  bool isEnabled(Switch S, SourceLocation L, bool Default) const {
    State St = stateAt(S, L);
    return St == this->Default ? Default : St == On;
  }
  bool empty() const { return Events.empty(); }
};

} // namespace p3

#endif // P3_BASIC_DIRECTIVES_H
