#include "p3/Basic/SourceManager.h"

#include "llvm/Support/MemoryBuffer.h"

using namespace p3;

llvm::ErrorOr<FileID> SourceManager::loadFile(llvm::StringRef Path) {
  auto BufOrErr = llvm::MemoryBuffer::getFileOrSTDIN(Path, /*IsText=*/true);
  if (!BufOrErr)
    return BufOrErr.getError();
  return SM.AddNewSourceBuffer(std::move(*BufOrErr), llvm::SMLoc());
}

FileID SourceManager::addBuffer(llvm::StringRef Contents, llvm::StringRef Name) {
  return SM.AddNewSourceBuffer(
      llvm::MemoryBuffer::getMemBufferCopy(Contents, Name), llvm::SMLoc());
}

llvm::StringRef SourceManager::getBufferData(FileID FID) const {
  return SM.getMemoryBuffer(FID)->getBuffer();
}

llvm::StringRef SourceManager::getFilename(FileID FID) const {
  return SM.getMemoryBuffer(FID)->getBufferIdentifier();
}

SourceLocation SourceManager::getStartOfFile(FileID FID) const {
  return SourceLocation::fromPointer(getBufferData(FID).data());
}

FileID SourceManager::getFileID(SourceLocation Loc) const {
  if (!Loc.isValid())
    return 0;
  return SM.FindBufferContainingLoc(Loc.getSMLoc());
}

PresumedLoc SourceManager::getPresumedLoc(SourceLocation Loc) const {
  FileID FID = getFileID(Loc);
  if (!FID)
    return {};
  auto [Line, Col] = SM.getLineAndColumn(Loc.getSMLoc(), FID);
  return {getFilename(FID), Line, Col};
}

llvm::StringRef SourceManager::getLineText(SourceLocation Loc) const {
  FileID FID = getFileID(Loc);
  if (!FID)
    return {};
  llvm::StringRef Buf = getBufferData(FID);
  size_t Off = Loc.getPointer() - Buf.data();
  // A location pointing at a line terminator belongs to the line it ends.
  size_t NL = Buf.take_front(Off).find_last_of("\r\n");
  size_t Start = NL == llvm::StringRef::npos ? 0 : NL + 1;
  size_t End = Buf.find_first_of("\r\n", Off);
  if (End == llvm::StringRef::npos)
    End = Buf.size();
  return Buf.slice(Start, End);
}
