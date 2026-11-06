//===- Linker.h - Link object files into an executable ------------*- C++ -*-===//
//
// Delegates to a C compiler driver (clang or cc), which knows where the
// platform's crt files and libc live:
//
//   <cc> prog.o <libdir>/libpascalrt.a -lm -o prog
//
// Overrides (environment):
//   PCC_CC           C compiler driver to use as the linker
//   PCC_RUNTIME_LIB  path to libpascalrt.a
//
//===----------------------------------------------------------------------===//
#ifndef PCC_DRIVER_LINKER_H
#define PCC_DRIVER_LINKER_H

#include "llvm/ADT/StringRef.h"

#include <string>

namespace pcc {

class DiagnosticsEngine;

/// Default location: <dir of pcc>/../lib/libpascalrt.a
std::string findRuntimeLibrary();

/// Returns false (after reporting) on failure.
bool linkExecutable(llvm::StringRef ObjectFile, llvm::StringRef OutputFile,
                    DiagnosticsEngine &Diags);

} // namespace pcc

#endif // PCC_DRIVER_LINKER_H
