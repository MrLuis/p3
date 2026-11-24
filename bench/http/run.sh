#!/bin/sh
# Builds httpbench with the unit library's current sources and with those
# of another commit (default: main), and runs both.
#
#   bench/http/run.sh [COMMIT] [SECONDS]
#   P3=<compiler> to use another p3 (default: build/bin/p3)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
p3=${P3:-$root/build/bin/p3}
base=${1:-main}
secs=${2:-0.3}
out=$(mktemp -d)
mkdir -p "$out/base"
for u in nethttp netrouter netloop netasync threading sysutils classes json_pow5.inc; do
  f=$u; case $u in *.inc) ;; *) f=$u.pas ;; esac
  git -C "$root" show "$base:rtl/$f" > "$out/base/$f" 2>/dev/null || rm -f "$out/base/$f"
done
"$p3" --no-project --mode=objfpc -O2 -dRESUME "$here/httpbench.pas" -o "$out/new"
"$p3" --no-project --mode=objfpc -O2 -I"$out/base" "$here/httpbench.pas" -o "$out/old"
echo "== $base"; "$out/old" "$secs"
echo "== working tree"; "$out/new" "$secs"
rm -rf "$out"
