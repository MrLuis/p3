# pcc — a Pascal compiler targeting LLVM

`pcc` compiles Pascal (ISO 7185, with optional Turbo/Free Pascal extensions)
to native executables through LLVM. See [PLAN.md](PLAN.md) for the roadmap.

**Status:** M11 complete (full ISO 7185 level 0). Supported:

- procedures and functions: value and `var` parameters, recursion, `forward`,
  nested routines with access to enclosing variables
- `const` and `type` declarations; `integer`, `real`, `boolean`, `char`,
  enumerations, subranges, arrays (multi-dimensional), records (with variant
  parts), strings (`packed array [1..n] of char`), `with`
- all statements except `goto`; full ISO expressions and type checking
- `read`/`readln`/`write`/`writeln` with field widths; the standard functions
  `abs sqr odd ord chr succ pred sin cos exp ln sqrt arctan trunc round`
- runtime checks for ranges, array bounds and integer overflow (on by
  default; `-fno-range-checks`, `-fno-overflow-checks`, or `{$R-}` / `{$Q-}`
  in the source)
- pointers, sets, files (text and typed; program parameters bind to
  command-line arguments), `goto`, procedural parameters, `pack`/`unpack`
- `-g` debug info (gdb/lldb), "did you mean" diagnostics, cross-compiling
  objects with `--target`

With `--mode=tp`, Turbo/Free Pascal extensions: units (`uses`, `-I`),
`string`/`string[N]` and string routines, `break continue exit(value) inc
dec`, `shl shr xor`, `$hex %bin #13` literals, `//` comments; the integer
types `longint int64 cardinal byte word shortint smallint` (FPC widening
rules); typecasts `T(x)`; `pointer`, `pchar`, `@`, `getmem freemem
reallocmem fillchar move sizeof low high assigned`; `const` parameters;
procedural types; typed constants including arrays and records; external C
functions (`cdecl; external 'c' name 'strlen'`); `{$I file}`; `paramcount
paramstr halt`. This is the dialect the self-hosted compiler is written in
(see [SELFHOST.md](SELFHOST.md) and [selfhost/DIALECT.md](selfhost/DIALECT.md)).

Units are compiled into the program by default. They can also be compiled
separately: `pcc --mode=tp -c unit.pas` writes `unit.o` (its interface
variables and routines are the symbols `pas.<unit>$<name>`, its
initialization part `pas.<unit>$$init`), and a program built with
`-fseparate-units` uses those objects (from next to each unit's source)
instead of compiling the units again:

```sh
pcc --mode=tp -c lib/mathutils.pas && pcc --mode=tp -I lib -c shapes.pas
pcc --mode=tp -I lib -fseparate-units main.pas -o main
```

`-L dir` and `-l name` pass library options to the linker.

**Self-hosting:** pcc is also written in Pascal ([selfhost/](selfhost/),
[SELFHOST.md](SELFHOST.md)); `ninja -C build check-bootstrap` builds it with
itself, and `utils/seed.py` makes and uses a stage-1 *seed* (a binary plus
the compiler's LLVM IR) to bootstrap it without this C++ compiler.

**ISO 7185 conformance:** the Pascal-P5 test `iso7185pat.pas` passes all 793
checks (`utils/iso7185pat.py build/bin/pcc`).

## Fuzzing

```sh
cmake -S . -B build-fuzz -G Ninja -DPCC_ENABLE_FUZZING=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined"
ninja -C build-fuzz pcc-fuzz
build-fuzz/bin/pcc-fuzz -max_total_time=300 corpus/ tests/run tests/*/Inputs
```

```pascal
program Sum(input, output);
var a, b: integer;
begin
  read(a, b);
  writeln(a, ' + ', b, ' = ', a + b:1);
  writeln('even? ', not odd(a + b))
end.
```

```sh
$ pcc sum.pas && echo "17 25" | ./sum
17 + 25 = 42
even? true
```

## Requirements

- LLVM 19 (dev headers + `FileCheck`), e.g. Debian/Ubuntu:
  `llvm-19-dev llvm-19-tools clang-19`; macOS: `brew install llvm@19`
- CMake ≥ 3.20, Ninja, a C++20 compiler
- `lit` (`pip install lit`)
- At run time, a C compiler driver (`clang` or `cc`) is used as the linker;
  override with `PCC_CC`. The runtime library is found at
  `<pcc dir>/../lib/libpascalrt.a` (override with `PCC_RUNTIME_LIB`).

## Build & test

```sh
cmake -S . -B build -G Ninja            # add -DLLVM_DIR=<prefix>/lib/cmake/llvm if not found
ninja -C build                          # -> build/bin/pcc
ninja -C build check                    # run the lit test suite
```

## Usage

```sh
pcc hello.pas -o hello        # executable (default)
pcc -c hello.pas              # object file
pcc -S hello.pas              # assembly
pcc --emit-llvm hello.pas     # LLVM IR
pcc --emit-tokens hello.pas   # lexer dump
pcc --emit-ast hello.pas      # AST dump
pcc --help                    # all options (-O0..3, -g, --target, --mode=iso|tp, ...)
```

## Layout

| Path | Contents |
|---|---|
| `src/basic/` | source manager, diagnostics |
| `src/lexer/` | tokens (`TokenKinds.def`) and lexer |
| `src/ast/`, `src/parse/` | AST nodes (`ASTNodes.def`), printer, recursive-descent parser |
| `src/sema/` | semantic checks |
| `src/codegen/` | AST → LLVM IR, optimization, object/asm emission |
| `src/driver/` | command line, pipeline, linking |
| `runtime/` | `libpascalrt` C runtime (ABI in `runtime/include/pascalrt.h`) |
| `tests/` | lit tests: `lexer/ parser/ sema/ codegen/ driver/`, `run/` (compile + execute + diff), C++ unit tests in `unit/` |

## Writing tests

Tests are lit files with `RUN:` lines; `%pcc` is the compiler under test and
`FileCheck`/`not` come from LLVM. Example:

```
; RUN: not %pcc %S/Inputs/bad.pas 2>&1 | FileCheck %s
; CHECK: bad.pas:3:5: error: undeclared identifier 'x'
```

C++ unit tests use the tiny `TEST`/`EXPECT_*` macros in `tests/unit/UnitTest.h`.
