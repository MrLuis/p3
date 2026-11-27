# Building p3

How to build p3 from source, install it, and run its tests. To use p3
without building it, see [Building and installing](../README.md#building-and-installing)
in the README.

## Requirements

CMake 3.20 or later, a C++20 compiler and LLVM 23 (CMake rejects other
major versions). Ninja is used below. The self-hosted compiler and the
inlining of runtime functions also need Python 3; the latter needs LLVM's
`clang` and `llvm-link` too. If CMake does not find LLVM, pass
`-DLLVM_DIR=<llvm>/lib/cmake/llvm`.

```sh
cmake -G Ninja -S . -B build
ninja -C build
```

The default configuration is `Release` (`-O2`/`-O3`); use
`-DCMAKE_BUILD_TYPE=Debug` (or `RelWithDebInfo`) while working on the
compiler. The C runtime is built at `-O2` in every configuration, and
`ninja -C build p3-self` builds the distributed self-hosted compiler at
`-O2`; `-DP3_SELFHOST_NO_CHECKS=ON` turns its range and overflow checks
off once the compiler is trusted enough for that.

## Ubuntu 24.04

LLVM 23 comes from [apt.llvm.org](https://apt.llvm.org):

```sh
sudo apt-get update
sudo apt-get install -y git cmake ninja-build build-essential python3 wget \
  zlib1g-dev libzstd-dev libedit-dev libxml2-dev
wget https://apt.llvm.org/llvm.sh
sudo bash llvm.sh 23
sudo apt-get install -y llvm-23-dev llvm-23-tools clang-23

cmake -G Ninja -S . -B build \
  -DLLVM_DIR=/usr/lib/llvm-23/lib/cmake/llvm \
  -DCMAKE_C_COMPILER=clang-23 -DCMAKE_CXX_COMPILER=clang++-23
ninja -C build
```

## macOS

LLVM 23 comes from Homebrew (`llvm`); the compiler itself is built with
Apple's, as in CI:

```sh
brew install cmake ninja llvm lit

cmake -G Ninja -S . -B build \
  -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm"
ninja -C build
```

Programs are linked with the Command Line Tools' `cc`
(`xcode-select --install`).

## Windows

p3 builds on Windows with [MSYS2](https://www.msys2.org)'s UCRT64
environment and its LLVM 23. In the "MSYS2 UCRT64" shell:

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-{llvm,clang,lld,cmake,ninja,python}

cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="$(llvm-config --cmakedir)" \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DP3_CROSS_TARGETS=OFF
ninja -C build p3 pascalrt rtl
```

That p3 links programs with MSYS2's `clang` (in PATH, or named by `P3_CC`).
The release's Windows package is built differently (see
`.github/workflows/windows-compiler.yml`): LLVM linked statically, with lld
inside p3 (`-DP3_LINK_LLVM_DYLIB=OFF -DP3_LLVM_STATIC_DEPS=ON
-DP3_EMBED_LLD=ON -DCMAKE_EXE_LINKER_FLAGS=-static`), so that it links
programs itself from MinGW-w64's libraries in `lib/mingw-w64` (from
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw);
`utils/package-windows.sh` puts them there). The self-hosted compiler and
the lit tests do not run on Windows yet.

Alternatively, build the Linux p3 in
[WSL2](https://learn.microsoft.com/windows/wsl/install): in an administrator
PowerShell, run

```powershell
wsl --install -d Ubuntu-24.04
```

Then follow the Ubuntu 24.04 steps above inside Ubuntu, with the repository
cloned into the Linux file system (under `~`, which is much faster than
`/mnt/c`). The compiler is then a Linux program. For it to write Windows
executables, install MinGW-w64 (`sudo apt-get install -y mingw-w64`) before
running `cmake` (if `build/` is already configured, run `cmake` again). Either
of MinGW's thread variants works.

## The runtime for Windows programs

On any host, when CMake finds MinGW-w64 (`x86_64-w64-mingw32-gcc`: `brew
install mingw-w64`, or the distribution's `mingw-w64` package), it also
builds the runtime for Windows programs, once for each of Windows' C
libraries: `lib/x86_64-w64-mingw32/msvcrt/` and
`lib/x86_64-w64-mingw32/ucrt/libpascalrt.a`. `-DP3_CROSS_TARGETS=<triples>`
picks the targets to build the runtime for (`OFF`: none). How p3 uses them
is under **Cross-compiling** in
[Compiler parameters](compiler-params-and-directives.md#compiler-parameters).

## Installing

After building:

```sh
cmake --install build --prefix <dir>
```

This writes the C++ compiler to `<dir>/bin/p3`, the runtime and unit library
to `<dir>/lib` (with the Windows runtimes, if they were built), and the
licenses to `<dir>/share/doc/p3`. p3 finds its runtime and units relative to
its own location, so the directory can be moved; keep its layout. The
self-hosted compiler is not installed.

The installed p3 still needs the shared LLVM library it was built against,
and a C compiler to link programs. With `-DP3_LINK_LLVM_DYLIB=OFF` LLVM is
linked in instead; the release archives are built that way
(`.github/workflows/package.yml`, `utils/package-linux.sh`,
`utils/package-macos.sh`). LLVM's targets then make most of p3's size:
`-DP3_LLVM_TARGETS="X86;AArch64"` links only those (the release archives
do; the default is every target LLVM was built with, and the host's must be
in the list). p3 cannot generate code for the others: `--target` says
"unsupported target".

## Testing

Tests are [lit](https://llvm.org/docs/CommandGuide/lit.html) files under
`tests/`:

```sh
ninja -C build check
```

This needs `lit` and `FileCheck`. On Ubuntu, `FileCheck` is in
`llvm-23-tools`; install `lit` with `pipx install lit` (package `pipx`, then
`pipx ensurepath` and a new shell) and run `cmake` again.

`ninja -C build check-bootstrap` builds the self-hosted compiler with
itself (stage 1, 2 and 3, and a stage 1 built by Free Pascal when `fpc` is
installed) and checks that all write the same IR.

With MinGW-w64 and Wine installed, `tests/windows` also runs the programs
of `tests/run` as Windows executables (see the README's Cross-compiling).
