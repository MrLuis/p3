# json demo

`jsontool`, a small command on p3's **`json` unit** (after Rust's
[sonic-rs](https://github.com/cloudwego/sonic-rs)): a document parsed and
written back, one value found without parsing the rest, errors with their
line and column, and records read and written by code that the library's
json generator writes at build time (no run-time type information).

```sh
cd examples/json
../p3                                   # builds bin/jsontool
bin/jsontool team data/team.json        # typed: the records of team.json
bin/jsontool get data/team.json /members/1/name
bin/jsontool pretty data/team.json
bin/jsontool stats data/team.json
bin/jsontool check broken.json          # broken.json:2:5: trailing comma
```

```
compilers: 3 members, budget 125000.50
  Ada (Lead, since 2019) parsing LLVM
  Niklaus (Senior, since 2021) Pascal
  Grace (Junior, since 2025)
{"name":"Grace","level":"Senior","started":2025,"skills":[]}
```

## What it shows

- `JsonParse(Text)` and `D.ToJson(Pretty)` — `pretty`, `compact`, `check`:
  a document, and its text back (strings and numbers as they were written).
- `JsonGetPath(Text, '/members/1/name')` — `get`: one value, reading only
  the text before it.
- `for E in JsonItems(V)`, `V.Kind` — `stats`: a walk over a document.
- `[Json]` on the types of `src/lib/team.pas`, with `"generators": ["json"]`
  in `p3.json` — `team`: `TTeam.FromJson(Text)` reads straight into the
  records (`T.Members[0].Level`, an enumeration read by its name),
  `M.ToJson` writes one back. `[JsonName('started')]` names a field in the
  JSON, `[Skip]` leaves one out. The JSON keys are the fields' names as
  spelled in the record (`name`), while code can write `M.Name`.
- `EJsonError` — its `Line`, `Column` and `Reason`.

The generated code is in `gen/` (`team.intf.inc`, `team.impl.inc`).
