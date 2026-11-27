// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- Lexer.h - Pascal tokenizer ---------------------------------*- C++ -*-===//
//
// Turns one source buffer into a stream of Tokens. Errors are reported through
// the DiagnosticsEngine and the lexer always recovers, so callers simply keep
// calling lex() until tok::eof.
//
// Implemented (M1): whitespace, { } and (* *) comments, identifiers and all
// keywords (case-insensitive), integer and real literals, '...' strings with
// '' escapes, every operator/punctuator incl. (. .) and @.
//
// Compiler directives {$R±} / {$Q±} are recorded (M11). Turbo/FPC lexical
// extensions arrive with M12. Conditional compilation ({$DEFINE}, {$UNDEF},
// {$IFDEF}, {$IFNDEF}, {$IF}, {$ELSEIF}, {$ELSE}, {$ENDIF}, {$IFEND}) is
// done here: the text of a branch that is not compiled is skipped, and only
// its comments, strings and conditional directives are recognized.
//
//===----------------------------------------------------------------------===//
#ifndef P3_LEXER_LEXER_H
#define P3_LEXER_LEXER_H

#include "p3/Basic/Directives.h"
#include "p3/Basic/LangOptions.h"
#include "p3/Basic/SourceLocation.h"
#include "p3/Lexer/Token.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"

#include <functional>
#include <string>
#include <vector>

namespace p3 {

class DiagnosticsEngine;
class SourceManager;
class StringPool;

class Lexer {
  DiagnosticsEngine &Diags;
  StringPool &Strings;
  LangOptions LangOpts;

  const char *BufBegin;
  const char *BufEnd;
  const char *Cur;
  CheckDirectives *Directives = nullptr;
  bool CorbaInterfaces = false; ///< {$interfaces corba} (default: com)
  bool AdvancedRecords = false; ///< {$modeswitch advancedrecords}
  bool TypeHelpers = false;     ///< {$modeswitch typehelpers}
  bool RaceChecks = false;      ///< {$modeswitch racechecks} (p3's own)
  bool NullSafe = false;        ///< {$nullsafe on} (p3's own, objfpc)
  bool ScopedEnums = false;     ///< {$scopedenums on}
  /// {$packrecords N} / {$align N} / {$aN}: the largest alignment of the
  /// fields of the records declared after it (1, 2, 4, 8, 16 or 32); 0: the
  /// natural (C) layout.
  unsigned PackRecords = 0;
  /// {$push}: the local switches saved, innermost last ({$pop} restores
  /// them), as in Free Pascal (mode switches and {$macro} are not local).
  struct SavedSwitches {
    bool LongStrings, CorbaInterfaces, InlineOn, ScopedEnums, NullSafe;
    unsigned PackRecords;
    CheckDirectives::State Range, Overflow, TypedAddress, PointerMath;
  };
  llvm::SmallVector<SavedSwitches, 4> SwitchStack;
  bool LongStrings = false;     ///< {$H+}: `string` is an ansistring

  // Macros ({$macro on}, {$define NAME := value}, as in Free Pascal): the
  // values are ranges of the source (in the define directives), keyed by
  // the upper-case name; a use of NAME lexes its value (MacroStack: the
  // places to continue at, innermost last). Expanded tokens have the
  // value's locations.
  bool MacrosOn = false;
  bool InlineOn; ///< {$inline on} (the default in objfpc mode)
  /// {$coperators on}: += -= *= /= are tokens (the default except in ISO
  /// mode, as in Free Pascal, where it is on in every mode).
  bool COperators;
  /// Mode switches that p3 lets a program turn on or off in any mode
  /// ({$modeswitch result|out|defaultparameters}); on in objfpc mode.
  bool ResultSwitch, OutSwitch, DefaultParamsSwitch;
  void handleModeSwitch(const char *Start, llvm::StringRef Arg);
  struct MacroValue {
    const char *Begin, *End;
  };
  llvm::StringMap<MacroValue> Macros;
  struct MacroFrame {
    const char *BufEnd, *Cur;
  };
  llvm::SmallVector<MacroFrame, 4> MacroStack;
  bool expandMacro(const Token &T, const char *Start);

  // {$I file} (Turbo/FPC): the including buffers, innermost last.
  struct IncludeFrame {
    const char *BufBegin, *BufEnd, *Cur;
  };
  llvm::SmallVector<IncludeFrame, 4> IncludeStack;
  SourceManager *IncludeSM = nullptr; ///< null: includes are not supported
  std::vector<std::string> IncludeDirs;
  std::string PendingInclude;          ///< file named by the last {$I ...}
  SourceLocation PendingIncludeLoc;
  void enterPendingInclude();

  void handleDirective(const char *Start, llvm::StringRef Text);

  // Conditional compilation. Symbols are kept in upper case; a file starts
  // with the predefined and command-line ones, and its include files share
  // them.
  llvm::StringSet<> Defines;
  struct Conditional {
    SourceLocation Loc; ///< the opening {$IF...}
    const char *Name;   ///< its directive, e.g. "$IFDEF"
    bool ParentActive;  ///< the text around the conditional is compiled
    bool Active;        ///< the current branch is compiled
    bool Taken;         ///< some branch has been compiled
    bool SeenElse;
    unsigned Depth;     ///< include depth of the opening directive
  };
  llvm::SmallVector<Conditional, 8> Conditionals;
  // declared(NAME) in $IF (Free Pascal): the names declared so far, which
  // the parser adds (innermost scope last: a routine's own), the predeclared
  // ones, and the used units' (BeforeDeclared loads them when first needed).
  llvm::SmallVector<llvm::StringSet<>, 4> NameScopes;
  const llvm::StringSet<> *Predeclared = nullptr;
  std::function<void()> BeforeDeclared;
  bool isActive() const {
    return Conditionals.empty() || Conditionals.back().Active;
  }
  /// Handles Word if it is a conditional-compilation directive.
  bool handleConditional(const char *Start, llvm::StringRef Word,
                         llvm::StringRef Arg);
  bool evalCondition(const char *Start, llvm::StringRef Expr);
  /// Skips the text of a branch that is not compiled.
  void skipInactive();
  /// At the end of a buffer: reports the conditionals it left open.
  void closeConditionals();

