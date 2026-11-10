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
#include <vector>

namespace pcc {

/// What the compiler should produce, in pipeline order.
enum class Action {
  EmitTokens, ///< --emit-tokens : dump lexer output
  EmitAST,    ///< --emit-ast    : dump the parsed AST
  EmitParseTree, ///< --emit-parse-tree : the AST without Sema, with units
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
  std::vector<std::string> IncludeDirs; ///< -I: where to look for units
  std::vector<std::string> LibraryDirs; ///< -L: passed to the linker
  std::vector<std::string> Libraries;   ///< -l: passed to the linker
  std::vector<std::string> Defines;     ///< -d: conditional symbols
  /// -fseparate-units: the program's units were compiled separately (with
  /// -c, to <unit>.o next to their sources); link those objects.
  bool SeparateUnits = false;
  ColorMode Color = ColorMode::Auto;
  unsigned ErrorLimit = 20; ///< 0 = unlimited
  bool WarningsAsErrors = false;
  bool WarnUnusedVariables = true; ///< -Wno-unused-variable disables
  bool WarnDeprecated = true;      ///< -Wno-deprecated disables
  bool RangeChecks = true;    ///< -fno-range-checks disables
  bool OverflowChecks = true; ///< -fno-overflow-checks disables

  /// The project file (pcc.json, see Project.h): --project=<file> (empty:
  /// look for one), --no-project.
  std::string ProjectFile;
  bool NoProject = false;
  /// Options with a value given on the command line, which override the
  /// project file's settings (flags only add to them).
  bool GivenOutput = false, GivenMode = false, GivenOptLevel = false,
       GivenTarget = false, GivenErrorLimit = false;
};

/// Default output path for an action, e.g. "dir/hello.pas" + EmitObj ->
/// "hello.o". Dumps (tokens/AST) go to stdout ("-").
std::string getDefaultOutputFile(const CompilerOptions &Opts);

const char *getActionName(Action A);

} // namespace pcc

#endif // PCC_DRIVER_COMPILEROPTIONS_H
