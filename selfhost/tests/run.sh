#!/bin/sh
# Builds and runs the selfhost unit tests (t_*.pas) with one compiler and
# compares their output with the .expected files.
#
#   run.sh fpc | run.sh p3 [path/to/p3]
#
# The units are in selfhost/; tests run in this directory (they read
# Inputs/). Exit status: 0 if every test matches.
set -u
here=$(cd "$(dirname "$0")" && pwd)
units=$(cd "$here/.." && pwd)
compiler=${1:-}
case "$compiler" in
  fpc) ;;
  p3) p3=${2:-$units/../build/bin/p3} ;;
  *) echo "usage: $0 fpc | p3 [path/to/p3]" >&2; exit 2 ;;
esac
work=$(mktemp -d "${TMPDIR:-/tmp}/p3-selfhost.XXXXXX")
trap 'rm -rf "$work"' EXIT
pass=0; fail=0
for src in "$here"/t_*.pas; do
  name=$(basename "$src" .pas)
  if [ "$compiler" = fpc ]; then
    fpc -v0 -Mobjfpc -Sh- -Cr -Co -Fu"$units" -FU"$work" -o"$work/$name" "$src" \
      >"$work/$name.log" 2>&1
  else
    "$p3" --mode=tp -I "$units" "$src" -o "$work/$name" >"$work/$name.log" 2>&1
  fi
  if [ $? -ne 0 ]; then
    echo "FAIL $name (compile)"; sed 's/^/    /' "$work/$name.log" | head -8
    fail=$((fail + 1)); continue
  fi
  (cd "$here" && "$work/$name") >"$work/$name.out" 2>&1
  if diff -u "$here/$name.expected" "$work/$name.out" >"$work/$name.diff"; then
    echo "PASS $name"; pass=$((pass + 1))
  else
    echo "FAIL $name (output)"; sed 's/^/    /' "$work/$name.diff" | head -20
    fail=$((fail + 1))
  fi
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
