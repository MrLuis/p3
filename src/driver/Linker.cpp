// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "p3/Driver/Linker.h"
#include "p3/Basic/Diagnostics.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/TargetParser/Host.h"

#ifdef P3_EMBED_LLD
#include "lld/Common/Driver.h"
LLD_HAS_DRIVER(mingw)
#endif

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

std::string p3::selfExecutable() {
  // getMainExecutable works without argv[0] on Linux and macOS, where it
  // resolves links too; on Windows it is GetModuleFileNameW's path.
  std::string Exe = llvm::sys::fs::getMainExecutable(nullptr, nullptr);
  llvm::SmallString<256> Real;
  if (!Exe.empty() && !llvm::sys::fs::real_path(Exe, Real))
    return std::string(Real);
  return Exe;
}

std::string p3::findRuntimeLibrary(llvm::StringRef Triple) {
  bool Host = isHostTriple(Triple);
  if (Host)
    if (auto Env = llvm::sys::Process::GetEnv("P3_RUNTIME_LIB"))
      return *Env;
  std::string Exe = selfExecutable();
  llvm::SmallString<256> Path(llvm::sys::path::parent_path(Exe));
  if (Host)
    llvm::sys::path::append(Path, "..", "lib", "libpascalrt.a");
  else
    llvm::sys::path::append(Path, "..", "lib", runtimeTargetName(Triple),
                            "libpascalrt.a");
  llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
  return std::string(Path);
}

std::string p3::hostExecutable(llvm::StringRef Path) {
#ifdef _WIN32
  if (!Path.ends_with_insensitive(".exe"))
    return (Path + ".exe").str();
#endif
  return Path.str();
}

bool p3::haveRuntimeLibrary(llvm::StringRef Triple) {
  std::string Flat = findRuntimeLibrary(Triple);
  if (llvm::sys::fs::exists(Flat))
    return true;
  if (isHostTriple(Triple) || !llvm::Triple(Triple).isWindowsGNUEnvironment())
    return false;
  for (const char *CRT : {"msvcrt", "ucrt"}) {
    llvm::SmallString<256> P(llvm::sys::path::parent_path(Flat));
    llvm::sys::path::append(P, CRT, "libpascalrt.a");
    if (llvm::sys::fs::exists(P))
      return true;
  }
  return false;
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
  for (const char *Name : {"clang-23", "llvm@23", "llvm"})
    if (auto P = llvm::sys::findProgramByName(Name)) {
      llvm::StringRef Dir = llvm::sys::path::parent_path(*P);
      LibDirs.push_back((llvm::Twine(Dir) + "/../lib").str());
      break;
    }
  LibDirs.push_back(P3_LLVM_LIBDIR);
  LibDirs.push_back("/usr/lib/llvm-23/lib");
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
  llvm::sys::path::append(Path, "pascal", "libpascalrt.ll");
  return std::string(Path);
}

std::string p3::findUnitLibrary() {
  if (auto Env = llvm::sys::Process::GetEnv("P3_UNIT_DIR"))
    return *Env;
  std::string Exe = selfExecutable();
  llvm::SmallString<256> Path(llvm::sys::path::parent_path(Exe));
  llvm::sys::path::append(Path, "..", "lib", "pascal");
  llvm::sys::path::remove_dots(Path, /*remove_dot_dot=*/true);
  return std::string(Path);
}

/// The linker driver an environment variable names: a path as it is, a bare
/// name looked up in $PATH.
static std::string linkerDriverFromEnv(llvm::StringRef Value,
                                       llvm::StringRef Variable,
                                       DiagnosticsEngine &Diags) {
  if (llvm::sys::path::has_parent_path(Value))
    return Value.str();
  if (auto P = llvm::sys::findProgramByName(Value))
    return *P;
  Diags.error("cannot find linker driver '" + llvm::Twine(Value) +
              "' (from " + Variable + ")");
  return "";
}

