// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
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

/// The runtime library for Triple (empty: the host). For the host:
/// $P3_RUNTIME_LIB, else <dir of p3>/../lib/libpascalrt.a; for another
/// target, <dir of p3>/../lib/<runtimeTargetName>/libpascalrt.a (built when
/// p3 is configured with that target in P3_CROSS_TARGETS).
std::string findRuntimeLibrary(llvm::StringRef Triple = "");

/// Is there a runtime for Triple: findRuntimeLibrary's, or for MinGW its
/// msvcrt or ucrt variant next to it?
bool haveRuntimeLibrary(llvm::StringRef Triple);

/// The name of Triple's runtime directory and C toolchain prefix: MinGW
/// triples as MinGW-w64 spells them (x86_64-w64-mingw32), others as they are.
std::string runtimeTargetName(llvm::StringRef Triple);

/// Is Triple the host's?
bool isHostTriple(llvm::StringRef Triple);

/// The file name of a program built for the host and run at once (a
/// generator, a compile-time computation): Path, with ".exe" on Windows,
/// where the linker would add it and CreateProcess needs it.
std::string hostExecutable(llvm::StringRef Path);

/// The directory of p3's unit library, searched after -I directories:
/// $P3_UNIT_DIR, else <dir of p3>/../lib/pascal
std::string findUnitLibrary();

/// The C compiler-rt's profile runtime for PGO: searched next to LLVM's
/// libraries (<libdir>/clang/*/lib/<host>/libclang_rt.profile*), or
/// $P3_PROFILE_RUNTIME.
std::string findProfileRuntimeLibrary();

/// The runtime as LLVM IR, linked into optimized programs for inlining:
/// $P3_RUNTIME_IR, else pascal/libpascalrt.ll next to the runtime library
/// (lib/pascal: not lib, where Homebrew allows only libraries). It
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
