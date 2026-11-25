// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- Backend.h - Target machine, optimization, file emission ----*- C++ -*-===//
#ifndef P3_CODEGEN_BACKEND_H
#define P3_CODEGEN_BACKEND_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>

namespace llvm {
class Module;
class TargetMachine;
} // namespace llvm

namespace p3 {

class DiagnosticsEngine;

/// Registers every LLVM target compiled into libLLVM (call once).
void initializeAllLLVMTargets();

/// Creates a TargetMachine for Triple (already normalized), or reports an
/// error and returns null.
std::unique_ptr<llvm::TargetMachine>
createTargetMachine(llvm::StringRef Triple, unsigned OptLevel,
                    llvm::StringRef CPU, llvm::StringRef Features,
                    DiagnosticsEngine &Diags);

/// Links the runtime's LLVM IR (libpascalrt.ll, at Path) into M, so that
/// the optimizer can inline the runtime's functions; they stay
/// available_externally, and calls that are not inlined go to the library.
/// Without the file M stays as it is; a file that cannot be read is
/// reported as a warning.
void linkRuntimeIR(llvm::Module &M, llvm::StringRef Path,
                   llvm::StringRef CPU, llvm::StringRef Features,
                   DiagnosticsEngine &Diags);

/// Links the IR that each separately compiled unit left next to its object
/// (Path.o -> Path.ll): its definitions stay available_externally, so the
/// optimizer can inline inline routines while the objects still provide the
/// code. Missing files are ignored; a file that cannot be read is a warning.
void linkUnitIR(llvm::Module &M, llvm::ArrayRef<std::string> ObjectPaths,
                DiagnosticsEngine &Diags);

/// Writes M as textual IR to Path (for the .ll next to a unit's object).
bool writeModuleIR(llvm::Module &M, llvm::StringRef Path,
                   DiagnosticsEngine &Diags);

/// Runs the standard LLVM -O0..-O3 module pipeline, or -Os/-Oz when
/// SizeLevel is 1 or 2. With ProfileGen (a -fprofile-instr-generate
/// build), the profile counters go in before the inlining; with
/// ProfileUse, the profile at the path drives the pipeline.
void optimizeModule(llvm::Module &M, llvm::TargetMachine &TM, unsigned OptLevel,
                    unsigned SizeLevel = 0,
                    const std::string &ProfileGen = std::string(),
                    const std::string &ProfileUse = std::string());

/// Returns false (after reporting) if the module is malformed -- always an
/// internal compiler bug.
bool verifyModule(llvm::Module &M, DiagnosticsEngine &Diags);

enum class OutputKind { LLVMIR, Assembly, Object };

/// Writes M to Path ("-" = stdout). Returns false on error (reported).
bool emitOutput(llvm::Module &M, llvm::TargetMachine &TM, OutputKind Kind,
                llvm::StringRef Path, DiagnosticsEngine &Diags);

} // namespace p3

#endif // P3_CODEGEN_BACKEND_H