static std::string findLinkerDriver(llvm::StringRef Triple,
                                    DiagnosticsEngine &Diags) {
  // Another target: its own C toolchain (MinGW-w64's for Windows), which
  // knows where the target's C library and system libraries are.
  if (!isHostTriple(Triple)) {
    std::string Name = runtimeTargetName(Triple);
    if (auto Env = llvm::sys::Process::GetEnv("P3_CROSS_CC"))
      return linkerDriverFromEnv(*Env, "P3_CROSS_CC", Diags);
    for (const char *Suffix : {"-gcc", "-clang"})
      if (auto P = llvm::sys::findProgramByName(Name + Suffix))
        return *P;
    Diags.error("cannot find a C compiler to link for '" + llvm::Twine(Triple) +
                "' (tried " + Name + "-gcc, " + Name + "-clang); set P3_CROSS_CC");
    return "";
  }
  if (auto Env = llvm::sys::Process::GetEnv("P3_CC"))
    return linkerDriverFromEnv(*Env, "P3_CC", Diags);
  for (const char *Name : {"clang-23", "clang", "cc", "gcc"})
    if (auto P = llvm::sys::findProgramByName(Name))
      return *P;
  Diags.error("cannot find a C compiler to link with (tried clang-23, clang, "
              "cc, gcc); set P3_CC");
  return "";
}

/// The C library MinGW-w64's compiler CC links by default, as its headers
/// say (_UCRT): "ucrt" or "msvcrt"; "" if CC cannot tell.
static std::string mingwCRT(llvm::StringRef CC) {
  llvm::SmallString<128> In, Out;
  if (llvm::sys::fs::createTemporaryFile("p3-crt", "c", In))
    return "";
  if (llvm::sys::fs::createTemporaryFile("p3-crt", "i", Out)) {
    llvm::sys::fs::remove(In);
    return "";
  }
  std::string Result;
  {
    std::error_code EC;
    llvm::raw_fd_ostream F(In, EC);
    F << "#include <_mingw.h>\n#ifdef _UCRT\nP3_CRT_UCRT\n#else\n"
         "P3_CRT_MSVCRT\n#endif\n";
  }
  llvm::StringRef Args[] = {CC, "-E", "-P", In};
  std::optional<llvm::StringRef> Redirects[] = {std::nullopt, llvm::StringRef(Out),
                                                llvm::StringRef("")};
  if (llvm::sys::ExecuteAndWait(CC, Args, std::nullopt, Redirects) == 0)
    if (auto Buf = llvm::MemoryBuffer::getFile(Out)) {
      llvm::StringRef Text = (*Buf)->getBuffer();
      if (Text.contains("P3_CRT_UCRT"))
        Result = "ucrt";
      else if (Text.contains("P3_CRT_MSVCRT"))
        Result = "msvcrt";
    }
  llvm::sys::fs::remove(In);
  llvm::sys::fs::remove(Out);
  return Result;
}

/// The runtime for a MinGW program: of the two in <Flat's dir>/msvcrt and
/// /ucrt, the one for the C library CC links ($P3_WINDOWS_CRT names it);
/// Flat itself when there are no such variants. "" (with an error) when the
/// C library cannot be told.
static std::string mingwRuntimeLibrary(llvm::StringRef Flat, llvm::StringRef CC,
                                       DiagnosticsEngine &Diags) {
  llvm::SmallString<256> Dir(llvm::sys::path::parent_path(Flat));
  auto Variant = [&](llvm::StringRef CRT) {
    llvm::SmallString<256> P(Dir);
    llvm::sys::path::append(P, CRT, "libpascalrt.a");
    return std::string(P);
  };
  if (!llvm::sys::fs::exists(Variant("msvcrt")) &&
      !llvm::sys::fs::exists(Variant("ucrt")))
    return Flat.str();
  std::string CRT;
  if (auto Env = llvm::sys::Process::GetEnv("P3_WINDOWS_CRT")) {
    CRT = *Env;
    if (CRT != "msvcrt" && CRT != "ucrt") {
      Diags.error("P3_WINDOWS_CRT is '" + llvm::Twine(CRT) +
                  "': it must be msvcrt or ucrt");
      return "";
    }
  } else
    CRT = mingwCRT(CC);
  if (CRT.empty()) {
    Diags.error("cannot tell whether '" + llvm::Twine(CC) +
                "' links msvcrt or UCRT; set P3_WINDOWS_CRT to msvcrt or ucrt");
    return "";
  }
  return Variant(CRT);
}

#ifdef P3_EMBED_LLD
/// MinGW-w64's libraries, for the lld inside p3: $P3_MINGW_SYSROOT, else
/// <dir of p3>/../lib/mingw-w64; "" when there are none (no crt2.o).
static std::string findMinGWLibraries() {
  llvm::SmallString<256> Dir;
  if (auto Env = llvm::sys::Process::GetEnv("P3_MINGW_SYSROOT")) {
    Dir = *Env;
  } else {
    std::string Exe = selfExecutable();
    Dir = llvm::sys::path::parent_path(Exe);
    llvm::sys::path::append(Dir, "..", "lib", "mingw-w64");
    llvm::sys::path::remove_dots(Dir, /*remove_dot_dot=*/true);
  }
  llvm::SmallString<256> Crt(Dir);
  llvm::sys::path::append(Crt, "crt2.o");
  return llvm::sys::fs::exists(Crt) ? std::string(Dir) : "";
}

