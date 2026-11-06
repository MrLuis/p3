//===- ASTPrinter.h - --emit-ast ----------------------------------*- C++ -*-===//
#ifndef PCC_AST_ASTPRINTER_H
#define PCC_AST_ASTPRINTER_H

#include "llvm/ADT/ArrayRef.h"

namespace llvm {
class raw_ostream;
}

namespace pcc {

class ProgramDecl;
class SourceManager;
class UnitDecl;

/// Prints an indented tree, one node per line with its <line:col>.
void printAST(ProgramDecl &P, const SourceManager &SM, llvm::raw_ostream &OS);

/// --emit-parse-tree: the parser's output without semantic analysis, with
/// everything the plain dump leaves to Sema (type specs, typed-constant
/// values, uses clauses, units). The self-hosted parser is checked against
/// it (SELFHOST.md S5).
void printParseTree(ProgramDecl &P, llvm::ArrayRef<UnitDecl *> Units,
                    const SourceManager &SM, llvm::raw_ostream &OS);

} // namespace pcc

#endif // PCC_AST_ASTPRINTER_H
