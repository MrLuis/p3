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
| `pcprint.pas` | tree dumps (`--emit-ast`, `--emit-parse-tree`) |
| `pctypes.pas` | semantic types: constructors, predicates, names |
| `pcscope.pas` | name scopes (with scopes, unit imports) |
| `pcsema.pas`, `pcsemastmt.inc` | semantic analysis (Sema.cpp) |
| `builtins.inc` | generated from `include/pcc/AST/Builtins.def` |
| `tokens.inc` | generated from `include/pcc/Lexer/TokenKinds.def` by `utils/gen-selfhost-tables.py` |
| `pcir.pas` | a builder for textual LLVM IR (types with 64-bit layout, values, instructions) |
| `pcgen.pas`, `pcgenstmt.inc` | code generation (CodeGen.cpp): `--emit-llvm` |
| `pcc.pas` | the driver: options, unit loading, `--emit-tokens`, `--emit-parse-tree`, `--emit-ast`, `--emit-llvm` |

Testing:

- `tests/run.sh pcc|fpc`: unit tests (`t_*.pas`).
- `utils/selfhost-diff.py --ref build/bin/pcc --new build/pcc-self tests selfhost`:
  compares output, diagnostics and exit status with the C++ pcc
  (`--action=--emit-parse-tree` for the parser). Add `--all-files <fuzz
  corpus>` for fuzzer inputs.
- `utils/selfhost-lint.py selfhost/*.pas`: dialect rules.
- `PCC_UNDER_TEST=build/pcc-self lit -sv build/tests/sema`: the C++ test
  suite run against the Pascal compiler (sema, lexer and parser pass).
- Until the Pascal compiler has its own driver (S8), `utils/selfhost-cc.py`
  is a pcc front for it: it runs `--emit-llvm`, checks the IR with
  `opt -passes=verify` and lets clang assemble and link it with the runtime.
  `SELFHOST_PCC=build/pcc-self PCC_UNDER_TEST=$PWD/utils/selfhost-cc.py lit
  build/tests/run` runs the run tests (all pass), and
  `SELFHOST_PCC=build/pcc-self utils/iso7185pat.py $PWD/utils/selfhost-cc.py`
  the ISO conformance test (793/793).
- The bootstrap: `build/pcc-self --mode=tp -I selfhost selfhost/pcc.pas
  --emit-llvm -o stage2.ll`, link it into stage 2 (as above), and stage 2
  must write the same IR for its own source. The pcc-built, FPC-built and
  stage 2 compilers must write identical IR for every input:
  `utils/selfhost-diff.py --ref build/pcc-self --new build/pcc-fpc
  --action=--emit-llvm --extra="-o -" ...`.
- lit runs all of these: `tests/run/selfhost.test`, `tests/selfhost/*.test`.
