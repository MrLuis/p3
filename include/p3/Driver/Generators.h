//===- Generators.h - Source generators ---------------------------*- C++ -*-===//
//
// Source generators (modernizing-pascal.md item 19). A generator is a program
// (`generator Name for Attr;`, unit p3gen) that reads a text description of
// the types marked [Attr] and writes Pascal source for them. The compiler
// builds and runs it before compiling the project: see Driver::runGenerators.
//
// The model is syntactic (no Sema): names, the text of each type as written,
// attributes with literal arguments, locations. Its format is the contract
// with unit p3gen, and is the same in the self-hosted compiler.
//
//===----------------------------------------------------------------------===//
#ifndef P3_DRIVER_GENERATORS_H
#define P3_DRIVER_GENERATORS_H

#include "p3/Basic/SourceManager.h"

#include "llvm/ADT/StringRef.h"

#include <string>

namespace p3 {

class Decl;

/// The model of the record and class types declared in Root (a program's
/// block, or a unit's interface) that are marked [Attr] (AttrLower: lower
/// case). Empty (no "type" line) if there are none; NumTypes is how many.
std::string buildModel(const SourceManager &SM, Decl *Root,
                       llvm::StringRef AttrLower, unsigned &NumTypes);

/// P made absolute, without . and .. (the key of Driver's splices).
std::string absolutePath(llvm::StringRef P);

/// Escapes \, tab and newline in a model or output field.
std::string escapeField(llvm::StringRef S);
std::string unescapeField(llvm::StringRef S);

} // namespace p3

#endif // P3_DRIVER_GENERATORS_H
