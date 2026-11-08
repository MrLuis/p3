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

With `--mode=objfpc`, everything of `--mode=tp` plus classes, as in Free
Pascal's objfpc mode (`class inherited is as property` become reserved):

```pascal
type
  TShape = class                 { implicitly class(TObject) }
  private
    FName: string;
  public
    constructor Create(const AName: string);
    function Area: real; virtual; abstract;
    procedure Show; virtual;
    property Name: string read FName write FName;
  end;
  TSquare = class(TShape)
    Side: real;
    function Area: real; override;
  end;

function TSquare.Area: real;
begin
  Result := Side * Side;         { Result names the function result }
end;
...
  S := TSquare.Create('square'); { allocates, then runs the constructor }
  if S is TSquare then (S as TSquare).Side := 2;
  S.Show;                        { virtual dispatch }
  S.Free;                        { Destroy (virtual) unless nil, then release }
```

Supported: fields, methods, constructors and destructors (called through
the class they allocate, called on an object they do not), `virtual`,
`override` and `abstract` methods, `inherited` (with or without a method
name and arguments), `Self`, properties with field or method accessors,
`private protected public published` and `strict private`/`strict
protected` (private: visible in the declaring program or unit; protected:
also in descendants' methods; strict: only in the class's (and
descendants') methods), forward declarations (`TNode = class;`), `is`,
`as`, typecasts, `with` on objects, `Result` in functions, and the root
class `TObject` (`Create`, `Destroy`, `Free`, `ClassName`, `ClassType`,
`ClassParent`, `InheritsFrom`, `InstanceSize`). Classes work in units,
compiled whole or separately. Objects live on the heap: a variable of a
class type holds a reference (`nil` by default), and the object starts
with a pointer to its class's VMT. Calls of virtual methods and field
accesses through a `nil` reference are runtime errors under range checks,
as are a failed `as` and calling an abstract method.

- **Properties:** array properties (`property Items[i: integer]: T read
  Get write Put`, several indexes), the default property (`; default;`:
  `list[i]`), indexed properties (`index N`, passed to shared accessors),
  `default V` / `nodefault` (no effect, as without streaming).
- **Class references:** `TFooClass = class of TFoo`, `TClass`; a class
  name is a value; `c.Create(...)` creates an object of the class `c`
  holds, through a virtual constructor if the constructor is virtual.
- **Class methods and variables:** `class procedure/function` (Self is the
  class; may be virtual; called through a class name, a class reference
  or an object), `class var` sections (one variable shared by the class
  and its descendants).
- **Overloading:** routines and methods of one name with different
  parameters (the `overload` directive is optional; on a method it also
  offers the ancestors' overloads). The best fit wins: exact types, then
  wider integer types (`integer` before `int64` before `real`), the
  nearest ancestor class; a tie is an error. Overloads get the symbols
  `<name>$N`.
- **Interfaces:** `IFoo = interface[(IBase)] ['GUID'] methods end`,
  classes listing them (`class(TBase, IFoo, IBar)`, every method
  implemented), interface variables, parameters and results, `obj is
  IFoo`, `obj as IFoo`, comparisons and `Assigned`. A value is {object,
  the object's class's method table}, so virtual implementations dispatch
  as usual. Interfaces are COM style unless `{$interfaces corba}` (as in
  Free Pascal's objfpc mode):
  - **COM interfaces** (`{$interfaces com}`, the default) descend from
    `IUnknown` (= `IInterface`) and are reference counted. Their classes
    descend from `TInterfacedObject` (property `RefCount`). An object is
    freed when the last interface reference to it goes away: when a
    variable or field is assigned or a routine ends (its local variables
    and value parameters; also when an exception leaves it), when an
    object or record holding the reference is freed, or with `dispose`.
    `const` parameters take no reference. While a constructor runs the
    count is 1, so `Self` can be passed as an interface there. A COM
    interface also works as the left operand of `is` / `as` (`intf as
    TFoo`, `intf is IBar`). COM interfaces cannot be in variant parts or
    files, and a function cannot return a record holding them. Interface
    types must be declared in a `type` section.
  - **CORBA interfaces** (`{$interfaces corba}`) are not counted, and any
    class can implement them.

  Differences from Free Pascal: a function's interface result used
  without a variable (`Make.Name`, or a call whose result is discarded)
  is released when the statement ends, while Free Pascal keeps it in a
  hidden temporary until the routine ends; comparing interfaces compares
  their objects; global interface variables are not released at program
  end; `QueryInterface`, `_AddRef` and `_Release` are not declared (the
  compiled code counts references itself).

Not supported (reported as such): class properties, class constructors
and destructors, `stored`, `implements`, properties in interfaces.

Exceptions (`--mode=objfpc`), as in Free Pascal:

```pascal
uses sysutils;                   { built in: Exception is predeclared }
type EParse = class(Exception);
...
try
  try
    if c < '0' then raise EParse.Create('not a digit');
  finally
    writeln('always');           { also after exit, break and continue }
  end;
except
  on E: EParse do writeln(E.ClassName, ': ', E.Message);
  on E: Exception do raise;      { re-raise }
else
  writeln('anything else');
end;
```

Any object can be raised. Handlers are tried in order; with none matching
(and no `else`), the exception goes on to the next `try`. The exception
object is destroyed (`Destroy`, then released) when its handler ends,
including through `exit`, `break` or `continue`, and when a new exception
leaves the handler; a constructor that raises destroys its new object.
`Exception` has `Create(msg)` and the property `Message`. Integer `div` and
`mod` by zero raise `EDivByZero` (message `Division by zero`; its ancestors
`EIntError` and `EExternal` are predeclared too), as in Free Pascal with
SysUtils; other runtime errors (range and overflow checks, real division by
zero, ...) are not exceptions. Following Free Pascal, `exit`, and
`break`/`continue` out of a `finally` part, are errors, and so are labels
and `goto` in `--mode=objfpc` (Free Pascal without `-Sg`). An unhandled
exception is a runtime error that names the class, where it was raised and
the message (`Runtime error: unhandled exception EParse at p.pas:12:5: not
a digit`); `finally` parts run first. Exceptions are `_setjmp`/`_longjmp`
based: each `try` statement pushes a frame on a stack kept by the runtime.

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

**Conditional compilation** (every mode), as in Free Pascal:

```pascal
{$DEFINE TRACE}                  { also -d TRACE / -dTRACE on the command line }
{$IFDEF TRACE} writeln('enter'); {$ENDIF}
{$IF defined(PCC_OBJFPC) and not defined(SLOW)}
  ...
{$ELSEIF defined(PCC_TP)}
  ...
{$ELSE}
  ...
{$ENDIF}                         { or $IFEND }
```

`$DEFINE`, `$UNDEF`, `$IFDEF`, `$IFNDEF`, `$IF` / `$ELSEIF` (expressions of
`defined(NAME)`, `true`, `false`, `not`, `and`, `or` and parentheses),
`$ELSE`, `$ENDIF` and `$IFEND`; symbols are case-insensitive. `PCC` is
predefined, and so is one of `PCC_ISO`, `PCC_TP` and `PCC_OBJFPC` (the
mode). Each file (program or unit) starts with only these and the `-d`
symbols; an include file shares its includer's symbols, and a conditional
must end in the file that starts it. The text of a branch that is not
compiled is only scanned for comments, strings and conditional
directives. Macros (`{$DEFINE X := 1}`), `$IFOPT` and other `$IF`
expressions are not supported.

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
pcc --help                    # all options (-O0..3, -g, --target, --mode=iso|tp|objfpc, ...)
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
