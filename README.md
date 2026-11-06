# pcc — a Pascal compiler targeting LLVM

`pcc` compiles Pascal (ISO 7185, with optional Turbo/Free Pascal extensions)
to native executables through LLVM. See [PLAN.md](PLAN.md) for the roadmap.

**Status:** M1 (lexer) complete: `pcc --emit-tokens` tokenizes ISO Pascal
(plus Turbo/FPC keywords with `--mode=tp`). The parser (M2) is next.

## Requirements

- LLVM 19 (dev headers + `FileCheck`), e.g. Debian/Ubuntu:
  `llvm-19-dev llvm-19-tools clang-19`; macOS: `brew install llvm@19`
- CMake ≥ 3.20, Ninja, a C++20 compiler
- `lit` (`pip install lit`)

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
| `include/pcc/`, `src/` | compiler sources (`basic/` = source manager & diagnostics, `driver/` = CLI) |
| `runtime/` | `libpascalrt` C runtime (from M3) |
| `tests/` | lit tests (`*.test`, `*.pas`) and C++ unit tests (`tests/unit/`) |

## Writing tests

Tests are lit files with `RUN:` lines; `%pcc` is the compiler under test and
`FileCheck`/`not` come from LLVM. Example:

```
; RUN: not %pcc %S/Inputs/bad.pas 2>&1 | FileCheck %s
; CHECK: bad.pas:3:5: error: undeclared identifier 'x'
```

C++ unit tests use the tiny `TEST`/`EXPECT_*` macros in `tests/unit/UnitTest.h`.
