# pcc — a Pascal compiler targeting LLVM

`pcc` compiles Pascal (ISO 7185, with optional Turbo/Free Pascal extensions)
to native executables through LLVM. See [PLAN.md](PLAN.md) for the roadmap.

**Status:** M4 complete: integer/boolean/char variables, full ISO expression
syntax and type checking, `read`/`readln`, `write` field widths, and the
standard functions `abs sqr odd ord chr succ pred`. Next up is M5 (`if`,
`while`, `repeat`, `for`, `case`).

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
