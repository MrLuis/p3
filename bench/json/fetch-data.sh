#!/bin/sh
# Downloads the three JSON benchmark files (from serde-rs/json-benchmark,
# as nativejson-benchmark has them) into data/.
set -e
cd "$(dirname "$0")"
mkdir -p data
for f in twitter citm_catalog canada; do
  [ -f data/$f.json ] || curl -sSfL -o data/$f.json \
    https://raw.githubusercontent.com/serde-rs/json-benchmark/master/data/$f.json
done
