#!/usr/bin/env bash
# Runs the Windows programs of utils/windows-bundle.py on Windows (Git
# Bash), each in a directory of its own, and compares each one's output
# with what it must print:
#
#   utils/windows-run-bundle.sh <bundle dir>
#
# RUNNER runs a program somewhere else (RUNNER=wine, to try this script).
# NET_REPEAT runs the event-loop programs (net*) that many times: their
# failures can depend on timing (default 1).
set -u

if [ $# -ne 1 ] || [ -z "$1" ]; then
  echo "usage: $0 <bundle dir>" >&2
  exit 2
fi
# Stop unless $1 is a directory with programs in it: never run whatever
# *.exe an empty or wrong path would name.
if ! bundle=$(cd "$1" && pwd) || [ -z "$bundle" ]; then
  echo "windows-run-bundle: no directory '$1'" >&2
  exit 2
fi
if ! ls "$bundle"/*.exe >/dev/null 2>&1; then
  echo "windows-run-bundle: no programs in $bundle" >&2
  exit 2
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
runner=${RUNNER:-}
# timeout(1) is part of Git for Windows; macOS has none.
limit=
if command -v timeout >/dev/null; then limit="timeout 120"; fi

passed=0
failed=()
for exe in "$bundle"/*.exe; do
  name=$(basename "$exe" .exe)
  times=1
  case $name in net*) times=${NET_REPEAT:-1} ;; esac
  for run in $(seq 1 "$times"); do
    dir=$work/$name.$run
    mkdir "$dir"
    (cd "$dir" && $limit $runner "$exe" </dev/null >out 2>err)
    status=$?
    tr -d '\r' <"$dir/out" >"$dir/out.lf"
    if [ $status -ne 0 ] || ! cmp -s "$bundle/$name.expected" "$dir/out.lf"; then
      [ "$times" -gt 1 ] && echo "($name: run $run of $times)"
      break
    fi
  done
  if [ $status -ne 0 ]; then
    echo "FAIL: $name: exit status $status"
    tail -20 "$dir/out.lf" | sed 's/^/  stdout: /'
    head -20 "$dir/err" | sed 's/^/  stderr: /'
    failed+=("$name")
  elif ! diff -u "$bundle/$name.expected" "$dir/out.lf" >"$dir/diff"; then
    echo "FAIL: $name: output differs"
    head -40 "$dir/diff"
    failed+=("$name")
  else
    passed=$((passed + 1))
  fi
done

echo "windows-run-bundle: $passed passed, ${#failed[@]} failed"
if [ ${#failed[@]} -ne 0 ]; then
  echo "failed: ${failed[*]}"
  exit 1
fi
