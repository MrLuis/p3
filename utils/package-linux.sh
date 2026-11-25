#!/bin/sh
# Makes the downloadable Linux archive of a p3 built with LLVM linked
# statically (.github/workflows/package.yml configures it):
#
#   utils/package-linux.sh <build dir> <output dir>
#
# writes <output dir>/p3-<version>-linux-<arch>.tar.xz and its .sha256. It
# stops if the installed p3 needs a library or a glibc that a target system
# may not have.
#
#   MAX_GLIBC         the newest glibc the package may need (default 2.35,
#                     Ubuntu 22.04's)
#   LLVM_LICENSE_DIR  where the LLVM package's copyright file is
#                     (default /usr/share/doc/llvm-23-dev)
set -eu

if [ $# -ne 2 ]; then
  echo "usage: $0 <build dir> <output dir>" >&2
  exit 2
fi
build=$1
out=$2
max_glibc=${MAX_GLIBC:-2.35}
llvm_doc=${LLVM_LICENSE_DIR:-/usr/share/doc/llvm-23-dev}
apache=/usr/share/common-licenses/Apache-2.0

fail() {
  echo "package-linux: $*" >&2
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
  x86_64) arch=x86_64 ;;
  aarch64) arch=arm64 ;;
  *) fail "unexpected architecture $(uname -m)" ;;
esac
name=p3-$version-linux-$arch

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
root=$work/$name
cmake --install "$build" --prefix "$root" --strip >/dev/null

# The runtime IR lets p3 inline runtime functions; do not ship without it.
[ -f "$root/lib/libpascalrt.ll" ] || fail "lib/libpascalrt.ll was not built"

# What the compiler needs from the user's system: glibc, and zlib and zstd,
# which every Debian and Ubuntu system has. Not LLVM, and not the C++
# runtime.
needed=$(readelf -d "$root/bin/p3" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
for lib in $needed; do
  case $lib in
    libc.so.6 | libm.so.6 | libdl.so.2 | librt.so.1 | libpthread.so.0) ;;
    libz.so.1 | libzstd.so.1 | ld-linux-*) ;;
    *) fail "bin/p3 needs $lib" ;;
  esac
done
glibc=$(objdump -T "$root/bin/p3" | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -uV | tail -1)
newest=$(printf '%s\n%s\n' "$glibc" "$max_glibc" | sort -V | tail -1)
[ "$newest" = "$max_glibc" ] || fail "bin/p3 needs glibc $glibc, newer than $max_glibc"

# p3's own license and the third-party notices (installed by CMake).
for f in LICENSE THIRD_PARTY_NOTICES.md; do
  [ -s "$root/share/doc/p3/$f" ] || fail "share/doc/p3/$f was not installed"
done

# LLVM is in bin/p3: its license goes along, as LLVM publishes it (with its
# notes on third-party code), and with the copyright file of the package it
# came from. That Debian file refers to the system's copy of the Apache
# License for the full text.
llvm_license=$(dirname "$0")/../licenses/llvm/LICENSE.TXT
[ -f "$llvm_license" ] || fail "no $llvm_license"
cp "$llvm_license" "$root/share/doc/p3/LICENSE.llvm-project"
[ -f "$llvm_doc/copyright" ] || fail "no $llvm_doc/copyright"
[ -f "$apache" ] || fail "no $apache"
{
  cat "$llvm_doc/copyright"
  printf '\n\n---- Apache License 2.0 ----\n\n'
  cat "$apache"
} > "$root/share/doc/p3/LICENSE.llvm"

mkdir -p "$out"
tar -C "$work" --owner=0 --group=0 -cJf "$out/$name.tar.xz" "$name"
(cd "$out" && sha256sum "$name.tar.xz" > "$name.tar.xz.sha256")

echo "package-linux: $out/$name.tar.xz ($(du -h "$out/$name.tar.xz" | cut -f1))"
echo "package-linux: bin/p3 needs glibc $glibc and:" $needed
