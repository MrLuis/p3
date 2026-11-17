//===- main.cpp - p3 command line entry point ----------------------------===//

#include "p3/Basic/Version.h"
#include "p3/Driver/Driver.h"

#include "llvm/Config/llvm-config.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

using namespace p3;
namespace cl = llvm::cl;

static cl::OptionCategory P3Category("p3 options");

static cl::opt<std::string> InputFile(cl::Positional,
                                      cl::desc("<input.pas>"),
                                      cl::cat(P3Category));

static cl::opt<std::string> OutputFile("o", cl::desc("Write output to <file>"),
                                       cl::value_desc("file"),
                                       cl::cat(P3Category));

static cl::opt<Action> Act(
    cl::desc("Output kind (default: linked executable):"),
    cl::values(
        clEnumValN(Action::EmitTokens, "emit-tokens", "Dump lexer tokens"),
        clEnumValN(Action::EmitAST, "emit-ast", "Dump the parsed AST"),
        clEnumValN(Action::EmitParseTree, "emit-parse-tree",
                   "Dump the parse tree (no semantic analysis; for testing)"),
        clEnumValN(Action::EmitLLVM, "emit-llvm", "Emit LLVM IR (.ll)"),
        clEnumValN(Action::EmitAsm, "S", "Emit target assembly (.s)"),
        clEnumValN(Action::EmitObj, "c", "Emit an object file (.o)")),
    cl::init(Action::EmitExe), cl::cat(P3Category));

static cl::opt<unsigned> OptLevel("O", cl::Prefix, cl::init(0),
                                  cl::desc("Optimization level (0-3)"),
                                  cl::value_desc("level"),
                                  cl::cat(P3Category));

static cl::list<std::string>
    IncludeDirs("I", cl::Prefix, cl::desc("Add a directory to search for units"),
                cl::value_desc("dir"), cl::cat(P3Category));

static cl::list<std::string>
    Defines("d", cl::Prefix,
            cl::desc("Define a symbol for conditional compilation ({$IFDEF})"),
            cl::value_desc("name"), cl::cat(P3Category));

static cl::list<std::string>
    LibraryDirs("L", cl::Prefix,
                cl::desc("Add a directory to search for libraries (linking)"),
                cl::value_desc("dir"), cl::cat(P3Category));

static cl::list<std::string>
    Libraries("l", cl::Prefix, cl::desc("Link with library <name>"),
              cl::value_desc("name"), cl::cat(P3Category));

static cl::opt<bool> SeparateUnits(
    "fseparate-units",
    cl::desc("Link separately compiled units (each compiled with -c to "
             "<unit>.o next to its source)"),
    cl::cat(P3Category));

static cl::opt<bool> DebugInfo("g", cl::desc("Generate debug information"),
                               cl::cat(P3Category));

static cl::opt<std::string>
    TargetTriple("target", cl::desc("Target triple (default: host)"),
                 cl::value_desc("triple"), cl::cat(P3Category));

static cl::opt<LangMode>
    Mode("mode", cl::desc("Language dialect:"), cl::init(LangMode::ISO),
         cl::values(clEnumValN(LangMode::ISO, "iso", "ISO 7185 Pascal"),
                    clEnumValN(LangMode::TP, "tp",
                               "Turbo/Free Pascal extensions"),
                    clEnumValN(LangMode::ObjFPC, "objfpc",
                               "Turbo/Free Pascal extensions and classes")),
         cl::cat(P3Category));

static cl::opt<ColorMode>
    Color("fdiagnostics-color", cl::desc("Colorize diagnostics:"), cl::init(ColorMode::Auto),
          cl::values(clEnumValN(ColorMode::Auto, "auto", "If stderr is a tty"),
                     clEnumValN(ColorMode::Always, "always", "Always"),
                     clEnumValN(ColorMode::Never, "never", "Never")),
          cl::cat(P3Category));

static cl::opt<unsigned>
    ErrorLimit("ferror-limit", cl::init(20),
               cl::desc("Stop after N errors (0 = no limit)"),
               cl::value_desc("N"), cl::cat(P3Category));

static cl::opt<bool> Werror("Werror", cl::desc("Treat warnings as errors"),
                            cl::cat(P3Category));

static cl::opt<bool> NoDeprecated(
    "Wno-deprecated",
    cl::desc("Do not warn about uses of deprecated (experimental, platform, "
             "library, unimplemented) declarations"),
    cl::cat(P3Category));

