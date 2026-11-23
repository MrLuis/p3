# JSON benchmark

Unit `json` (rtl/json.pas) on the three files JSON libraries are usually
measured with — `twitter.json` (strings, Unicode), `citm_catalog.json`
(structure, integers), `canada.json` (floating-point numbers) — next to the
same operations with Rust's serde_json and sonic-rs, which unit `json`
follows.

```sh
bench/json/run.sh          # fetches data/, builds, runs both (cargo: optional)
bench/json/run.sh 3        # 3 seconds per operation
```

Each operation runs until its time is up (at least 3 times); the best run
counts, as MB/s of the file:

| Operation | p3 (unit json) | Rust |
|---|---|---|
| read tokens | `TJsonReader`: every token, strings decoded, numbers converted | — |
| parse dom | `JsonParse` | `serde_json::Value`, `sonic_rs::Value` |
| write dom | `ToJson` of the parsed document | `to_string` |
| lazy get | `JsonGet(S, [...])` of a value near the end | `sonic_rs::get` |
| typed read | `TFeatureCollection.FromJson` (the json generator, canadatypes.pas) | serde derive, `from_str` |

## Results

Apple M5, macOS; p3 at -O2, Rust 1.100 release with LTO; MB/s (higher is
better).

| | p3 | serde_json | sonic-rs |
|---|---:|---:|---:|
| twitter: parse dom | 644 | 421 | 1795 |
| twitter: write dom | 1556 | 1408 | 2641 |
| twitter: lazy get | 1815 | — | 1320 |
| citm_catalog: parse dom | 682 | 561 | 2248 |
| citm_catalog: write dom | 2071 | 3128 | 3342 |
| citm_catalog: lazy get | 1078 | — | 1699 |
| canada: parse dom | 292 | 338 | 1036 |
| canada: write dom | 1044 | 837 | 815 |
| canada: lazy get | 1174 | — | 1126 |
| canada: typed read | 308 | 474 | 527 |

Reading tokens alone: twitter 478, citm_catalog 797, canada 488 MB/s.

What this says: on strings and structure unit `json` parses faster than
serde_json and about a third as fast as sonic-rs, whose parser uses SIMD
(p3 scans 8 bytes at a time in general-purpose registers); writing is in
the same range as both; lazy access is like sonic-rs's (both skip what is
not asked for). Numbers are the weak side: each float is converted when
read (Eisel-Lemire, exact), but the DOM and the generated code add work
per number that serde and sonic do not have, so canada.json is below both.
A document's nodes also keep strings in the source text, so the DOM is
cheap to build and write back but each string is decoded when read.

## Files

| File | |
|---|---|
| `jsonbench.pas` | the p3 benchmark |
| `canadatypes.pas` | canada.json as `[Json]` records |
| `rust/` | the same operations with serde_json and sonic-rs |
| `fetch-data.sh` | downloads `data/` (not in the repository) |
| `run.sh` | builds and runs both |
