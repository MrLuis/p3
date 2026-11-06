#!/bin/sh
# Compiles and runs the dialect check programs with one compiler and compares
# the output with the .expected files.
#
#   run.sh fpc                 # Free Pascal (the reference implementation)
#   run.sh pcc [path/to/pcc]   # pcc --mode=tp (default: build/bin/pcc)
#   run.sh fpc --update        # regenerate the .expected files from FPC
#
# Exit status: 0 if every program matches, 1 otherwise.
set -u
here=$(cd "$(dirname "$0")" && pwd)
compiler=${1:-}
case "$compiler" in
  fpc) update=${2:-} ;;
  pcc) pcc=${2:-$here/../../build/bin/pcc}; update= ;;
  *) echo "usage: $0 fpc [--update] | pcc [path/to/pcc]" >&2; exit 2 ;;
esac

work=$(mktemp -d "${TMPDIR:-/tmp}/pcc-dialect.XXXXXX")
trap 'rm -rf "$work"' EXIT
pass=0; fail=0

for src in "$here"/d[0-9][0-9]_*.pas; do
  name=$(basename "$src" .pas)
  exe=$work/$name
  if [ "$compiler" = fpc ]; then
    # objfpc mode with short strings; range and overflow
    # checks on, as in pcc.
    fpc -v0 -Mobjfpc -Sh- -Cr -Co -Fu"$here" -Fi"$here" -FU"$work" \
        -o"$exe" "$src" >"$work/$name.log" 2>&1
  else
    "$pcc" --mode=tp -I "$here" "$src" -o "$exe" >"$work/$name.log" 2>&1
  fi
  if [ $? -ne 0 ]; then
    echo "FAIL $name (compile)"; sed 's/^/    /' "$work/$name.log" | head -5
    fail=$((fail + 1)); continue
  fi
  # Run in the work directory (d03 writes a temporary file there).
  if [ "$name" = d04_cmdline ]; then
    (cd "$work" && "$exe" alpha 'beta gamma'; echo "exit=$?") >"$work/$name.out" 2>&1
  else
    (cd "$work" && "$exe"; echo "exit=$?") >"$work/$name.out" 2>&1
  fi
  if [ -n "$update" ]; then
    cp "$work/$name.out" "$here/$name.expected"
    echo "UPDATED $name"
  elif diff -u "$here/$name.expected" "$work/$name.out" >"$work/$name.diff"; then
    echo "PASS $name"; pass=$((pass + 1))
  else
    echo "FAIL $name (output)"; sed 's/^/    /' "$work/$name.diff" | head -12
    fail=$((fail + 1))
  fi
done

[ -n "$update" ] && exit 0
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
