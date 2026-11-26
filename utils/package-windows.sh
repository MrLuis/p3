#!/bin/sh
# Makes the downloadable Windows archive (run in MSYS2's UCRT64 shell, after
# building p3 there with LLVM linked statically and -DP3_EMBED_LLD=ON;
# .github/workflows/windows-compiler.yml):
#
#   utils/package-windows.sh <build dir> <llvm-mingw dir> <output dir>
#
# writes <output dir>/p3-<version>-windows-x86_64.zip and its .sha256. The
# archive holds p3.exe (LLVM and lld inside it), its runtime and units, and
# MinGW-w64's libraries for x86-64 Windows (UCRT) with compiler-rt's
# builtins, from llvm-mingw (github.com/mstorsjo/llvm-mingw), in
# lib/mingw-w64, where p3 links programs from: nothing else to install.
#
#   P3_VERSION   names a release (0.2.0-rc.1); its number must be CMake's
set -eu

if [ $# -ne 3 ]; then
  echo "usage: $0 <build dir> <llvm-mingw dir> <output dir>" >&2
  exit 2
fi
build=$1
lm=$2
out=$3

fail() {
  echo "package-windows: $*" >&2
  exit 1
}

version=$(sed -n 's/^project(p3 VERSION \([0-9.]*\) .*/\1/p' "$(dirname "$0")/../CMakeLists.txt")
[ -n "$version" ] || fail "no version in CMakeLists.txt"
case ${P3_VERSION:-$version} in
  "$version" | "$version"-*) version=${P3_VERSION:-$version} ;;
  *) fail "P3_VERSION $P3_VERSION is not version $version" ;;
esac
name=p3-$version-windows-x86_64

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
root=$work/$name
cmake --install "$build" --prefix "$root" --strip >/dev/null

[ -f "$root/bin/p3.exe" ] || fail "bin/p3.exe was not installed"
[ -f "$root/lib/pascal/libpascalrt.ll" ] || fail "lib/pascal/libpascalrt.ll was not built"

# p3.exe may need Windows' own DLLs only, none of MSYS2's.
needed=$(objdump -p "$root/bin/p3.exe" | sed -n 's/.*DLL Name: //p' | sort -u)
for dll in $needed; do
  [ ! -e "${MINGW_PREFIX:-/ucrt64}/bin/$dll" ] || fail "bin/p3.exe needs $dll (MSYS2's)"
done

# MinGW-w64's x86-64 libraries, built with clang and compiler-rt (no GCC
# runtime), without the C++ ones.
src=$lm/x86_64-w64-mingw32/lib
builtins=$(ls "$lm"/lib/clang/*/lib/windows/libclang_rt.builtins-x86_64.a 2>/dev/null | head -1)
[ -f "$src/crt2.o" ] || fail "no $src/crt2.o (not an llvm-mingw directory?)"
[ -n "$builtins" ] || fail "no compiler-rt builtins for x86_64 in $lm/lib/clang"
mkdir -p "$root/lib/mingw-w64"
for f in "$src"/*; do
  case $(basename "$f") in
    libc++* | libunwind* | *.la | *.dll) ;;
    *) cp -R "$f" "$root/lib/mingw-w64/" ;;
  esac
done
cp "$builtins" "$root/lib/mingw-w64/"

# Licenses: LLVM's (p3.exe, lld, compiler-rt) with the third-party code in
# LLVM's Support library, MinGW-w64's and winpthreads'
# (the libraries programs are linked with), and zstd's (linked into p3.exe;
# BSD: its notice goes along).
doc=$root/share/doc/p3
[ -s "$doc/LICENSE" ] || fail "share/doc/p3/LICENSE was not installed"
cp "$(dirname "$0")/../licenses/llvm/LICENSE.TXT" "$doc/LICENSE.llvm-project"
cp "$(dirname "$0")/../licenses/llvm/THIRD-PARTY.txt" "$doc/LICENSE.llvm-third-party"
for f in COPYING.MinGW-w64.txt COPYING.MinGW-w64-runtime.txt COPYING.winpthreads.txt; do
  [ -f "$lm/x86_64-w64-mingw32/share/mingw32/$f" ] || fail "no $f in llvm-mingw"
  cp "$lm/x86_64-w64-mingw32/share/mingw32/$f" "$doc/LICENSE.${f#COPYING.}"
done
zstd_license=${MINGW_PREFIX:-/ucrt64}/share/licenses/zstd/LICENSE
[ -f "$zstd_license" ] || fail "no zstd license ($zstd_license)"
cp "$zstd_license" "$doc/LICENSE.zstd"

mkdir -p "$out"
out=$(cd "$out" && pwd)
(cd "$work" && zip -qr9 "$out/$name.zip" "$name")
(cd "$out" && sha256sum "$name.zip" > "$name.zip.sha256")

echo "package-windows: $out/$name.zip ($(du -h "$out/$name.zip" | cut -f1))"
echo "package-windows: bin/p3.exe needs:" $needed
