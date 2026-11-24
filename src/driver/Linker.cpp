#include "p3/Driver/Linker.h"
#include "p3/Basic/Diagnostics.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/TargetParser/Host.h"

using namespace p3;

bool p3::isHostTriple(llvm::StringRef Triple) {
  return Triple.empty() ||
         llvm::Triple(Triple) == llvm::Triple(llvm::sys::getDefaultTargetTriple());
}

std::string p3::runtimeTargetName(llvm::StringRef Triple) {
  llvm::Triple T(Triple);
  if (T.isWindowsGNUEnvironment())
    return (T.getArchName() + "-w64-mingw32").str();
  return Triple.str();
}

std::string p3::findRuntimeLibrary(llvm::StringRef Triple) {
  bool Host = isHostTriple(Triple);
  if (Host)
    if (auto Env = llvm::sys::Process::GetEnv("P3_RUNTIME_LIB"))
      return *Env;
  // getMainExecutable works without argv[0] on Linux and macOS.
  std::string Exe = llvm::sys::fs::getMainExecutable(nullptr, nullptr);
  llvm::SmallString<256> Path(llvm::sys::path::parent_path(Exe));
  if (Host)
    llvm::sys::path::append(Path, "..", "lib", "libpascalrt.a");
  else
    llvm::sys::path::append(Path, "..", "lib", runtimeTargetName(Triple),
                            "libpascalrt.a");
  llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
  return std::string(Path);
}

std::string p3::findProfileRuntimeLibrary() {
  if (auto Env = llvm::sys::Process::GetEnv("P3_PROFILE_RUNTIME"))
    return *Env;
  // Next to LLVM's libraries: <libdir>/clang/<version>/lib/<triple>/
  // libclang_rt.profile<suffix>.a. The libdir is not asked of LLVM at
  // runtime; the known install shapes are searched.
  llvm::Triple T(llvm::sys::getDefaultTargetTriple());
  std::string Arch =
      T.isMacOSX() ? "osx" : T.getArchName().str();
  llvm::SmallVector<std::string, 6> LibDirs;
  if (auto P = llvm::sys::Process::GetEnv("P3_LLVM_LIBDIR"))
    LibDirs.push_back(*P);
  for (const char *Name : {"clang-23", "llvm@23", "llvm", "clang-19", "llvm@19"})
    if (auto P = llvm::sys::findProgramByName(Name)) {
      llvm::StringRef Dir = llvm::sys::path::parent_path(*P);
      LibDirs.push_back((llvm::Twine(Dir) + "/../lib").str());
      break;
    }
  LibDirs.push_back(P3_LLVM_LIBDIR);
  LibDirs.push_back("/usr/lib/llvm-19/lib");
  for (const std::string &D : LibDirs) {
    std::error_code EC;
    llvm::SmallString<256> Clang(D);
    llvm::sys::path::append(Clang, "clang");
    for (llvm::sys::fs::directory_iterator Ver(Clang, EC), VerEnd;
         !EC && Ver != VerEnd; Ver.increment(EC)) {
      llvm::SmallString<256> Lib(D);
      llvm::sys::path::append(Lib, "clang", llvm::sys::path::filename(Ver->path()),
                              "lib");
      llvm::SmallString<256> OsDir(Lib);
      llvm::sys::path::append(OsDir, T.isMacOSX() ? "darwin" : T.getArchName().str() + "-unknown-" +
                                        (T.isOSLinux() ? (T.isMusl() ? "musl" : "gnu") : T.getOSName().str()));
      std::error_code FEC;
      for (llvm::sys::fs::directory_iterator F(OsDir, FEC), FEnd;
           !FEC && F != FEnd; F.increment(FEC)) {
        llvm::StringRef Name(llvm::sys::path::filename(F->path()));
        if (Name.starts_with("libclang_rt.profile") &&
            Name.ends_with(".a"))
          return std::string(F->path());
      }
    }
  }
  return "";
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

static std::string findLinkerDriver(llvm::StringRef Triple,
                                    DiagnosticsEngine &Diags) {
  // Another target: its own C toolchain (MinGW-w64's for Windows), which
  // knows where the target's C library and system libraries are.
  if (!isHostTriple(Triple)) {
    std::string Name = runtimeTargetName(Triple);
    if (auto Env = llvm::sys::Process::GetEnv("P3_CROSS_CC"))
      return *Env;
    for (const char *Suffix : {"-gcc", "-clang"})
      if (auto P = llvm::sys::findProgramByName(Name + Suffix))
        return *P;
    Diags.error("cannot find a C compiler to link for '" + llvm::Twine(Triple) +
                "' (tried " + Name + "-gcc, " + Name + "-clang); set P3_CROSS_CC");
    return "";
  }
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
  std::string RuntimeLib = findRuntimeLibrary(Triple);
  if (!llvm::sys::fs::exists(RuntimeLib)) {
    Diags.error("cannot find the Pascal runtime library '" +
                llvm::Twine(RuntimeLib) + "'; set P3_RUNTIME_LIB");
    return false;
  }

  std::string CC = findLinkerDriver(Triple, Diags);
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
    // `external 'c'`: on Windows the C library (msvcrt or UCRT) is always
    // linked, and there is no libc to name.
    if (T.isOSWindows() && L == "c")
      continue;
    if (L.find('/') != std::string::npos) {
      // A path (the profile runtime, or ':<file>' spellings): it is
      // passed as it is, without a -L search - a bare file name without
      // a slash is searched in the -L directories first.
      llvm::StringRef File = L[0] == ':' ? llvm::StringRef(L.c_str() + 1) : llvm::StringRef(L);
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
    } else if (T.isOSBinFormatMachO() && !L.empty() && L[0] == ':') {
      llvm::StringRef File(L.c_str() + 1);
      std::string Found;
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
  if (T.isOSBinFormatELF() || T.isWindowsGNUEnvironment())
    Args.push_back("-Wl,--gc-sections"); // GNU ld, MinGW's too
  else if (T.isOSBinFormatMachO())
    Args.push_back("-Wl,-dead_strip");
  else if (T.isOSBinFormatCOFF())
    Args.push_back("-Wl,/OPT:REF");
  // MinGW: a self-contained executable, without MinGW's own DLLs (the
  // runtime's thread-local variables need libwinpthread's).
  if (T.isWindowsGNUEnvironment())
    Args.push_back("-static");
  // Windows: the runtime's socket waits (and unit netasync) use Winsock.
  if (T.isOSWindows())
    Args.push_back("-lws2_32");
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
