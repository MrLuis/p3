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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace pcc {

class DiagnosticsEngine;

/// Default location: <dir of pcc>/../lib/libpascalrt.a
std::string findRuntimeLibrary();

/// The directory of pcc's unit library, searched after -I directories:
/// $PCC_UNIT_DIR, else <dir of pcc>/../lib/pascal
std::string findUnitLibrary();

/// Returns false (after reporting) on failure.
bool linkExecutable(llvm::StringRef ObjectFile,
                    llvm::ArrayRef<std::string> UnitObjects,
                    llvm::StringRef OutputFile,
                    llvm::ArrayRef<std::string> LibraryDirs,
                    llvm::ArrayRef<std::string> Libraries,
                    DiagnosticsEngine &Diags);

} // namespace pcc

#endif // PCC_DRIVER_LINKER_H
