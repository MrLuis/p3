# p3 — a modern implementation of a Pascal compiler

Why another pascal compiler? Well it I had some extra tokens to spare... actually the 
story is a bit larger than that, but boring neverthe less. Antway, I'm not thinking about
replace Delphi, nor Free Pascal. I was more looking towards server side coding, with
a bit more modern features that current compilers might not care to implement. You will 
be able to see ownership management (basic implementation of rust owner/borrow), SIMD
as a first class feature, better Lamda implementations, and so on... We are even moving
towards to memory safe implementations (althou there is a lot of work there). We would like 
to also support more platform, right now is Linux, MacOS, and Win64... LLVM does provide
more way to extent to other platform, however more work need to be done in that direction.

`p3` compiles Pascal to native executables through LLVM. It starts from
ISO 7185 and adds Turbo/Free Pascal extensions (units, strings, classes,
exceptions, ...) plus a few features of its own (null-safe references,
owned variables, template strings, iterators with `yield`, lazy sequences).

For quality control (and a bit of self insecurity), there are two compilers, kept in parity:

- **C++ `p3`** (`src/`, `include/`): the original compiler, built with CMake.
- **Self-hosted `p3`** (`selfhost/`): the same compiler written in Pascal,
  which can compile itself.

## Building and installing

- **From source:** CMake and LLVM 23; [docs/build.md](docs/build.md) has the
  requirements, the steps for Ubuntu, macOS and Windows, installing, and
  running the tests. In short:

  ```sh
  cmake -G Ninja -S . -B build
  ninja -C build
  ```