static cl::opt<bool>
    NoUnusedVariable("Wno-unused-variable",
                     cl::desc("Do not warn about unused variables"),
                     cl::cat(P3Category));

static cl::opt<bool> NoRangeChecks(
    "fno-range-checks",
    cl::desc("Disable runtime range checks (subranges, array indices, ...)"),
    cl::cat(P3Category));

static cl::opt<bool> NoInlineRuntime(
    "fno-inline-runtime",
    cl::desc("Do not inline the runtime's functions (from -O1 they are, "
             "when compiling for the host)"),
    cl::cat(P3Category));

static cl::opt<bool> KeepUnused(
    "fkeep-unused",
    cl::desc("Keep the routines and data that nothing reachable from the "
             "program refers to"),
    cl::cat(P3Category));

static cl::opt<bool> ZeroInitOn(
    "fzero-init",
    cl::desc("Local arrays, records and strings start zeroed (the default "
             "at -O0)"),
    cl::cat(P3Category));

static cl::opt<bool> ZeroInitOff(
    "fno-zero-init",
    cl::desc("Do not zero local arrays, records and strings (the default "
             "from -O1)"),
    cl::cat(P3Category));

static cl::opt<bool>
    NoOverflowChecks("fno-overflow-checks",
                     cl::desc("Disable runtime integer overflow checks"),
                     cl::cat(P3Category));

static cl::opt<std::string> ProjectFile(
    "project",
    cl::desc("Read project settings from <file> (default: the first p3.json "
             "in the input file's directory or its parents)"),
    cl::value_desc("file"), cl::cat(P3Category));

static cl::opt<bool> NoProject("no-project",
                               cl::desc("Do not read a project file (p3.json)"),
                               cl::cat(P3Category));

static void printVersion(llvm::raw_ostream &OS) {
  OS << "p3 version " P3_VERSION_STRING " (LLVM " LLVM_VERSION_STRING ")\n"
     << "  Default target: " << llvm::sys::getDefaultTargetTriple() << '\n'
     << "  Host CPU: " << llvm::sys::getHostCPUName() << '\n';
}

int main(int argc, char **argv) {
  llvm::InitLLVM X(argc, argv);

  cl::HideUnrelatedOptions(P3Category);
  cl::SetVersionPrinter(printVersion);
  cl::ParseCommandLineOptions(argc, argv,
                              "p3 - Pascal compiler targeting LLVM\n");

  CompilerOptions Opts;
  Opts.InputFile = InputFile;
  Opts.OutputFile = OutputFile;
  Opts.Act = Act;
  Opts.Mode = Mode;
  Opts.OptLevel = OptLevel;
  Opts.DebugInfo = DebugInfo;
  Opts.IncludeDirs.assign(IncludeDirs.begin(), IncludeDirs.end());
  Opts.LibraryDirs.assign(LibraryDirs.begin(), LibraryDirs.end());
  Opts.Libraries.assign(Libraries.begin(), Libraries.end());
  Opts.Defines.assign(Defines.begin(), Defines.end());
  Opts.SeparateUnits = SeparateUnits;
  Opts.TargetTriple = TargetTriple;
  Opts.Color = Color;
  Opts.ErrorLimit = ErrorLimit;
  Opts.WarningsAsErrors = Werror;
  Opts.RangeChecks = !NoRangeChecks;
  Opts.InlineRuntime = !NoInlineRuntime;
  Opts.KeepUnused = KeepUnused;
  if (ZeroInitOn || ZeroInitOff) // the last one given wins
    Opts.ZeroInit = !ZeroInitOff || (ZeroInitOn && ZeroInitOn.getPosition() >
                                                       ZeroInitOff.getPosition());
  Opts.WarnUnusedVariables = !NoUnusedVariable;
  Opts.WarnDeprecated = !NoDeprecated;
  Opts.OverflowChecks = !NoOverflowChecks;
  Opts.ProjectFile = ProjectFile;
  Opts.NoProject = NoProject;
  Opts.GivenOutput = OutputFile.getNumOccurrences() > 0;
  Opts.GivenMode = Mode.getNumOccurrences() > 0;
  Opts.GivenOptLevel = OptLevel.getNumOccurrences() > 0;
  Opts.GivenTarget = TargetTriple.getNumOccurrences() > 0;
  Opts.GivenErrorLimit = ErrorLimit.getNumOccurrences() > 0;

  Driver D(std::move(Opts), llvm::errs());
  return D.run();
}
