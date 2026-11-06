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
| `pcir.pas` | a builder for textual LLVM IR that prints exactly as LLVM 19 prints the C++ pcc's module: constant folding, names, block order, alignments, metadata |
| `pcgen.pas`, `pcgenstmt.inc`, `pcgendbg.inc` | code generation and debug information (CodeGen.cpp) |
| `pcdriver.pas` | the back end of the driver: clang for `-S`, `-c` and `-O`, linking with the runtime, `--target`, `--version` |
| `version.inc` | generated from `CMakeLists.txt` |
| `pcc.pas` | the driver: options (as llvm::cl parses them), unit loading, the dumps, code generation |

The Pascal compiler writes LLVM IR and runs clang to optimize, assemble and
compile it (`$PCC_CLANG`, else `clang-19` or `clang` on `$PATH`), then links
like the C++ pcc (`$PCC_CC`, else clang-19, clang, cc or gcc) with
`$PCC_RUNTIME_LIB`, else `../lib/libpascalrt.a` next to its executable. Its
data layout is the target's, as clang reports it. Its `--emit-llvm` output is
byte-identical to the C++ pcc's (SELFHOST.md S10).

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
  lit build/tests`: the whole suite against the Pascal compiler (all pass);
  `utils/iso7185pat.py build/pcc-self` runs the ISO conformance test
  (793/793).
- IR parity: `utils/gen-pascal.py --mode iso|tp --count N --seed S --out DIR`
  writes random valid programs; `utils/selfhost-diff.py --ref build/bin/pcc
  --new build/pcc-self --action=--emit-llvm --extra="-g -o -" --modes=iso DIR`
  must report 0 differences (also without `-g`, and on the repository and
  the fuzz corpora). A Pascal compiler built at `-O0` needs a large stack for
  deeply nested sources (`ulimit -s unlimited`).
- The bootstrap: `ninja -C build check-bootstrap` runs `utils/bootstrap.py`,
  which builds stage 1 (by pcc), stage 2 (by stage 1) and stage 3 (by stage
  2) into `build/bootstrap/stageN/{bin,lib}` and checks that stages 1-3
  write the same IR for `pcc.pas`, that stages 2 and 3 write the same IR
  for every `.pas` file of the repository in both modes, that the stage 2
  and stage 3 executables are identical, and that stage 3 builds a working
  program; with `fpc` installed also that a stage 1 built by Free Pascal
  leads to the same IR. `utils/selfhost-diff.py --ref A --new B
  --action=--emit-llvm --extra="-o -" ...` compares any two compilers' IR
  on more inputs (fuzz corpora).
- lit runs all of these: `tests/run/selfhost.test`, `tests/selfhost/*.test`.
