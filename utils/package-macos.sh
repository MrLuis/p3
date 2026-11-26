#!/bin/sh
# Makes the downloadable macOS archive of a p3 built with LLVM linked
# statically, and LLVM's zstd with it (-DP3_LLVM_STATIC_DEPS=ON;
# .github/workflows/package.yml configures it):
#
#   utils/package-macos.sh <build dir> <output dir>
#
# writes <output dir>/p3-<version>-macos-<arch>.tar.xz and its .sha256. It
# stops if the installed p3 needs a library outside macOS, or if p3 or the
# runtime needs a newer macOS than MACOS_MIN.
#
#   MACOS_MIN       the oldest macOS the package is for (default: the
#                   deployment target p3 was built with)
#   ZSTD_LICENSE    zstd's license, as it is linked into p3 (default:
#                   Homebrew's zstd's LICENSE)
set -eu

if [ $# -ne 2 ]; then
  echo "usage: $0 <build dir> <output dir>" >&2
  exit 2
fi
build=$1
out=$2

fail() {
  echo "package-macos: $*" >&2
  exit 1
}

# P3_VERSION names a release (0.2.0-rc.1); its number must be CMake's.
version=$(sed -n 's/^project(p3 VERSION \([0-9.]*\) .*/\1/p' "$(dirname "$0")/../CMakeLists.txt")
[ -n "$version" ] || fail "no version in CMakeLists.txt"
case ${P3_VERSION:-$version} in
  "$version" | "$version"-*) version=${P3_VERSION:-$version} ;;
  *) fail "P3_VERSION $P3_VERSION is not version $version" ;;
esac
case $(uname -m) in
  arm64) arch=arm64 ;;
  *) fail "unexpected architecture $(uname -m)" ;;
esac
name=p3-$version-macos-$arch

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
root=$work/$name
cmake --install "$build" --prefix "$root" --strip >/dev/null

[ -f "$root/lib/pascal/libpascalrt.ll" ] || fail "lib/pascal/libpascalrt.ll was not built"

# Only macOS's own libraries: not LLVM's, and nothing from Homebrew.
needed=$(otool -L "$root/bin/p3" | tail -n +2 | awk '{print $1}')
for lib in $needed; do
  case $lib in
    /usr/lib/* | /System/Library/*) ;;
    *) fail "bin/p3 needs $lib" ;;
  esac
done

# The macOS version p3 and the runtime need: none newer than the package's.
minos() { # the minimum versions in a Mach-O file or archive, one a line
  otool -l "$1" | awk '/LC_BUILD_VERSION/ {b = 1} b && $1 == "minos" {print $2; b = 0}' | sort -uV
}
p3_min=$(minos "$root/bin/p3" | tail -1)
[ -n "$p3_min" ] || fail "no minimum macOS version in bin/p3"
max=${MACOS_MIN:-$p3_min}
for v in $p3_min $(minos "$root/lib/libpascalrt.a"); do
  newest=$(printf '%s\n%s\n' "$v" "$max" | sort -V | tail -1)
  [ "$newest" = "$max" ] || fail "the package needs macOS $v, newer than $max"
done

# The runtime for Windows programs (cross-compiled with MinGW-w64), and the
# license of wepoll, which it contains.
for crt in msvcrt ucrt; do
  [ -f "$root/lib/x86_64-w64-mingw32/$crt/libpascalrt.a" ] ||
    fail "lib/x86_64-w64-mingw32/$crt/libpascalrt.a was not built (P3_CROSS_TARGETS)"
done

for f in LICENSE THIRD_PARTY_NOTICES.md LICENSE.wepoll; do
  [ -s "$root/share/doc/p3/$f" ] || fail "share/doc/p3/$f was not installed"
done

# LLVM and zstd are in bin/p3: their licenses go along.
llvm_license=$(dirname "$0")/../licenses/llvm/LICENSE.TXT
[ -f "$llvm_license" ] || fail "no $llvm_license"
cp "$llvm_license" "$root/share/doc/p3/LICENSE.llvm-project"
llvm_third_party=$(dirname "$0")/../licenses/llvm/THIRD-PARTY.txt
[ -f "$llvm_third_party" ] || fail "no $llvm_third_party"
cp "$llvm_third_party" "$root/share/doc/p3/LICENSE.llvm-third-party"
zstd_license=${ZSTD_LICENSE:-$(brew --prefix zstd 2>/dev/null)/LICENSE}
[ -f "$zstd_license" ] || fail "no zstd license ($zstd_license)"
cp "$zstd_license" "$root/share/doc/p3/LICENSE.zstd"

mkdir -p "$out"
COPYFILE_DISABLE=1 tar -C "$work" --uid 0 --gid 0 -cJf "$out/$name.tar.xz" "$name"
(cd "$out" && shasum -a 256 "$name.tar.xz" > "$name.tar.xz.sha256")

echo "package-macos: $out/$name.tar.xz ($(du -h "$out/$name.tar.xz" | cut -f1))"
echo "package-macos: for macOS $max and later; bin/p3 needs:" $needed
