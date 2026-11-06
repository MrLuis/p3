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
class SourceManager;
struct LangOptions;

/// Lowers a Sema-checked program to an LLVM module for TM's target.
/// The program body becomes `int main(int argc, char **argv)`; program
/// variables become internal globals. SM is used to put source locations
/// into runtime error messages.
std::unique_ptr<llvm::Module>
generateModule(ProgramDecl &P, const SourceManager &SM,
               const LangOptions &LangOpts, llvm::LLVMContext &Ctx,
               llvm::TargetMachine &TM, llvm::StringRef SourceFileName);

} // namespace pcc

#endif // PCC_CODEGEN_CODEGEN_H
