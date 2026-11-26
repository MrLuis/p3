// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===- WindowsManifestStub.cpp - lld inside p3, without libxml2 -----------===//
//
// LLVM's WindowsManifest library, which lld's COFF linker uses to merge
// application manifests, needs libxml2 (and with it, in MSYS2, libiconv). p3
// links its Windows programs without manifests, so when lld is linked into
// p3 (P3_EMBED_LLD) this stands in for that library: merging is reported as
// unavailable, as for an LLVM built without libxml2.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/WindowsManifest/WindowsManifestMerger.h"

using namespace llvm;
using namespace llvm::windows_manifest;

char WindowsManifestError::ID = 0;

WindowsManifestError::WindowsManifestError(const Twine &Msg) : Msg(Msg.str()) {}

void WindowsManifestError::log(raw_ostream &OS) const { OS << Msg; }

class WindowsManifestMerger::WindowsManifestMergerImpl {};

WindowsManifestMerger::WindowsManifestMerger() = default;

WindowsManifestMerger::~WindowsManifestMerger() = default;

bool llvm::windows_manifest::isAvailable() { return false; }

Error WindowsManifestMerger::merge(MemoryBufferRef) {
  return make_error<WindowsManifestError>(
      "p3 does not merge Windows manifests");
}

std::unique_ptr<MemoryBuffer> WindowsManifestMerger::getMergedManifest() {
  return nullptr;
}
