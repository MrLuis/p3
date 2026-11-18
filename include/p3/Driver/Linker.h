//===- Linker.h - Link object files into an executable ------------*- C++ -*-===//
//
// Delegates to a C compiler driver (clang or cc), which knows where the
// platform's crt files and libc live:
//
//   <cc> prog.o <libdir>/libpascalrt.a -lm -o prog
//
// Overrides (environment):
//   P3_CC           C compiler driver to use as the linker
//   P3_RUNTIME_LIB  path to libpascalrt.a
//
//===----------------------------------------------------------------------===//
#ifndef P3_DRIVER_LINKER_H
#define P3_DRIVER_LINKER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace p3 {

class DiagnosticsEngine;

/// Default location: <dir of p3>/../lib/libpascalrt.a
std::string findRuntimeLibrary();

/// The directory of p3's unit library, searched after -I directories:
/// $P3_UNIT_DIR, else <dir of p3>/../lib/pascal
std::string findUnitLibrary();

/// The runtime as LLVM IR, linked into optimized programs for inlining:
/// $P3_RUNTIME_IR, else libpascalrt.ll next to the runtime library. It
/// need not exist.
std::string findRuntimeIR();

/// Returns false (after reporting) on failure.
bool linkExecutable(llvm::StringRef ObjectFile,
                    llvm::ArrayRef<std::string> UnitObjects,
                    llvm::StringRef OutputFile,
                    llvm::ArrayRef<std::string> LibraryDirs,
                    llvm::ArrayRef<std::string> Libraries,
                    llvm::StringRef Triple, DiagnosticsEngine &Diags);

} // namespace p3

#endif // P3_DRIVER_LINKER_H