- **Homebrew** (macOS ARM64, Linux x86-64): `brew install mrluis/p3/p3`
  ([the tap](https://github.com/MrLuis/homebrew-p3)).
- **Release archives** for Linux x86-64 and ARM64 and macOS ARM64, with
  LLVM linked in: [Releases](https://github.com/MrLuis/p3/releases).
- **Windows** (x86-64, Windows 10 and later): it needs nothing else
  installed, because it links programs itself (lld, with the MinGW-w64
  libraries it carries). Any of:
  - **Scoop** ([the bucket](https://github.com/MrLuis/scoop-p3)):

    ```powershell
    scoop bucket add p3 https://github.com/MrLuis/scoop-p3
    scoop install p3/p3
    ```

  - **winget** (`MrLuis.p3`; submitted to winget-pkgs, pending review):

    ```powershell
    winget install MrLuis.p3
    ```

  - **The zip**: the release's `p3-<version>-windows-x86_64.zip`. Unzip it
    anywhere and run `bin\p3.exe` (or put that `bin` directory on `PATH`).

  A Windows program built by p3 contains the MinGW-w64 runtime and
  winpthreads: distribute it with their notices
  ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md#programs-for-windows)).

Homebrew's p3 and the Linux and macOS archives need a C compiler on the
machine to link programs.

## Usage

```sh
build/bin/p3 hello.pas -o hello          # ISO 7185
build/bin/p3 --mode=tp hello.pas         # Turbo/Free Pascal extensions
build/bin/p3 --mode=objfpc hello.pas     # plus classes, exceptions, ...
```

See
[docs/compiler-params-and-directives.md](docs/compiler-params-and-directives.md)
for the full list of modes, options and directives.

## Features

Beyond ISO 7185 and the Turbo/Free Pascal extensions, p3 has features of its
own. They need `--mode=objfpc` unless noted; each works in both compilers
(most come from the "Modernizing Pascal" proposals). The details and
examples are in the reference.

- **Variables among the statements**: `var x := e;` anywhere in a block, and
  `for var i := 1 to n do`.
- **Type inference**: `var x := e`, `const x := e`, generic calls, array
  literals and lambda parameter types are worked out by the compiler.
- **Owned variables and `move`**: `x: TFoo owned` frees its object when the
  scope ends (also on exceptions); `move x` hands it to someone else, checked
  at compile time.
- **Template strings**: `` `Hello ${Name}, ${Total:0:2}` `` spans lines and
  takes values; routines can tag a template.
- **Tuples and destructuring**: several values as one, returned from a
  routine and unpacked on assignment. `function test(const inData:string) : (val1: string; val2: integer);`
- **Named arguments and defaults anywhere**: `Open(Path, Mode := fmRead)`.
- **Lambdas and closures**: `function(x) => x * 2`, block bodies, capture by
  copy (`function[k](x) => ...`), closures that outlive their routine.
- **Conditional expressions**: `if c then a else b` as an expression.
- **Generics**: types, routines, methods, type helpers, inheritance, and
  inference through specializations.
- **Checked arithmetic**: `checked ... end` and `unchecked ... end`
  blocks (overflow raises `EIntOverflow`; unchecked wraps), plus
  `checked(e)` / `unchecked(e)` expressions and checked conversions.
- **async/await and structured concurrency**: real threads, `scope` /
  `spawn`, cancellation tokens and channels.
- **Compile-time data-race checks**: `shared` / `isolated` qualifiers, opt-in
  with `{$modeswitch racechecks}`.
- **Null-safe references & pointers**: with `{$nullsafe on}` (classes/interfaces in
  `objfpc`), `T?`, `?.` and `??`, plus `{$nilpointers on}` (typed/untyped pointers
  in `tp` and `objfpc`, scoped with `{$push}`/`{$pop}`, supporting `^T?`, `P?`, and
  `??`), the compiler rejects dereferences, indexing, arithmetic, and `dispose`/`freemem`
  uses that may be nil.
- **Typed addresses**: with `{$T+}` or `{$typedaddress on}` (available in
  `tp` and `objfpc` modes, as in Turbo/Free Pascal, and scoped with
  `{$push}`/`{$pop}`), `@x` yields `^typeof(x)` rather than untyped
  `pointer` (untyped `var`/`const` parameters remain `pointer`), rejecting
  incompatible pointer assignments at compile time.
- **Scoped pointer arithmetic policies**: `{$pointermath unchecked|checked|off}`
  (saved/restored by `{$push}`/`{$pop}`): `off` rejects `+`/`-`, indexing, and
  `Inc`/`Dec` on pointers at compile time while keeping ordering comparisons;
  `checked` enforces live heap allocation bounds at runtime across `p + n`,
  `p[i]`, `Inc`/`Dec`, and same-allocation `p - q`.
- **Unsafe regions**: `unsafe ... end` blocks and `unsafe(expr)` (contextual in
  `tp` and `objfpc` modes) locally relax pointer-safety rules (unchecked pointer
  arithmetic, untyped `@`, and pointer nil checks) while leaving range/overflow
  and class null-safety checks untouched. Passing `--Wunsafe` (implied by
  `--safe=checked`, suppressible with `--Wno-unsafe`) warns on escape-hatch
  constructs used outside `unsafe` regions.
- **Iterators**: `function Fibonacci: iterator UInt64` with `yield`, usable in
  `for ... in`.
- **Lazy sequences**: LINQ-style `map`/`filter`/... in the `sequences` unit.
- **SIMD vectors**: `vector[N] of T` with elementwise `+ - *`, lane
  comparisons giving a vector of booleans (`V = Ord('"')`), lane `and`,
  `or`, `xor`, `not`, `VectorLoad`/`VectorStore` at any address and
  `VectorMask` (a bit per lane); with `PopCnt`, `CountTrailingZeros` and
  `CountLeadingZeros` a byte scan is a load, a compare, a mask and a count
  (SSE2, NEON); `T(v)` converts vectors lane by lane. The unit library
  scans long strings 16 bytes at a time with them: substring search
  (`PosEx`, `StringReplace`), `CompareStr`, `CompareText`, `UpperCase`,
  `LowerCase`, `TStrings.Text`, nethttp's request parsing, and unit json
  (see [`bench/strings`](bench/strings/README.md)).
- **Reachability pruning**: code nothing refers to is left out of the
  executable, also at `-O0`.
- **Attributes, partial types and source generators**: `[Serializable]`
  before a type, field or method; `T = partial record|class` declared in
  pieces; and generators, programs that read a description of the types
  marked with an attribute and write Pascal for them (see below).
- **Compiletime functions**: `const Table = BuildTable;` computed while
  compiling (see below).
- **JSON**: unit `json` a pull reader, documents,
  lookup of one value without parsing the rest, and typed reading and
  writing generated at build time (see below).

## Source generators

A generator writes Pascal source for the types marked with one attribute, at
build time, with no run-time type information. It is a program that starts
`generator Name for Attr;`, uses the unit `p3gen`, and calls `RunGenerator`:

```pascal
generator JsonGenerator for Serializable;
uses p3gen;

procedure Generate(const T: TTypeModel; W: TSourceWriter);
begin
  W.Member('function ToJson: string;');
  W.Line('function ' + T.Name + '.ToJson: string;');
  W.Line('begin Result := ''{...}''; end;');
end;

begin
  RunGenerator(@Generate);
end.
```

```pascal
unit customer;
interface
type
  [Serializable]
  TCustomer = partial record
    Id: Integer;
    [Skip] Cache: Pointer;
  end;
```

`p3 --generator=jsongen.pas main.pas` builds the generator, runs it on a text
description of each type marked `[Serializable]` in the program and in the
units next to it (names, types as written, attributes with literal
arguments, locations; `--emit-model=serializable file.pas` prints it), and
includes what it writes in the file that declares the type: the
`W.Member(...)` declarations in a `partial` second half of the type, the
`W.Line(...)` lines after the unit's declarations, the `W.Declaration(...)`
lines next to the type (a type helper, say: an enumeration marked with the
attribute is in the model too, with its values, but cannot be partial). The
generated code is in
`gen/` next to the input (`--generated-dir`), where it can be read and
debugged; errors in it point there, and `W.Error(Field, 'message')` reports
at the user's own declaration. Types must be declared in a unit's interface
or in a program, as `partial`.

The generator is built again only when its source changes, and runs again
only when the model of the types it handles (or the generator) changes; it
stops after `--generator-timeout` seconds (30), with a memory limit where the
system has one. It is not sandboxed otherwise: it is a native program, so
only use generators you trust. In `p3.json`: `generators`, `generatedDir`,
`generatorTimeout`. Not done yet: generics and properties in the model, a
generator that uses the output of another, resolved types in the model.

[`examples/generators`](examples/generators/README.md) is a project with
two generators (enumeration names, JSON) and compiletime constants.

## Compiletime functions

A function marked `compiletime` runs while compiling, to compute a constant
declared at the level of a program or unit (`--mode=tp` and `objfpc`):

```pascal
type
  TCrcTable = array[0..255] of LongWord;

function BuildCrcTable: TCrcTable; compiletime;
var
  i, j: Integer;
  c: LongWord;
begin
  for i := 0 to 255 do
  begin
    c := i;
    for j := 1 to 8 do
      if Odd(c) then c := (c shr 1) xor $EDB88320 else c := c shr 1;
    Result[i] := c;
  end;
end;

const
  CrcTable = BuildCrcTable;   // computed while compiling
  N = Fib(30);                // (Fib compiletime too) a plain constant
```

The function may not do input or output, use pointers, objects, files,
procedural values or global variables (it can read typed constants), and
calls only compiletime functions, the routines nested in it and builtins
such as `Length`, `Copy` or `Ord`; the compiler reports the rest. For each
such constant, p3 writes a small program to `gen/.ct/` (`--generated-dir`),
builds and runs it with the time and memory limits of generators
(`--generator-timeout`), and uses its value: a typed constant for an array
or record (`CrcTable: TCrcTable = (0, 1996959894, ...)`), a plain constant
otherwise. Like source generators, a `compiletime` function builds and runs native
code on the host (not sandboxed beyond subset checks and resource limits), so
only compile untrusted sources if you have disabled project files and do not
rely on `compiletime` isolation. A runtime error or the time limit is reported at the constant.
The value is computed again only when the sources or the compiler change. A
constant can use earlier ones and units' compiletime functions. The result
can be an ordinal, real or string type, an enumeration, or an array or
record of these (sets not yet), declared in the program or its units'
sources. A function may now also return a static array (Free Pascal does).
`--emit-ast` does not compute them.

## JSON

Unit `json` reads and writes JSON, after Rust's
[sonic-rs](https://github.com/cloudwego/sonic-rs):

```pascal
uses json;

var
  D: TJsonDocument;
begin
  D := JsonParse('{"users": [{"name": "Ada", "langs": ["en", "fr"]}]}');
  WriteLn(D.Root['users'][0]['name'].AsString);      // Ada
  D.Root['users'][0]['langs'].Add('de');
  WriteLn(D.ToJson(True));                            // pretty
  D.Free;
  WriteLn(JsonGet(Text, ['users', 0, 'name']).AsString);  // one value, lazily
  WriteLn(JsonGetPath(Text, '/users/0/name').AsString);   // a JSON Pointer
end.
```

- **`TJsonReader`**: a pull reader. `Read` moves to the next token and
  checks the grammar; `AsString`, `AsInt64`, `AsDouble`, ... read it;
  `Skip` passes over a value.
- **`TJsonDocument`, `TJsonValue`**: a document in blocks of nodes owned by
  the document. Strings and numbers stay in the source text until read, and
  are written back as they were. Values are accessed with `V['key'][0]`,
  `Path('/a/0')`, `Get`, `TryGet` and `As...Def`, and changed with `Put`,
  `Add`, `Delete` and `Remove`. `JsonItems` and `JsonMembers` iterate.
- **`JsonGet`, `JsonGetPath`, `JsonArrayItems`, `JsonObjectItems`**: lazy
  access. A path is followed by skipping everything else: its brackets are
  matched and its strings closed, and nothing more is checked. A
  `TJsonLazy` is read when asked.
- **`TJsonWriter`**: compact or pretty output; doubles are written as the
  shortest text that reads back the same.
- **Typed JSON** through the library's json generator (`--generator=json`,
  or `"generators": ["json"]` in `p3.json`). A record, class or enumeration
  marked `[Json]` gets `ToJson`, `FromJson`, `JsonWrite` and `JsonRead`,
  which read straight into the value. Fields can be numbers, strings,
  booleans, dynamic arrays and `[Json]` types; `[Skip]` and
  `[JsonName('n')]` are supported. Any other field type is an error at the
  field, while compiling.
- **Numbers and errors**:
  - Integers are exact (`Int64`, or `UInt64`). Other numbers convert to the
    nearest double (Eisel-Lemire for up to 19 digits).
  - Strings are checked as UTF-8.
  - Errors raise `EJsonError` with a line, a column and a serde_json-style
    message.
- **Speed**: strings are scanned 8 bytes at a time. Skipping a value (lazy
  access, `Skip`), escaping and the UTF-8 check use 16-byte SIMD vectors.
  On the usual benchmark files it parses faster than serde_json on strings
  and structure, but slower on numbers and slower than sonic-rs's SIMD
  parser. Lazy access is about three times sonic-rs's. Results are in
  [`bench/json`](bench/json/README.md), and an example is in
  [`examples/json`](examples/json/README.md).

## Repository layout

| Path | Contents |
| --- | --- |
| `src/`, `include/` | the C++ compiler (lexer, parser, sema, codegen, driver) |
| `selfhost/` | the self-hosted compiler, written in Pascal |
| `runtime/`, `rtl/` | the C runtime and the Pascal unit library |
| `tests/`, `bench/` | lit tests and benchmarks |
| `examples/` | example projects built with the self-hosted compiler |
| `utils/`, `tools/` | helper scripts |
| `docs/` | documentation: [building](docs/build.md), [compiler parameters and directives](docs/compiler-params-and-directives.md) |

## License

Copyright 2026 Luis Ortega.

p3 (both compilers, the runtime and the unit library) is licensed under the
[Apache License 2.0 with LLVM Exceptions](LICENSE), LLVM's own license
(SPDX: `Apache-2.0 WITH LLVM-exception`). The exception means that programs
you compile with p3 may be distributed under terms of your choice without
p3's license notices, although they contain parts of its runtime and unit
library.

Some material has other terms, listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). **Windows programs that
wait on sockets contain wepoll and must carry its notice.** Versions of p3
published before this change were under the GNU GPL version 2.

## Compiler parameters and directives

The full list of [compiler directives](docs/compiler-params-and-directives.md#compiler-directives)
and [compiler parameters](docs/compiler-params-and-directives.md#compiler-parameters)
is in [docs/compiler-params-and-directives.md](docs/compiler-params-and-directives.md).

## The project file (p3.json)

A project file holds settings for compiling a project's programs and units
(mode, unit and library paths, defines, libraries, optimization, debug info,
target, warnings, checks) and its main program and output. It is one JSON
object; its paths are relative to its own directory. It is parsed in
`selfhost/pcproject.pas` (a port of `src/driver/Project.cpp`, same
diagnostics).

It is looked for in the input file's directory (the current directory without
an input file) and its parents; `--project=<file>` names one explicitly, and
`--no-project` ignores project files altogether (recommended when compiling
untrusted files inside shared directories, since `p3.json` can configure
`generators`, `libraries`, and paths). The command line wins over
the project in value settings (`mode`, `output`, `optimize`, `target`, `cpu`,
`errorLimit`); the boolean flags combine — the project can turn on
`debug`/`separateUnits`/warnings-as-errors just as `-g`/`--fseparate-units`/
`--Werror` can, and it can turn off checks just as `--fno-range-checks` can.

| Setting | Meaning | Default when absent |
| --- | --- | --- |
| `name`, `version`, `description` | for people and tools; not used by p3 | — |
| `main` | the main program to build when p3 runs with no input file | — (an error then) |
| `output` | where the executable goes for a `main` build (parent directories are created) | the input name |
| `mode` | `"iso"`, `"tp"` or `"objfpc"` | `iso` |
| `optimize` | optimization level, an integer from 0 to 3 | 0 |
| `errorLimit` | stop after this many errors; 0 means no limit | 20 |
| `debug` | generate debug information | false |
| `separateUnits` | link separately compiled units (like `--fseparate-units`) | false |
| `wholeProgram` | with `separateUnits`, optimize the units as part of the program (like `--fwhole-program`) | false |
| `unitPaths` | array of directories added to the unit search path (like `-I`) | — |
| `defines` | array of symbols defined for conditional compilation (like `-d`) | — |
| `libraryPaths` | array of directories added to the linker's search path (like `-L`) | — |
| `libraries` | array of libraries to link with (like `-l`) | — |
| `generators` | array of source generator programs (like `--generator`) | — |
| `generatedDir` | where the generators' code goes (like `--generated-dir`) | `gen` |
| `generatorTimeout` | seconds a generator may run, 1 to 86400 | 30 |
| `target` | the target triple | host |
| `cpu` | the target CPU (like `-mcpu`) | generic |
| `warnings` | object: `asErrors`, `unusedVariable`, `deprecated` | all `true` |
| `checks` | object: `range`, `overflow` | all `true` |
| `safety` | object: `safe` (`"standard"`, `"checked"`, or `"verified"`) | `"standard"` |

A duplicate setting is an error; an unknown one is warned about and ignored.
Example (`examples/httploopserver/p3.json`):

```json
{
  "name": "httploopserver",
  "version": "0.1.0",
  "description": "An HTTP/1.1 server on a single-threaded poll() event loop (non-blocking sockets, keep-alive) routed by netrouter",
  "main": "src/httploopserver.pas",
  "output": "bin/httploopserver",
  "mode": "objfpc",
  "unitPaths": ["src/lib"],
  "optimize": 2
}
```
