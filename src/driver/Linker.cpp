#include "pcc/Driver/Linker.h"
#include "pcc/Basic/Diagnostics.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"

using namespace pcc;

std::string pcc::findRuntimeLibrary() {
  if (auto Env = llvm::sys::Process::GetEnv("PCC_RUNTIME_LIB"))
    return *Env;
  // getMainExecutable works without argv[0] on Linux and macOS.
  std::string Exe = llvm::sys::fs::getMainExecutable(nullptr, nullptr);
  llvm::SmallString<256> Path(llvm::sys::path::parent_path(Exe));
  llvm::sys::path::append(Path, "..", "lib", "libpascalrt.a");
  llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
  return std::string(Path);
}

std::string pcc::findRuntimeIR() {
  if (auto Env = llvm::sys::Process::GetEnv("PCC_RUNTIME_IR"))
    return *Env;
  llvm::SmallString<256> Path(llvm::sys::path::parent_path(findRuntimeLibrary()));
  llvm::sys::path::append(Path, "libpascalrt.ll");
  return std::string(Path);
}

std::string pcc::findUnitLibrary() {
  if (auto Env = llvm::sys::Process::GetEnv("PCC_UNIT_DIR"))
    return *Env;
  std::string Exe = llvm::sys::fs::getMainExecutable(nullptr, nullptr);
  llvm::SmallString<256> Path(llvm::sys::path::parent_path(Exe));
  llvm::sys::path::append(Path, "..", "lib", "pascal");
  llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
  return std::string(Path);
}

static std::string findLinkerDriver(DiagnosticsEngine &Diags) {
  if (auto Env = llvm::sys::Process::GetEnv("PCC_CC")) {
    if (llvm::sys::path::has_parent_path(*Env))
      return *Env;
    if (auto P = llvm::sys::findProgramByName(*Env))
      return *P;
    Diags.error("cannot find linker driver '" + llvm::Twine(*Env) +
                "' (from PCC_CC)");
    return "";
  }
  for (const char *Name : {"clang-19", "clang", "cc", "gcc"})
    if (auto P = llvm::sys::findProgramByName(Name))
      return *P;
  Diags.error("cannot find a C compiler to link with (tried clang-19, clang, "
              "cc, gcc); set PCC_CC");
  return "";
}

bool pcc::linkExecutable(llvm::StringRef ObjectFile,
                         llvm::ArrayRef<std::string> UnitObjects,
                         llvm::StringRef OutputFile,
                         llvm::ArrayRef<std::string> LibraryDirs,
                         llvm::ArrayRef<std::string> Libraries,
                         DiagnosticsEngine &Diags) {
  std::string RuntimeLib = findRuntimeLibrary();
  if (!llvm::sys::fs::exists(RuntimeLib)) {
    Diags.error("cannot find the Pascal runtime library '" +
                llvm::Twine(RuntimeLib) + "'; set PCC_RUNTIME_LIB");
    return false;
  }

  std::string CC = findLinkerDriver(Diags);
  if (CC.empty())
    return false;

  // -L directories, then -l libraries (each in command-line order).
  std::vector<std::string> Extra;
  for (const std::string &D : LibraryDirs)
    Extra.push_back("-L" + D);
  for (const std::string &L : Libraries)
    Extra.push_back("-l" + L);
  llvm::SmallVector<llvm::StringRef, 8> Args = {CC, ObjectFile};
  Args.append(UnitObjects.begin(), UnitObjects.end());
  Args.push_back(RuntimeLib);
  Args.append(Extra.begin(), Extra.end());
  Args.append({"-lm", "-o", OutputFile});

  std::string ErrMsg;
  int RC = llvm::sys::ExecuteAndWait(CC, Args, /*Env=*/std::nullopt,
                                     /*Redirects=*/{}, /*SecondsToWait=*/0,
                                     /*MemoryLimit=*/0, &ErrMsg);
  if (RC < 0) {
    Diags.error("failed to run linker '" + llvm::Twine(CC) + "': " + ErrMsg);
    return false;
  }
  if (RC != 0) {
    Diags.error("linker command failed with exit code " + llvm::Twine(RC));
    return false;
  }
  return true;
}
