//===- ASTPrinter.h - --emit-ast ----------------------------------*- C++ -*-===//
#ifndef PCC_AST_ASTPRINTER_H
#define PCC_AST_ASTPRINTER_H

namespace llvm {
class raw_ostream;
}

namespace pcc {

class ProgramDecl;
class SourceManager;

/// Prints an indented tree, one node per line with its <line:col>.
void printAST(ProgramDecl &P, const SourceManager &SM, llvm::raw_ostream &OS);

} // namespace pcc

#endif // PCC_AST_ASTPRINTER_H
