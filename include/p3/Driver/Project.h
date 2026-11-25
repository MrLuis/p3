// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- Project.h - p3.json project settings ----------------------*- C++ -*-===//
//
// A project file, p3.json, holds settings for compiling the programs and
// units of a project: what the command line options would say (mode, unit
// and library paths, defines, libraries, optimization, debug info, target,
// warnings, checks) and the project's main program and its output. p3
// looks for it in the input file's directory (the current directory
// without an input file) and its parents; --project=<file> names one and
// --no-project ignores it. Options given on the command line override its
// settings; its lists are added after theirs. Paths in it are relative to
// its directory. The self-hosted compiler reads it the same way
// (selfhost/pcproject.pas).
//
//===----------------------------------------------------------------------===//
#ifndef P3_DRIVER_PROJECT_H
#define P3_DRIVER_PROJECT_H

#include "p3/Driver/CompilerOptions.h"

namespace p3 {

class SourceManager;
class DiagnosticsEngine;

/// Finds and reads the project file for Opts (see above) and applies its
/// settings to Opts. Without an input file, Opts.InputFile becomes the
/// project's main program. Returns false after reporting an error.
bool applyProjectFile(CompilerOptions &Opts, SourceManager &SM,
                      DiagnosticsEngine &Diags);

} // namespace p3

#endif // P3_DRIVER_PROJECT_H
