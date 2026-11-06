//===- Backend.h - Target machine, optimization, file emission ----*- C++ -*-===//
#ifndef PCC_CODEGEN_BACKEND_H
#define PCC_CODEGEN_BACKEND_H

#include "llvm/ADT/StringRef.h"

#include <memory>

namespace llvm {
class Module;
class TargetMachine;
} // namespace llvm

namespace pcc {

class DiagnosticsEngine;

/// Registers every LLVM target compiled into libLLVM (call once).
void initializeAllLLVMTargets();

/// Creates a TargetMachine for Triple (already normalized), or reports an
/// error and returns null.
std::unique_ptr<llvm::TargetMachine>
createTargetMachine(llvm::StringRef Triple, unsigned OptLevel,
                    DiagnosticsEngine &Diags);

/// Runs the standard LLVM -O0..-O3 module pipeline.
void optimizeModule(llvm::Module &M, llvm::TargetMachine &TM, unsigned OptLevel);

/// Returns false (after reporting) if the module is malformed -- always an
/// internal compiler bug.
bool verifyModule(llvm::Module &M, DiagnosticsEngine &Diags);

enum class OutputKind { LLVMIR, Assembly, Object };

/// Writes M to Path ("-" = stdout). Returns false on error (reported).
bool emitOutput(llvm::Module &M, llvm::TargetMachine &TM, OutputKind Kind,
                llvm::StringRef Path, DiagnosticsEngine &Diags);

} // namespace pcc

#endif // PCC_CODEGEN_BACKEND_H
