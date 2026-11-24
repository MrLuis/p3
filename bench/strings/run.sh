#!/bin/sh
# Builds strbench with the unit library's current sources and with those
# of another commit (default: HEAD~1... as given), and runs both.
#
#   bench/strings/run.sh [COMMIT] [SECONDS]
#   P3=<compiler> to use another p3 (default: build/bin/p3)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
p3=${P3:-$root/build/bin/p3}
base=${1:-main}
secs=${2:-0.3}
out=$(mktemp -d)
mkdir -p "$out/base"
for u in sysutils strutils classes nethttp netloop json; do
  git -C "$root" show "$base:rtl/$u.pas" > "$out/base/$u.pas" 2>/dev/null || true
done
"$p3" --no-project --mode=objfpc -O2 "$here/strbench.pas" -o "$out/new"
"$p3" --no-project --mode=objfpc -O2 -I"$out/base" "$here/strbench.pas" -o "$out/old"
echo "== $base"; "$out/old" "$secs"
echo "== working tree"; "$out/new" "$secs"
rm -rf "$out"
