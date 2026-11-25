# The self-hosted p3

p3 written in Pascal (the dialect in [DIALECT.md](DIALECT.md)); the plan is
[../docs/SELFHOST.md](../docs/SELFHOST.md). It links with libLLVM (its back end calls
the LLVM-C API), so it needs two things from the CMake build: the target
initializers of that LLVM (`build/selfhost/llvmtargets.inc`, written by
`utils/gen-llvm-targets.py`) and the link options, both in
`build/selfhost/flags.txt` (e.g. `-I build/selfhost -L/usr/lib/llvm-23/lib
-lLLVM-23`). It builds with p3 and with Free Pascal:

```sh
build/bin/p3 --mode=tp $(cat build/selfhost/flags.txt) -I selfhost selfhost/p3.pas -o build/p3-self
fpc -Mobjfpc -Sh- -Cr -Co -FUbuild/fpc-units -Fibuild/selfhost \
    -k-L/usr/lib/llvm-23/lib -k-lLLVM-23 -obuild/p3-fpc selfhost/p3.pas
```

| File | Contents |
|------|----------|
| `pcsys.pas` | the only unit calling C: files, output, environment, processes |
| `pcutil.pas` | arena, interned names, lists, maps, buffers, `%g` formatting |
| `pcdiag.pas` | source manager and diagnostics (same output as the C++ p3) |
| `pclex.pas` | lexer, directives, `{$I}` includes, conditional compilation |
| `pcast.pas` | the syntax tree (variant records in the arena) |
| `pcparse.pas` | recursive-descent parser with the C++ parser's error recovery |
| `pcprint.pas` | tree dumps (`--emit-ast`, `--emit-parse-tree`) |
| `pctypes.pas` | semantic types: constructors, predicates, names |
| `pcscope.pas` | name scopes (with scopes, unit imports) |
| `pcsema.pas`, `pcsemastmt.inc` | semantic analysis (Sema.cpp) |
| `builtins.inc` | generated from `include/p3/AST/Builtins.def` |
| `tokens.inc` | generated from `include/p3/Lexer/TokenKinds.def` by `utils/gen-selfhost-tables.py` |
| `pcir.pas` | a builder for textual LLVM IR that prints exactly as LLVM 23 prints the C++ p3's module: constant folding, names, block order, alignments, metadata |
| `pcgen.pas`, `pcgenstmt.inc`, `pcgendbg.inc` | code generation and debug information (CodeGen.cpp) |
| `pcdriver.pas` | the back end of the driver: clang for `-S`, `-c` and `-O`, linking with the runtime, `--target`, `--version` |
| `pcllvm.pas` | the LLVM-C API: parse, verify, optimize and emit in process (Backend.cpp) |
| `version.inc` | generated from `CMakeLists.txt` |
| `p3.pas` | the driver: options (as llvm::cl parses them), unit loading, the dumps, code generation |

It accepts the same languages as the C++ p3, including classes and
exceptions with `--mode=objfpc` (PLAN.md M13, M14), though it is itself
written without them (DIALECT.md).

The Pascal compiler writes LLVM IR as text, then libLLVM parses, verifies,
optimizes (the same `default<On>` pipeline as the C++ p3) and compiles it
in process, with a target machine set up like the C++ p3's; the default
triple, data layout, host CPU and target checks come from LLVM too. It
links like the C++ p3 (`$P3_CC`, else clang-23, clang, cc or gcc) with
`$P3_RUNTIME_LIB`, else `../lib/libpascalrt.a` next to its executable; `-L`
and `-l` add libraries. The unit library is `$P3_UNIT_DIR`, else
`../lib/pascal` next to its executable. Its `--emit-llvm` output at `-O0` is byte-identical
to the C++ p3's (SELFHOST.md S10), and so are `-S` and `-c` at `-O0`. At
`-O1`-`-O3` the code can differ slightly: the C++ p3 optimizes the module it
built in memory, the Pascal compiler one parsed from text, whose use lists
are in another order (which a few passes depend on) and whose name counters
start at zero.

Testing:

- `tests/run.sh p3|fpc`: unit tests (`t_*.pas`).
- `utils/selfhost-diff.py --ref build/bin/p3 --new build/p3-self tests selfhost`:
  compares output, diagnostics and exit status with the C++ p3
  (`--action=--emit-parse-tree` for the parser). Add `--all-files <fuzz
  corpus>` for fuzzer inputs.
- `utils/selfhost-lint.py selfhost/*.pas`: dialect rules.
- `P3_UNDER_TEST=build/p3-self lit -sv build/tests/sema`: the C++ test
  suite run against the Pascal compiler (sema, lexer and parser pass).
- `P3_RUNTIME_LIB=$PWD/build/lib/libpascalrt.a P3_UNDER_TEST=build/p3-self
  lit build/tests`: the whole suite against the Pascal compiler (all pass);
  `utils/iso7185pat.py build/p3-self` runs the ISO conformance test
  (793/793).
- IR parity: `utils/gen-pascal.py --mode iso|tp --count N --seed S --out DIR`
  writes random valid programs; `utils/selfhost-diff.py --ref build/bin/p3
  --new build/p3-self --action=--emit-llvm --extra="-g -o -" --modes=iso DIR`
  must report 0 differences (also without `-g`, and on the repository and
  the fuzz corpora). A Pascal compiler built at `-O0` needs a large stack for
  deeply nested sources (`ulimit -s unlimited`).
- The bootstrap: `ninja -C build check-bootstrap` runs `utils/bootstrap.py`,
  which builds stage 1 (by p3), stage 2 (by stage 1) and stage 3 (by stage
  2) into `build/bootstrap/stageN/{bin,lib}` and checks that stages 1-3
  write the same IR for `p3.pas`, that stages 2 and 3 write the same IR
  for every `.pas` file of the repository in both modes, that the stage 2
  and stage 3 executables are identical, and that stage 3 builds a working
  program; with `fpc` installed also that a stage 1 built by Free Pascal
  leads to the same IR. `utils/selfhost-diff.py --ref A --new B
  --action=--emit-llvm --extra="-o -" ...` compares any two compilers' IR
  on more inputs (fuzz corpora).
- The stage-1 seed: `ninja -C build seed` writes `build/seed/p3-seed-*`;
  `utils/seed.py bootstrap --seed <seed> [--from ir]` bootstraps from it
  without the C++ p3 (needs `cc`, libLLVM 23 and `llvm-config`, and clang
  for `--from ir`). See SELFHOST.md S11.
- Separate units: `utils/bootstrap.py --separate` also builds the compiler
  from its separately compiled units (each unit with `-c`, then `p3.pas`
  with `-fseparate-units`; objects are written next to the sources, so it
  works on a copy of `selfhost/`).
- lit runs all of these: `tests/run/selfhost.test`, `tests/selfhost/*.test`.
