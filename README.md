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

## Building

Requires CMake 3.20 or later, a C++20 compiler and LLVM 23 (CMake rejects
other major versions). Ninja is used below. The self-hosted compiler and the
inlining of runtime functions also need Python 3; the latter needs LLVM's
`clang` and `llvm-link` too. If CMake does not find LLVM, pass
`-DLLVM_DIR=<llvm>/lib/cmake/llvm`.

```sh
cmake -G Ninja -S . -B build
ninja -C build
```

The default configuration is `Release` (`-O2`/`-O3`); use
`-DCMAKE_BUILD_TYPE=Debug` (or `RelWithDebInfo`) while working on the
compiler. The C runtime is built at `-O2` in every configuration, and
`ninja -C build p3-self` builds the distributed self-hosted compiler at
`-O2`; `-DP3_SELFHOST_NO_CHECKS=ON` turns its range and overflow checks
off once the compiler is trusted enough for that.

### Ubuntu 24.04

LLVM 23 comes from [apt.llvm.org](https://apt.llvm.org):

```sh
sudo apt-get update
sudo apt-get install -y git cmake ninja-build build-essential python3 wget \
  zlib1g-dev libzstd-dev libedit-dev libxml2-dev
wget https://apt.llvm.org/llvm.sh
sudo bash llvm.sh 23
sudo apt-get install -y llvm-23-dev llvm-23-tools clang-23

cmake -G Ninja -S . -B build \
  -DLLVM_DIR=/usr/lib/llvm-23/lib/cmake/llvm \
  -DCMAKE_C_COMPILER=clang-23 -DCMAKE_CXX_COMPILER=clang++-23
ninja -C build
```

### Windows

p3 does not build natively on Windows yet. Build it in
[WSL2](https://learn.microsoft.com/windows/wsl/install): in an administrator
PowerShell, run

```powershell
wsl --install -d Ubuntu-24.04
```

Then follow the Ubuntu 24.04 steps above inside Ubuntu, with the repository
cloned into the Linux file system (under `~`, which is much faster than
`/mnt/c`). The compiler is then a Linux program. For it to write Windows
executables, install MinGW-w64 (`sudo apt-get install -y mingw-w64`) before
running `cmake` (if `build/` is already configured, run `cmake` again). Either
of MinGW's thread variants works.

See **Cross-compiling** under [Compiler parameters](#compiler-parameters).

### Installing

After building:

```sh
cmake --install build --prefix <dir>
```

This writes the C++ compiler to `<dir>/bin/p3`, the runtime and unit library
to `<dir>/lib` (with the Windows runtime, if it was built), and the licenses
to `<dir>/share/doc/p3`. p3 finds its runtime and units relative to its own
location, so the directory can be moved; keep its layout. The self-hosted
compiler is not installed.

The installed p3 still needs the shared LLVM library it was built against
(unless configured with `-DP3_LINK_LLVM_DYLIB=OFF`, which is untested), and
a C compiler to link programs. It is not a self-contained package for a
machine without them.

## Usage

```sh
build/bin/p3 hello.pas -o hello          # ISO 7185
build/bin/p3 --mode=tp hello.pas         # Turbo/Free Pascal extensions
build/bin/p3 --mode=objfpc hello.pas     # plus classes, exceptions, ...
```

See the language and compiler reference for the full list
of modes, options and supported features.

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
- **Null-safe references**: with `{$nullsafe on}`, `T?`, `?.` and `??`; the
  compiler rejects dereferences that may be nil.
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
otherwise. A runtime error or the time limit is reported at the constant.
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

## Testing

Tests are [lit](https://llvm.org/docs/CommandGuide/lit.html) files under
`tests/`:

```sh
ninja -C build check
```

This needs `lit` and `FileCheck`. On Ubuntu, `FileCheck` is in
`llvm-23-tools`; install `lit` with `pipx install lit` (package `pipx`, then
`pipx ensurepath` and a new shell) and run `cmake` again.

With MinGW-w64 and Wine installed, `tests/windows` also runs the programs
of `tests/run` as Windows executables (see Cross-compiling).

## Repository layout

| Path | Contents |
| --- | --- |
| `src/`, `include/` | the C++ compiler (lexer, parser, sema, codegen, driver) |
| `selfhost/` | the self-hosted compiler, written in Pascal |
| `runtime/`, `rtl/` | the C runtime and the Pascal unit library |
| `tests/`, `bench/` | lit tests and benchmarks |
| `examples/` | example projects built with the self-hosted compiler |
| `utils/`, `tools/` | helper scripts |
| `docs/` | documentation |

## License

See [LICENSE](LICENSE).

## Compiler directives

All handled in `selfhost/pclex.pas` (the C++ compiler recognizes the same set).

**Conditionals** (pclex.pas:1284-1293)

`$IF` `$IFDEF` `$IFNDEF` `$IFOPT` `$ELSEIF` `$ELSE` `$ENDIF` `$IFEND`
`$DEFINE` `$UNDEF`

**Files / linking**

- `$I file` / `$INCLUDE file` (pclex.pas:1579)
- `$interfaces com|corba` (pclex.pas:1594)

**Switch-like directives**

- `$H+`/`$H-` (long strings; long form `$longstrings on/off`, pclex.pas:1762)
- `$R+`/`$R-` (range checks), `$Q+`/`$Q-` (overflow checks) (pclex.pas:1802-1805;
  comma lists like `{$R-,Q+}` work)
- `$A+|-`, `$A1|2|4|8|16|32`, `$align N|on|off` (pclex.pas:1692,1706,1795)
- `$packrecords 1|2|4|8|16|32|C|DEFAULT|NORMAL` (pclex.pas:1679)
- `$scopedenums on/off` (pclex.pas:1711)
- `$inline on/off` (pclex.pas:1723)
- `$coperators on/off` (pclex.pas:1736)
- `$macro on/off` (pclex.pas:1750)
- `$nullsafe on/off` (p3's own, objfpc mode) (pclex.pas:1616)
- `$push` / `$pop` (pclex.pas:1639,1658)

**Mode switches** — `$modeswitch NAME on/off` (pclex.pas:1454)

- Effective: `advancedrecords`, `typehelpers`, `racechecks` (p3's own),
  `result`, `out`, `defaultparameters`
- Accepted but mode-decided: `class`, `objpas`, `properties`, `exceptions`
- Accepted but always on: `cvar`, `initfinal`, `hintdirective`, `allowinline`
- Known-and-ignored (p3 lacks them): `pchartostring`, `nestedcomments`,
  `classicprocvars`, `macprocvars`, `repeatforward`, `pointertoprocvar`,
  `autoderef`, `ansistrings`, `duplicatelocals`, `objectivec1`/`objectivec2`,
  `nestedprocvars`, `nonlocalgoto`, `isounaryminus`, `systemcodepage`,
  `finalfields`, `unicodestrings`, `cblocks`, `isoio`, `isoprogramparas`,
  `isomod`, `arrayoperators`

Any other switch letter is warned about and ignored ("listing, boolean
evaluation, ..." — pclex.pas:1792).

## Compiler parameters

As printed by `p3 --help` (parsed in `selfhost/p3.pas`, same set in the C++
compiler).

**Output and actions** (default: a linked executable named after the input)

- `-o <file>` — write output to `<file>` (default: input name without `.pas`)
- `-S` — emit target assembly (`.s`)
- `-c` — emit an object file (`.o`)
- `--emit-llvm` — emit LLVM IR (`.ll`)
- `--emit-ast` — dump the parsed AST
- `--emit-parse-tree` — dump the parse tree (no semantic analysis; for testing)
- `--emit-tokens` — dump lexer tokens
- `--emit-model=<attr>` — print the source generator model of the types marked
  `[attr]` (for testing)

**Language**

- `--mode=<iso|tp|objfpc>` — language dialect (default: `iso`)

**Source generators**

- `--generator=<file>` — build and run a source generator first (repeatable;
  the code it writes is included in the units that declare its types)
- `--generated-dir=<dir>` — where that code goes (default: `gen` next to the
  input)
- `--generator-timeout=<seconds>` — how long a generator, or the program
  computing a compiletime constant, may run (default 30)

**Optimization and code generation**

- `-O <level>` — optimization level 0-3 (default: `0`)
- `-Os` — optimize for size (like `-O2`, SLP vectorization on)
- `-Oz` — optimize aggressively for size (like `-Os`)
- `--ffast-math` — fast-math flags on floating-point instructions (off by default)
- `--fkeep-unused` — keep routines and data nothing reachable refers to (off by
  default: such code is pruned, also at `-O0`)
- `--fseparate-units` — link separately compiled units (each compiled with
  `-c` to `<unit>.o` next to its source; off by default)
- `-g` — generate debug information (off by default)
- `--target=<triple>` — target triple (default: host). Any triple of a target
  LLVM 23 was built with: X86, AArch64, ARM, RISCV, Mips, PowerPC, WebAssembly,
  AMDGPU, NVPTX, AVR, MSP430, BPF, Hexagon, Lanai, LoongArch, M68k, Sparc,
  SystemZ, VE, XCore, Xtensa — written as `x86_64-pc-linux-gnu`,
  `aarch64-apple-darwin`, ... For a non-host triple `-S`, `-c` and
  `--emit-llvm` always work; linking an executable needs the runtime built
  for that target (see Cross-compiling: Windows has it).
- `-mcpu <cpu>` — target CPU (default: `generic`; `native` selects the host
  CPU and its feature set). Other names are the target's LLVM CPU names —
  e.g. for x86 `x86-64-v3`, `skylake`, `znver4`; for AArch64 `apple-m1`,
  `neoverse-n2` — passed to LLVM, which rejects unknown ones.
- `-mattr <features>` — target features, e.g. `+avx2,-sse4.2`

**Cross-compiling**

Compilation to a non-host triple works as usual (`-S`, `-c`, `--emit-llvm`),
and the unit library takes the target's branches (`MSWINDOWS`, `LINUX`,
`DARWIN`).

Windows executables (64-bit x86) come from any host. Install MinGW-w64
(`brew install mingw-w64`, or the distribution's `mingw-w64` package):
CMake finds `x86_64-w64-mingw32-gcc` and builds the runtime for Windows too,
into `lib/x86_64-w64-mingw32/libpascalrt.a`. Then:

```sh
build/bin/p3 --mode=objfpc --target=x86_64-w64-mingw32 hello.pas   # hello.exe
```

p3 links with the target's own toolchain (`x86_64-w64-mingw32-gcc`, or
`P3_CROSS_CC`), statically: the program needs no DLL but Windows' own (the
Universal C Runtime, Windows 10 and later). `-DP3_CROSS_TARGETS=<triples>`
picks the targets to build the runtime for (`OFF`: none). The whole unit
library works there, tasks, iterators and the network units included:
`netasync`, `netloop` and `nethttp` use Winsock, and wait on sockets with
the bundled [wepoll](https://github.com/piscisaureus/wepoll) (epoll over
the AFD driver's polling, BSD-2-Clause, in `runtime/wepoll`), as on Linux.
Two things differ: `TNetListenOpts.ReusePort` raises `ENetError` (Windows
has no `SO_REUSEPORT`; `netloop` shares one listener among its loops, as on
macOS), and `TNetLoop.WatchFd` takes sockets only. With
[Wine](https://www.winehq.org/) installed, the tests run every program of
`tests/run` again as a Windows program (`tests/windows`).

Without the runtime for a target, p3 says how to build it; the MSVC triple
(`x86_64-pc-windows-msvc`) and other targets compile but do not link yet.

**Runtime checks**

- `--fno-overflow-checks` — disable runtime integer overflow checks (on by default)
- `--fno-range-checks` — disable runtime range checks (subranges, array
  indices, ...; on by default)
- `--fno-zero-init` / `--fzero-init` — local arrays, records and strings start
  zeroed at `-O0` (the default there); from `-O1` they are not (use
  `--fzero-init` to ask for it)
- `--fno-inline-runtime` — do not inline the runtime's functions (from `-O1`
  they are, when compiling for the host)

**Searching and linking**

- `-I <dir>` — add a directory to the unit search path
- `-L <dir>` — add a directory to the library search path (linking)
- `-l <name>` — link with library `<name>`
- `--project=<file>` — read project settings from `<file>` (default: the first
  `p3.json` in the input file's directory or its parents)
- `--no-project` — do not read a project file

**Diagnostics**

- `--Werror` — treat warnings as errors (off by default)
- `--Wno-deprecated` — do not warn about uses of deprecated declarations
- `--Wno-unused-variable` — do not warn about unused variables
- `--fdiagnostics-color=<auto|always|never>` — colorize diagnostics (default: `auto`)
- `-d <name>` — define a symbol for conditional compilation (`{$IFDEF}`)
- `--ferror-limit=<N>` — stop after N errors (default: `20`; `0` = no limit)

**Information**

- `--help` — display available options
- `--version` — display the version of this program

## The project file (p3.json)

A project file holds settings for compiling a project's programs and units
(mode, unit and library paths, defines, libraries, optimization, debug info,
target, warnings, checks) and its main program and output. It is one JSON
object; its paths are relative to its own directory. It is parsed in
`selfhost/pcproject.pas` (a port of `src/driver/Project.cpp`, same
diagnostics).

It is looked for in the input file's directory (the current directory without
an input file) and its parents; `--project=<file>` names one explicitly, and
`--no-project` ignores project files altogether. The command line wins over
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
