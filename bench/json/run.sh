#!/bin/sh
# Builds and runs the benchmark of unit json, then (with cargo) the same
# operations with serde_json and sonic-rs.
#
#   bench/json/run.sh [SECONDS]      (per operation; default 1)
#   P3=<compiler> to use another p3 (default: build/bin/p3)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
p3=${P3:-$root/build/bin/p3}
secs=${1:-1}
sh "$here/fetch-data.sh"
out=$(mktemp -d)
"$p3" --no-project --mode=objfpc -O2 --generator=json --generated-dir="$out/gen" \
  "$here/jsonbench.pas" -o "$out/jsonbench"
echo "== p3, unit json"
"$out/jsonbench" "$here/data" "$secs"
if command -v cargo >/dev/null; then
  echo "== Rust: serde_json, sonic-rs"
  (cd "$here/rust" && cargo run --quiet --release -- ../data "$secs")
fi
rm -rf "$out"