/// Links a Windows program with lld, in this process, the way clang's
/// MinGW driver does with compiler-rt (no GCC runtime): MinGW-w64's start
/// files and libraries from Libs, statically.
static bool linkWithLLD(llvm::StringRef ObjectFile,
                        llvm::ArrayRef<std::string> UnitObjects,
                        llvm::StringRef RuntimeLib,
                        llvm::ArrayRef<std::string> Extra,
                        llvm::StringRef OutputFile, llvm::StringRef Libs,
                        DiagnosticsEngine &Diags) {
  auto In = [&](const char *File) {
    llvm::SmallString<256> P(Libs);
    llvm::sys::path::append(P, File);
    return std::string(P);
  };
  std::vector<std::string> A = {"ld.lld", "-m", "i386pep", "-Bstatic",
                                "--gc-sections", "-o", OutputFile.str(),
                                In("crt2.o"), In("crtbegin.o"),
                                "-L" + Libs.str(), ObjectFile.str()};
  A.insert(A.end(), UnitObjects.begin(), UnitObjects.end());
  A.push_back(RuntimeLib.str());
  A.insert(A.end(), Extra.begin(), Extra.end());
  // Winsock (the runtime's socket waits), winpthreads (its tasks), then
  // what every MinGW program links (lld finds symbols in any order).
  for (const char *L : {"-lws2_32", "-lpthread", "-lmingw32"})
    A.push_back(L);
  A.push_back(In("libclang_rt.builtins-x86_64.a"));
  for (const char *L : {"-lmoldname", "-lmingwex", "-lmsvcrt", "-ladvapi32",
                        "-lshell32", "-luser32", "-lkernel32"})
    A.push_back(L);
  A.push_back(In("crtend.o"));
  std::vector<const char *> Argv;
  for (const std::string &S : A)
    Argv.push_back(S.c_str());
  lld::Result R = lld::lldMain(Argv, llvm::outs(), llvm::errs(),
                               {{lld::MinGW, &lld::mingw::link}});
  if (R.retCode != 0) {
    Diags.error("linker command failed with exit code " + llvm::Twine(R.retCode));
    return false;
  }
  return true;
}
#endif

bool p3::linkExecutable(llvm::StringRef ObjectFile,
                         llvm::ArrayRef<std::string> UnitObjects,
                         llvm::StringRef OutputFile,
                         llvm::ArrayRef<std::string> LibraryDirs,
                         llvm::ArrayRef<std::string> Libraries,
                         llvm::StringRef Triple, DiagnosticsEngine &Diags) {
  // A Windows host with lld inside p3 and MinGW-w64's libraries next to it
  // needs no C toolchain (P3_CC still names one).
  std::string LLDLibs;
#ifdef P3_EMBED_LLD
  if (isHostTriple(Triple) && llvm::Triple(Triple).isWindowsGNUEnvironment() &&
      !llvm::sys::Process::GetEnv("P3_CC"))
    LLDLibs = findMinGWLibraries();
#endif
  std::string CC;
  if (LLDLibs.empty()) {
    CC = findLinkerDriver(Triple, Diags);
    if (CC.empty())
      return false;
  }

  std::string RuntimeLib = findRuntimeLibrary(Triple);
  if (!isHostTriple(Triple) && llvm::Triple(Triple).isWindowsGNUEnvironment()) {
    RuntimeLib = mingwRuntimeLibrary(RuntimeLib, CC, Diags);
    if (RuntimeLib.empty())
      return false;
  }
  if (!llvm::sys::fs::exists(RuntimeLib)) {
    Diags.error("cannot find the Pascal runtime library '" +
                llvm::Twine(RuntimeLib) + "'; set P3_RUNTIME_LIB");
    return false;
  }

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
#ifdef P3_EMBED_LLD
  if (!LLDLibs.empty())
    return linkWithLLD(ObjectFile, UnitObjects, RuntimeLib, Extra, OutputFile,
                       LLDLibs, Diags);
#endif
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
  // MinGW: a self-contained executable, without MinGW's own DLLs. The
  // runtime's tasks use winpthreads, which a toolchain of the posix thread
  // model links by itself and one of the win32 model (Debian's and Ubuntu's
  // default) does not.
  if (T.isWindowsGNUEnvironment())
    Args.append({"-static", "-lpthread"});
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
