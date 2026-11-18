#include "p3/Driver/Linker.h"
#include "p3/Basic/Diagnostics.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Triple.h"

using namespace p3;

std::string p3::findRuntimeLibrary() {
  if (auto Env = llvm::sys::Process::GetEnv("P3_RUNTIME_LIB"))
    return *Env;
  // getMainExecutable works without argv[0] on Linux and macOS.
  std::string Exe = llvm::sys::fs::getMainExecutable(nullptr, nullptr);
  llvm::SmallString<256> Path(llvm::sys::path::parent_path(Exe));
  llvm::sys::path::append(Path, "..", "lib", "libpascalrt.a");
  llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
  return std::string(Path);
}

std::string p3::findRuntimeIR() {
  if (auto Env = llvm::sys::Process::GetEnv("P3_RUNTIME_IR"))
    return *Env;
  llvm::SmallString<256> Path(llvm::sys::path::parent_path(findRuntimeLibrary()));
  llvm::sys::path::append(Path, "libpascalrt.ll");
  return std::string(Path);
}

std::string p3::findUnitLibrary() {
  if (auto Env = llvm::sys::Process::GetEnv("P3_UNIT_DIR"))
    return *Env;
  std::string Exe = llvm::sys::fs::getMainExecutable(nullptr, nullptr);
  llvm::SmallString<256> Path(llvm::sys::path::parent_path(Exe));
  llvm::sys::path::append(Path, "..", "lib", "pascal");
  llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
  return std::string(Path);
}

static std::string findLinkerDriver(DiagnosticsEngine &Diags) {
  if (auto Env = llvm::sys::Process::GetEnv("P3_CC")) {
    if (llvm::sys::path::has_parent_path(*Env))
      return *Env;
    if (auto P = llvm::sys::findProgramByName(*Env))
      return *P;
    Diags.error("cannot find linker driver '" + llvm::Twine(*Env) +
                "' (from P3_CC)");
    return "";
  }
  for (const char *Name : {"clang-19", "clang", "cc", "gcc"})
    if (auto P = llvm::sys::findProgramByName(Name))
      return *P;
  Diags.error("cannot find a C compiler to link with (tried clang-19, clang, "
              "cc, gcc); set P3_CC");
  return "";
}

bool p3::linkExecutable(llvm::StringRef ObjectFile,
                         llvm::ArrayRef<std::string> UnitObjects,
                         llvm::StringRef OutputFile,
                         llvm::ArrayRef<std::string> LibraryDirs,
                         llvm::ArrayRef<std::string> Libraries,
                         llvm::StringRef Triple, DiagnosticsEngine &Diags) {
  std::string RuntimeLib = findRuntimeLibrary();
  if (!llvm::sys::fs::exists(RuntimeLib)) {
    Diags.error("cannot find the Pascal runtime library '" +
                llvm::Twine(RuntimeLib) + "'; set P3_RUNTIME_LIB");
    return false;
  }

  std::string CC = findLinkerDriver(Diags);
  if (CC.empty())
    return false;

  // -L directories, then -l libraries (each in command-line order). A
  // library named as a file (":name") is -l:name, a GNU ld extension; on
  // Mach-O resolve the file in the -L directories and pass its path.
  llvm::Triple T(Triple);
  std::vector<std::string> Extra;
  for (const std::string &D : LibraryDirs)
    Extra.push_back("-L" + D);
  for (const std::string &L : Libraries) {
    if (T.isOSBinFormatMachO() && !L.empty() && L[0] == ':') {
      llvm::StringRef File(L.c_str() + 1);
      std::string Found;
      if (!File.contains('/'))
        for (const std::string &D : LibraryDirs) {
          llvm::SmallString<256> Path(D);
          llvm::sys::path::append(Path, File);
          if (llvm::sys::fs::exists(Path)) {
            Found = std::string(Path);
            break;
          }
        }
      Extra.push_back(Found.empty() ? File.str() : Found);
    } else {
      Extra.push_back("-l" + L);
    }
  }
  // On macOS the object records the deployment target from the target
  // triple; clang's default can be older, which makes ld warn that the
  // object was built for a newer macOS. Use the object's target.
  if (T.isMacOSX()) {
    llvm::VersionTuple V;
    if (T.getMacOSXVersion(V))
      Extra.push_back("-mmacosx-version-min=" + V.getAsString());
  }
  llvm::SmallVector<llvm::StringRef, 8> Args = {CC, ObjectFile};
  Args.append(UnitObjects.begin(), UnitObjects.end());
  Args.push_back(RuntimeLib);
  Args.append(Extra.begin(), Extra.end());
  // Dead-code stripping of the unused sections (item 18).
  if (T.isOSBinFormatELF())
    Args.push_back("-Wl,--gc-sections");
  else if (T.isOSBinFormatMachO())
    Args.push_back("-Wl,-dead_strip");
  else if (T.isOSBinFormatCOFF())
    Args.push_back("-Wl,/OPT:REF");
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
