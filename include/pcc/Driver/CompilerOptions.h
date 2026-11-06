//===- CompilerOptions.h - Settings for one compiler run ----------*- C++ -*-===//
//
// Plain data describing what the user asked for. main.cpp fills it from the
// command line; tests and future tools can fill it directly.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_DRIVER_COMPILEROPTIONS_H
#define PCC_DRIVER_COMPILEROPTIONS_H

#include "pcc/Basic/LangOptions.h"

#include <string>

namespace pcc {

/// What the compiler should produce, in pipeline order.
enum class Action {
  EmitTokens, ///< --emit-tokens : dump lexer output
  EmitAST,    ///< --emit-ast    : dump the parsed AST
  EmitLLVM,   ///< --emit-llvm   : textual LLVM IR (.ll)
  EmitAsm,    ///< -S            : target assembly (.s)
  EmitObj,    ///< -c            : object file (.o)
  EmitExe,    ///< (default)     : linked executable
};

enum class ColorMode { Auto, Always, Never };

struct CompilerOptions {
  std::string InputFile;
  std::string OutputFile;   ///< empty = derive from input / action
  Action Act = Action::EmitExe;
  LangMode Mode = LangMode::ISO;
  unsigned OptLevel = 0;    ///< 0..3
  bool DebugInfo = false;   ///< -g
  std::string TargetTriple; ///< empty = host
  ColorMode Color = ColorMode::Auto;
  unsigned ErrorLimit = 20; ///< 0 = unlimited
  bool WarningsAsErrors = false;
  bool RangeChecks = true;    ///< -fno-range-checks disables
  bool OverflowChecks = true; ///< -fno-overflow-checks disables
};

/// Default output path for an action, e.g. "dir/hello.pas" + EmitObj ->
/// "hello.o". Dumps (tokens/AST) go to stdout ("-").
std::string getDefaultOutputFile(const CompilerOptions &Opts);

const char *getActionName(Action A);

} // namespace pcc

#endif // PCC_DRIVER_COMPILEROPTIONS_H
