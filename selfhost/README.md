# The self-hosted pcc

pcc written in Pascal (the dialect in [DIALECT.md](DIALECT.md)); the plan is
[../SELFHOST.md](../SELFHOST.md). It builds with pcc and with Free Pascal:

```sh
build/bin/pcc --mode=tp -I selfhost selfhost/pcc.pas -o build/pcc-self
fpc -Mobjfpc -Sh- -Cr -Co -FUbuild/fpc-units -obuild/pcc-fpc selfhost/pcc.pas
```

| File | Contents |
|------|----------|
| `pcsys.pas` | the only unit calling C: files, output, environment, processes |
| `pcutil.pas` | arena, interned names, lists, maps, buffers, `%g` formatting |
| `pcdiag.pas` | source manager and diagnostics (same output as the C++ pcc) |
| `pclex.pas` | lexer, directives, `{$I}` includes |
| `pcast.pas` | the syntax tree (variant records in the arena) |
| `pcparse.pas` | recursive-descent parser with the C++ parser's error recovery |
| `pcprint.pas` | tree dumps (`--emit-parse-tree`) |
| `tokens.inc` | generated from `include/pcc/Lexer/TokenKinds.def` by `utils/gen-selfhost-tables.py` |
| `pcc.pas` | the driver: options, unit loading, `--emit-tokens`, `--emit-parse-tree` |

Testing:

- `tests/run.sh pcc|fpc`: unit tests (`t_*.pas`).
- `utils/selfhost-diff.py --ref build/bin/pcc --new build/pcc-self tests selfhost`:
  compares output, diagnostics and exit status with the C++ pcc
  (`--action=--emit-parse-tree` for the parser). Add `--all-files <fuzz
  corpus>` for fuzzer inputs.
- `utils/selfhost-lint.py selfhost/*.pas`: dialect rules.
- lit runs all of these: `tests/run/selfhost.test`, `tests/selfhost/*.test`.
