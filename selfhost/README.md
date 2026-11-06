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
| `pcdriver.pas` | the back end of the driver: clang for `-S`, `-c` and `-O`, linking with the runtime, `--target`, `--version` |
| `version.inc` | generated from `CMakeLists.txt` |
| `pcc.pas` | the driver: options (as llvm::cl parses them), unit loading, the dumps, code generation |

The Pascal compiler writes LLVM IR and runs clang to optimize, assemble and
compile it (`$PCC_CLANG`, else `clang-19` or `clang` on `$PATH`), then links
like the C++ pcc (`$PCC_CC`, else clang-19, clang, cc or gcc) with
`$PCC_RUNTIME_LIB`, else `../lib/libpascalrt.a` next to its executable. Its
data layout is that of 64-bit targets. `-g` is accepted but produces no
debug information yet.

Testing:

- `tests/run.sh pcc|fpc`: unit tests (`t_*.pas`).
- `utils/selfhost-diff.py --ref build/bin/pcc --new build/pcc-self tests selfhost`:
  compares output, diagnostics and exit status with the C++ pcc
  (`--action=--emit-parse-tree` for the parser). Add `--all-files <fuzz
  corpus>` for fuzzer inputs.
- `utils/selfhost-lint.py selfhost/*.pas`: dialect rules.
- `PCC_UNDER_TEST=build/pcc-self lit -sv build/tests/sema`: the C++ test
  suite run against the Pascal compiler (sema, lexer and parser pass).
- `PCC_RUNTIME_LIB=$PWD/build/lib/libpascalrt.a PCC_UNDER_TEST=build/pcc-self
  lit build/tests`: the whole suite against the Pascal compiler (all pass but
  the `codegen/` tests that match IR text, see SELFHOST.md S10, and debug
  information); `utils/iso7185pat.py build/pcc-self` runs the ISO
  conformance test (793/793).
- The bootstrap: `build/pcc-self --mode=tp -I selfhost selfhost/pcc.pas -o
  stage2` builds stage 2, which must write the same `--emit-llvm` output for
  its own source as `build/pcc-self`. The pcc-built, FPC-built and
  stage 2 compilers must write identical IR for every input:
  `utils/selfhost-diff.py --ref build/pcc-self --new build/pcc-fpc
  --action=--emit-llvm --extra="-o -" ...`.
- lit runs all of these: `tests/run/selfhost.test`, `tests/selfhost/*.test`.
