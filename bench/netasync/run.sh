#!/usr/bin/env bash
# Request/response benchmark: the netasync echo server against the
# event-loop reference (loop_echo.c, mill's loopsrv style), same client.
#
#   bench/netasync/run.sh [conns=150] [seconds=5] [threads="1 4"]
#
# Builds into $OUT (default: a temp dir) with the p3 on PATH, or $P3.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CONNS=${1:-150}
SECS=${2:-5}
THREADS=${3:-"1 4"}
P3=${P3:-p3}
OUT=${OUT:-$(mktemp -d)}
CC=${CC:-cc}

$CC -O2 -pthread "$HERE/pingpong.c" -o "$OUT/pingpong"
$CC -O2 -pthread "$HERE/loop_echo.c" -o "$OUT/loop_echo"
(cd "$OUT" && $P3 --mode=objfpc -O2 "$HERE/echo.pas" -o "$OUT/echo" >/dev/null)

PORT=$((20000 + RANDOM % 20000))
run() { # name, server command...
  local name=$1; shift
  "$@" >/dev/null 2>&1 &
  local pid=$!
  for _ in $(seq 50); do
    nc -z 127.0.0.1 "$PORT" 2>/dev/null && break
    sleep 0.1
  done
  printf '%-22s ' "$name"
  "$OUT/pingpong" "$PORT" "$CONNS" "$SECS" 64 4 || true
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  PORT=$((PORT + 1))
}

echo "conns=$CONNS seconds=$SECS ($(uname -sm))"
for t in $THREADS; do
  run "loop_echo  threads=$t" "$OUT/loop_echo" "$PORT" "$t"
  run "netasync   threads=$t" env P3_POOL_THREADS="$t" "$OUT/echo" "$PORT"
done
