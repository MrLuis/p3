//===- Driver.h - Runs the compilation pipeline -------------------*- C++ -*-===//
#ifndef PCC_DRIVER_DRIVER_H
#define PCC_DRIVER_DRIVER_H

#include "pcc/Basic/Diagnostics.h"
#include "pcc/Basic/LangOptions.h"
#include "pcc/Basic/SourceManager.h"
#include "pcc/Basic/StringPool.h"
#include "pcc/Driver/CompilerOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <memory>
#include <string>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace llvm {
class Module;
class TargetMachine;
} // namespace llvm

namespace pcc {

class Lexer;
class Parser;

class ASTContext;
class CheckDirectives;
struct Identifier;
class UnitDecl;
class Block;

class Driver {
  CompilerOptions Opts;
  LangOptions LangOpts;
  SourceManager SM;
  DiagnosticsEngine Diags;
  StringPool Strings;

  bool validateOptions();
  bool validateTarget();

  /// lex -> parse -> sema -> codegen -> emit/link, stopping at Opts.Act.
  bool runPipeline(FileID FID);

  /// Self: the unit being compiled, if any (using it is circular). Units
  /// are loaded once: early for declared(NAME) in $IF, then again for Sema
  /// (all of those loaded, dependencies first, in Ordered).
  void loadUnits(llvm::ArrayRef<Identifier> Uses, ASTContext &Ctx,
                 CheckDirectives &Directives,
                 llvm::SmallVectorImpl<UnitDecl *> &Ordered,
                 llvm::StringRef Self = "");
  void visitUnit(const Identifier &U, ASTContext &Ctx,
                 CheckDirectives &Directives);
  llvm::StringMap<UnitDecl *> UnitsDone;
  llvm::StringSet<> UnitsInProgress;
  llvm::DenseSet<const char *> FailedUses; ///< by the location of the name
  std::vector<std::unique_ptr<Lexer>> UnitLexers;
  std::vector<UnitDecl *> UnitsOrdered;

  /// declared(NAME): the predeclared names, and the lexer and parser of a
  /// file set up for it (the used units' names come from loading them).
  llvm::StringSet<> PredeclaredNames;
  void prepareLexer(Lexer &L, CheckDirectives &Directives);
  void prepareParser(Parser &P, ASTContext &Ctx, CheckDirectives &Directives);

  /// The libraries named by the external routines of the program and its
  /// units (`external 'z'`), linked after the -l ones: -lz, or -l:name for
  /// a file name ('libz.so.1').
  std::vector<std::string> ExternalLibraries;
  void collectExternalLibraries(Block *B);

  /// Verifies, optimizes and writes M (or links it with UnitObjects).
  bool finishModule(llvm::Module &M, llvm::TargetMachine &TM,
                    llvm::ArrayRef<std::string> UnitObjects);
  /// The pipeline for a unit file: its own module (S11).
  bool compileUnit(Parser &P, FileID FID, ASTContext &Ctx,
                   CheckDirectives &Directives);

  /// --emit-tokens
  void dumpTokens(FileID FID, llvm::raw_ostream &OS);

public:
  Driver(CompilerOptions Opts, llvm::raw_ostream &DiagOS);
  ~Driver();
  /// Applies Opts' language mode and diagnostic settings.
  void configure();

  /// Runs the whole pipeline. Returns the process exit code.
  int run();

  SourceManager &getSourceManager() { return SM; }
  DiagnosticsEngine &getDiagnostics() { return Diags; }
  const CompilerOptions &getOptions() const { return Opts; }
};

} // namespace pcc

#endif // PCC_DRIVER_DRIVER_H
