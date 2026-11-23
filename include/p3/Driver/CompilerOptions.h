//===- CompilerOptions.h - Settings for one compiler run ----------*- C++ -*-===//
//
// Plain data describing what the user asked for. main.cpp fills it from the
// command line; tests and future tools can fill it directly.
//
//===----------------------------------------------------------------------===//
#ifndef P3_DRIVER_COMPILEROPTIONS_H
#define P3_DRIVER_COMPILEROPTIONS_H

#include "p3/Basic/LangOptions.h"

#include <optional>
#include <string>
#include <vector>

namespace p3 {

/// What the compiler should produce, in pipeline order.
enum class Action {
  EmitTokens, ///< --emit-tokens : dump lexer output
  EmitAST,    ///< --emit-ast    : dump the parsed AST
  EmitParseTree, ///< --emit-parse-tree : the AST without Sema, with units
  EmitModel,  ///< --emit-model=ATTR : the generator model of the types marked [ATTR]
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
  /// -Os (1) / -Oz (2): optimize for size rather than speed.
  unsigned SizeLevel = 0;
  bool DebugInfo = false;   ///< -g
  std::string TargetTriple; ///< empty = host
  /// -mcpu / -mattr (and the project file's "cpu"): the target CPU and
  /// features passed to LLVM; "native" at -mcpu means the host CPU.
  std::string TargetCPU = "generic", TargetFeatures;
  bool GivenCPU = false;
  std::vector<std::string> IncludeDirs; ///< -I: where to look for units
  std::vector<std::string> LibraryDirs; ///< -L: passed to the linker
  std::vector<std::string> Libraries;   ///< -l: passed to the linker
  std::vector<std::string> Defines;     ///< -d: conditional symbols
  /// --generator=FILE: source generators (programs starting `generator Name
  /// for Attr;`), built and run before the compilation, which then includes
  /// the code they write into --generated-dir (default: gen next to the input).
  std::vector<std::string> Generators;
  std::string GeneratedDir;
  unsigned GeneratorTimeout = 30; ///< seconds one generator may run
  bool GivenGeneratorTimeout = false;
  std::string ModelAttr;          ///< --emit-model's attribute
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
  /// -ffast-math: set the fast-math flags on floating-point instructions
  /// (contraction, reassociation, no NaN/Inf assumptions, ...).
  bool FastMath = false;
  /// Local arrays, records and strings start zeroed: -fzero-init /
  /// -fno-zero-init; without either, at -O0 only.
  std::optional<bool> ZeroInit;

  /// -fkeep-unused: keep the routines, constants and variables that
  /// nothing reachable from the program refers to (by default they are not
  /// compiled to machine code, at any optimization level).
  bool KeepUnused = false;

  /// From -O1, the runtime's small functions are inlined (the runtime's
  /// LLVM IR, libpascalrt.ll, is linked into the module before it is
  /// optimized); -fno-inline-runtime disables that.
  bool InlineRuntime = true;

  /// -fprofile-instr-generate[=<file>]: build with the counters and link
  /// the profile runtime; the program writes <file> (or the default
  /// default.profraw, overridable with LLVM_PROFILE_FILE) on exit.
  std::string ProfileInstrGenerate;
  /// -fprofile-instr-use=<file>: optimize with the profile.
  std::string ProfileInstrUse;
  /// The project file (p3.json, see Project.h): --project=<file> (empty:
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

} // namespace p3

#endif // P3_DRIVER_COMPILEROPTIONS_H