  void skipWhitespaceAndComments();
  void skipBlockComment(unsigned OpenLen, llvm::StringRef Close);

  void lexIdentifier(Token &T);
  void lexNumber(Token &T);
  void lexString(Token &T);
  void lexBasedNumber(Token &T, const char *Start, unsigned Radix);
  void lexTemplate(Token &T, const char *Start, bool Raw, bool Opening);
  /// Template strings whose interpolation `${ ... }` is being lexed: a '}'
  /// continues the innermost one.
  unsigned TemplateDepth = 0;

  void formToken(Token &T, tok::TokenKind K, const char *Start);
  SourceLocation locOf(const char *P) const {
    return SourceLocation::fromPointer(P);
  }

public:
  Lexer(const SourceManager &SM, FileID FID, DiagnosticsEngine &Diags,
        StringPool &Strings, const LangOptions &LangOpts);

  /// Lexes the next token. Returns tok::eof (repeatedly) at the end.
  void lex(Token &T);

  const LangOptions &getLangOpts() const { return LangOpts; }

  /// Removes what a template string's layout adds to its text. Chunks: the
  /// text between its interpolations, in order (line ends are '\n'). A line
  /// break right after the opening backtick goes, and so does a last line
  /// of only blanks, with its line break. Then the lines lose the
  /// indentation they all share (blanks and tabs count alike; that last
  /// line's counts too, lines of only blanks do not and are emptied). Text
  /// on the opening backtick's line is left as it is.
  static void dedentTemplate(llvm::MutableArrayRef<std::string> Chunks);

  /// Are interfaces declared here CORBA style ({$interfaces corba})?
  bool corbaInterfaces() const { return CorbaInterfaces; }
  /// May records declared here have methods ({$modeswitch advancedrecords})?
  bool advancedRecords() const { return AdvancedRecords; }
  /// May type helpers be declared here ({$modeswitch typehelpers})?
  bool typeHelpers() const { return TypeHelpers; }
  bool raceChecks() const { return RaceChecks; }
  /// Are the routines that start here null-safe ({$nullsafe on})?
  bool nullSafe() const { return NullSafe; }
  /// Are the values of enumerations declared here reached only through
  /// their type ({$scopedenums on})?
  bool scopedEnums() const { return ScopedEnums; }
  unsigned packRecords() const { return PackRecords; }
  bool longStrings() const { return LongStrings; }
  /// Does the `inline` directive inline ({$inline on})?
  bool inlineOn() const { return InlineOn; }
  /// {$modeswitch result}: `Result` names a function's result.
  bool resultSwitch() const { return ResultSwitch; }
  /// {$modeswitch out}: `out` parameters.
  bool outSwitch() const { return OutSwitch; }
  /// {$modeswitch defaultparameters}: parameters' default values.
  bool defaultParamsSwitch() const { return DefaultParamsSwitch; }

  /// Defines symbols for conditional compilation (-d NAME).
  void setDefines(llvm::ArrayRef<std::string> Names);

  /// declared(NAME): names (lower case) declared so far, in the current
  /// scope or (Global) the outermost one; a routine's in a scope of its own.
  void declareName(llvm::StringRef Name, bool Global = false) {
    (Global ? NameScopes.front() : NameScopes.back()).insert(Name);
  }
  void pushNameScope() { NameScopes.emplace_back(); }
  void popNameScope() {
    if (NameScopes.size() > 1)
      NameScopes.pop_back();
  }
  void setPredeclared(const llvm::StringSet<> *Names) { Predeclared = Names; }
  void setBeforeDeclared(std::function<void()> F) { BeforeDeclared = std::move(F); }
  bool isDeclared(llvm::StringRef Name);

  /// Where to record {$R±} / {$Q±} switches (null: ignore directives).
  void setDirectives(CheckDirectives *D) { Directives = D; }
  /// A switch state the parser sets at L, in the buffer being lexed
  /// (checked / unchecked blocks), and the state at L.
  void addCheckState(SourceLocation L, CheckDirectives::Switch S,
                     CheckDirectives::State St) {
    if (Directives && L.isValid() && BufBegin <= L.getPointer() &&
        L.getPointer() <= BufEnd)
      Directives->addState(L, BufBegin, BufEnd, S, St);
  }
  CheckDirectives::State checkStateAt(CheckDirectives::Switch S,
                                      SourceLocation L) const {
    return Directives ? Directives->stateAt(S, L) : CheckDirectives::Default;
  }

  /// Enables {$I file}: files are searched next to the including file,
  /// then in Dirs, and loaded into SM.
  void setIncludeSupport(SourceManager &SM, llvm::ArrayRef<std::string> Dirs) {
    IncludeSM = &SM;
    IncludeDirs.assign(Dirs.begin(), Dirs.end());
  }
};

} // namespace p3

#endif // P3_LEXER_LEXER_H
