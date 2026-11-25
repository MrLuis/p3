#!/bin/sh
# Smoke test of an unpacked package, for a system without LLVM:
#
#   utils/package-smoke.sh <package dir> <p3 source tree>
#
# The source tree gives only the test programs. Without a C compiler it
# checks what works then (--version, -c, and the message when linking);
# with one, it builds and runs programs.
set -eu

if [ $# -ne 2 ]; then
  echo "usage: $0 <package dir> <p3 source tree>" >&2
  exit 2
fi
p3=$(cd "$1" && pwd)/bin/p3
tests=$(cd "$2" && pwd)/tests
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"

fail() {
  echo "package-smoke: FAIL: $*" >&2
  exit 1
}

# macOS has no timeout(1).
limit() {
  if command -v timeout >/dev/null; then
    timeout 60 "$@"
  else
    "$@"
  fi
}

# Nothing may point p3 anywhere but into its own package.
unset P3_UNIT_DIR P3_RUNTIME_LIB P3_RUNTIME_IR P3_CC

if ls -d /usr/lib/llvm* /usr/lib/*/libLLVM* >/dev/null 2>&1; then
  echo "package-smoke: note: this system has LLVM installed"
fi
"$p3" --version

if ! command -v cc >/dev/null && ! command -v gcc >/dev/null &&
   ! command -v clang >/dev/null; then
  "$p3" -c "$tests/driver/Inputs/hello.pas" -o hello.o || fail "-c"
  [ -s hello.o ] || fail "-c wrote no object"
  if "$p3" "$tests/driver/Inputs/hello.pas" -o hello 2>link.err; then
    fail "linked without a C compiler"
  fi
  grep -q 'cannot find a C compiler to link with' link.err || {
    cat link.err >&2
    fail "unexpected message without a C compiler"
  }
  echo "package-smoke: PASS (without a C compiler: --version, -c, the link message)"
  exit 0
fi

"$p3" "$tests/driver/Inputs/hello.pas" -o hello || fail "building hello"
[ "$(./hello)" = 'Hello, World!' ] || fail "hello's output"

# Library units: compiled with the program and linked from their packaged
# objects, with and without optimization and debug information.
for opts in -O0 -O2 -g '-O2 -fseparate-units'; do
  # $opts is split into its options.
  "$p3" --mode=objfpc $opts "$tests/driver/Inputs/install-units.pas" -o units ||
    fail "building with units ($opts)"
  [ "$(./units)" = 'INSTALLED moved' ] || fail "the units program's output ($opts)"
done

# Tasks (per-architecture assembly) and the network units (epoll or kqueue,
# loopback).
for program in async netasync-echo; do
  for opt in -O0 -O2; do
    "$p3" --mode=objfpc $opt "$tests/run/$program.pas" -o "$program" ||
      fail "building $program $opt"
    limit "./$program" > "$program.out" || fail "running $program $opt"
    diff "$program.out" "$tests/run/$program.expected" ||
      fail "$program's output ($opt)"
  done
done

echo "package-smoke: PASS"
