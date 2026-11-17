//===- SourceManager.h - Owns source buffers ----------------------*- C++ -*-===//
//
// Owns every source buffer the compiler reads and maps SourceLocations back to
// file / line / column. Built on llvm::SourceMgr, which caches line offsets.
//
//===----------------------------------------------------------------------===//
#ifndef P3_BASIC_SOURCEMANAGER_H
#define P3_BASIC_SOURCEMANAGER_H

#include "p3/Basic/SourceLocation.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/SourceMgr.h"

#include <utility>

namespace p3 {

struct PresumedLoc {
  llvm::StringRef Filename;
  unsigned Line = 0;   ///< 1-based
  unsigned Column = 0; ///< 1-based, in bytes (a tab counts as one column)
  bool isValid() const { return Line != 0; }
};

class SourceManager {
  llvm::SourceMgr SM;

public:
  /// Load a file from disk ("-" reads stdin).
  llvm::ErrorOr<FileID> loadFile(llvm::StringRef Path);

  /// Register an in-memory buffer (used by tests and later by `{$I file}`).
  FileID addBuffer(llvm::StringRef Contents, llvm::StringRef Name);

  unsigned getNumFiles() const { return SM.getNumBuffers(); }

  llvm::StringRef getBufferData(FileID FID) const;
  llvm::StringRef getFilename(FileID FID) const;
  SourceLocation getStartOfFile(FileID FID) const;

  /// Returns 0 if the location is not inside any registered buffer.
  FileID getFileID(SourceLocation Loc) const;

  PresumedLoc getPresumedLoc(SourceLocation Loc) const;

  /// Text of the source line containing Loc, without the line terminator.
  llvm::StringRef getLineText(SourceLocation Loc) const;

  llvm::SourceMgr &getLLVMSourceMgr() { return SM; }
  const llvm::SourceMgr &getLLVMSourceMgr() const { return SM; }
};

} // namespace p3

#endif // P3_BASIC_SOURCEMANAGER_H
