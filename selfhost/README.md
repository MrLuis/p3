# The self-hosted pcc

pcc written in Pascal (the dialect in [DIALECT.md](DIALECT.md)); the plan is
[../SELFHOST.md](../SELFHOST.md). It links with libLLVM (its back end calls
the LLVM-C API), so it needs two things from the CMake build: the target
initializers of that LLVM (`build/selfhost/llvmtargets.inc`, written by
`utils/gen-llvm-targets.py`) and the link options, both in
`build/selfhost/flags.txt` (e.g. `-I build/selfhost -L/usr/lib/llvm-19/lib
-lLLVM-19`). It builds with pcc and with Free Pascal:

```sh
build/bin/pcc --mode=tp $(cat build/selfhost/flags.txt) -I selfhost selfhost/pcc.pas -o build/pcc-self
fpc -Mobjfpc -Sh- -Cr -Co -FUbuild/fpc-units -Fibuild/selfhost \
    -k-L/usr/lib/llvm-19/lib -k-lLLVM-19 -obuild/pcc-fpc selfhost/pcc.pas
```

| File | Contents |
|------|----------|
| `pcsys.pas` | the only unit calling C: files, output, environment, processes |
| `pcutil.pas` | arena, interned names, lists, maps, buffers, `%g` formatting |
| `pcdiag.pas` | source manager and diagnostics (same output as the C++ pcc) |
| `pclex.pas` | lexer, directives, `{$I}` includes, conditional compilation |
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
| `pcllvm.pas` | the LLVM-C API: parse, verify, optimize and emit in process (Backend.cpp) |
| `version.inc` | generated from `CMakeLists.txt` |
| `pcc.pas` | the driver: options (as llvm::cl parses them), unit loading, the dumps, code generation |

It accepts the same languages as the C++ pcc, including classes and
exceptions with `--mode=objfpc` (PLAN.md M13, M14), though it is itself
written without them (DIALECT.md).

The Pascal compiler writes LLVM IR as text, then libLLVM parses, verifies,
optimizes (the same `default<On>` pipeline as the C++ pcc) and compiles it
in process, with a target machine set up like the C++ pcc's; the default
triple, data layout, host CPU and target checks come from LLVM too. It
links like the C++ pcc (`$PCC_CC`, else clang-19, clang, cc or gcc) with
`$PCC_RUNTIME_LIB`, else `../lib/libpascalrt.a` next to its executable; `-L`
and `-l` add libraries. The unit library is `$PCC_UNIT_DIR`, else
`../lib/pascal` next to its executable. Its `--emit-llvm` output at `-O0` is byte-identical
to the C++ pcc's (SELFHOST.md S10), and so are `-S` and `-c` at `-O0`. At
`-O1`-`-O3` the code can differ slightly: the C++ pcc optimizes the module it
built in memory, the Pascal compiler one parsed from text, whose use lists
are in another order (which a few passes depend on) and whose name counters
start at zero.

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
- The stage-1 seed: `ninja -C build seed` writes `build/seed/pcc-seed-*`;
  `utils/seed.py bootstrap --seed <seed> [--from ir]` bootstraps from it
  without the C++ pcc (needs `cc`, libLLVM 19 and `llvm-config`, and clang
  for `--from ir`). See SELFHOST.md S11.
- Separate units: `utils/bootstrap.py --separate` also builds the compiler
  from its separately compiled units (each unit with `-c`, then `pcc.pas`
  with `-fseparate-units`; objects are written next to the sources, so it
  works on a copy of `selfhost/`).
- lit runs all of these: `tests/run/selfhost.test`, `tests/selfhost/*.test`.
