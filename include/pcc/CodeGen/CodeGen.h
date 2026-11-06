//===- CodeGen.h - AST -> LLVM IR ---------------------------------*- C++ -*-===//
#ifndef PCC_CODEGEN_CODEGEN_H
#define PCC_CODEGEN_CODEGEN_H

#include "llvm/ADT/StringRef.h"

#include <memory>

namespace llvm {
class LLVMContext;
class Module;
class TargetMachine;
} // namespace llvm

namespace pcc {

class ProgramDecl;

/// Lowers a Sema-checked program to an LLVM module for TM's target.
/// The program body becomes `int main(int argc, char **argv)`.
std::unique_ptr<llvm::Module> generateModule(ProgramDecl &P,
                                             llvm::LLVMContext &Ctx,
                                             llvm::TargetMachine &TM,
                                             llvm::StringRef SourceFileName);

} // namespace pcc

#endif // PCC_CODEGEN_CODEGEN_H
