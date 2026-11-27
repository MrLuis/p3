# Compiler parameters and directives

For building and installing, see [docs/build.md](build.md) and the
[README](../README.md).

## Compiler directives

All handled in `selfhost/pclex.pas` (the C++ compiler recognizes the same set).

**Conditionals**

- `$IF <expr>` — compile the following block if the compile-time conditional
  expression `<expr>` evaluates to true (supports macros, `defined(...)`,
  `declared(...)`, arithmetic/boolean operators, and `in [values]`)
- `$IFDEF <sym>` — compile the following block if symbol `<sym>` is defined
- `$IFNDEF <sym>` — compile the following block if symbol `<sym>` is not defined
- `$IFOPT` — recognized in conditional syntax, but reports an error because
  `$IFOPT` is not supported
- `$ELSEIF <expr>` — continue an `$IF` block with another conditional expression
- `$ELSE` — fallback branch when preceding conditional branches were false
- `$ENDIF` — close the current conditional block
- `$IFEND` — synonym for `$ENDIF`
- `$DEFINE <sym>` / `$DEFINE <sym> := <value>` — define a conditional symbol
  (or, when `{$macro on}` is active, a macro whose value expands in code and
  `$IF` expressions)
- `$UNDEF <sym>` — undefine a conditional symbol (and any macro with that name)

**Files / interfaces**

- `$I <file>` / `$INCLUDE <file>` — insert the contents of `<file>` at this
  point in compilation (quotes optional; single-letter `$I+`/`$I-` is treated
  as a switch rather than an include)
- `$interfaces <com|corba>` — choose whether interfaces declared after it use
  reference-counted COM interfaces (`com`, default) or plain CORBA-style
  interfaces (`corba`)

**Switch-like directives**

Single-letter switches (`$H+`, `$R-`, `$Q+`, `$T+`, `$A4`) can also be combined
in comma-separated lists like `{$R-,Q+,T+}`.

- `$H+` / `$H-`, `$longstrings on|off` — treat `string` as an `AnsiString`
  (`+`/`on`) or a `ShortString` (`-`/`off`)
- `$R+` / `$R-` — enable or disable runtime range checks (subranges, array
  indices, etc.) for code after the directive (default: on unless disabled via
  `--fno-range-checks`)
- `$Q+` / `$Q-` — enable or disable runtime integer overflow checks for code
  after the directive (default: on unless disabled via `--fno-overflow-checks`)
- `$T+` / `$T-`, `$typedaddress on|off` — enable or disable typed `@` addresses:
  when on, `@x` yields `^typeof(x)` instead of untyped `pointer` (in `tp` and
  `objfpc` modes; forced on by `--safe=verified`)
- `$pointermath unchecked|checked|off` — control pointer arithmetic (`p + n`,
  `p[i]`, `Inc`/`Dec`, `p - q`): `unchecked` allows raw pointer math, `checked`
  adds runtime heap allocation bounds checks, and `off` rejects pointer math
  and indexing at compile time while still allowing comparisons
- `$A+` / `$A-`, `$A1|2|4|8|16|32`, `$align 1|2|4|8|16|32|on|off` — set the
  maximum field alignment (in bytes) for records declared afterward (`on`/`+`
  sets 4-byte alignment; `off`/`-` sets 1-byte alignment)
- `$packrecords 1|2|4|8|16|32|C|DEFAULT|NORMAL` — set record field packing/alignment
  for records declared afterward (`C`, `DEFAULT`, and `NORMAL` select natural C
  layout)
- `$scopedenums on|off` — when on, values of enumerations declared afterward
  must be qualified by their type name (for example `TColor.Red`)
- `$inline on|off` — enable or disable `inline` expansion for routines marked
  `inline` (default: on in `objfpc` mode)
- `$coperators on|off` — enable or disable C-style compound assignment
  operators (`+=`, `-=`, `*=`, `/=`; default: on in `tp` and `objfpc` modes,
  off in `iso` mode; not saved/restored by `$push`/`$pop`)
- `$macro on|off` — enable or disable `$DEFINE NAME := value` macro expansion
  (default: off; not saved/restored by `$push`/`$pop`)
- `$nullsafe on|off` — enable or disable null-safe classes/interfaces in
  routines declared afterward (`T?`, `?.`, `??`; `p3`-specific, `objfpc` mode)
- `$nilpointers on|off` — enable or disable non-nil pointer checking in routines
  declared afterward (`^T?`, `P?`, `??`; `p3`-specific, `tp` and `objfpc` modes)
- `$push` / `$pop` — save and restore local switch settings (`$H`, `$interfaces`,
  `$inline`, `$scopedenums`, `$nullsafe`, `$nilpointers`, `$packrecords`/`$align`,
  `$R`, `$Q`, `$T`, and `$pointermath`; up to 21 nested
  levels; mode switches and `$macro` are not local)

**Mode switches** — `$modeswitch NAME[+|-| on|off]`

- Effective switches (toggle behavior in `p3`):
  - `advancedrecords` — allow methods, visibility sections, properties, and
    class operators inside records (`objfpc`)
  - `typehelpers` — allow record and type helper declarations (`objfpc`)
  - `racechecks` — enable compile-time data-race checking on `spawn` blocks
    with `shared`/`isolated` qualifiers (`p3`-specific)
  - `result` — enable the implicit `Result` variable in functions in any mode
    (on by default in `objfpc`)
  - `out` — enable `out` parameters in any mode (on by default in `objfpc`)
  - `defaultparameters` — enable default parameter values in any mode (on by
    default in `objfpc`)
- Accepted when matching the active compiler mode (otherwise warned and ignored):
  - `class`, `objpas`, `properties`, `exceptions` — tied to `--mode=objfpc`
    (on in `objfpc`, off in `tp`/`iso`)
- Accepted when enabled (cannot be turned off; warned and ignored if disabled):
  - `cvar`, `initfinal`, `hintdirective`, `allowinline`
- Known-and-ignored (accepted when turned off, warned as unsupported if turned
  on):
  - `pchartostring`, `nestedcomments`, `classicprocvars`, `macprocvars`,
    `repeatforward`, `pointertoprocvar`, `autoderef`, `ansistrings`,
    `duplicatelocals`, `objectivec1`, `objectivec2`, `nestedprocvars`,
    `nonlocalgoto`, `isounaryminus`, `systemcodepage`, `finalfields`,
    `unicodestrings`, `cblocks`, `isoio`, `isoprogramparas`, `isomod`,
    `arrayoperators`

Any other single-letter switch (such as listing or short-circuit boolean
evaluation switches) is ignored. Unsupported multi-character directives emit a
warning and are ignored.

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
- `--safe=<standard|checked|verified>` — memory safety level (default: `standard`;
  `verified` forces `{$T+}` typed addresses on)

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
- `--fwhole-program` — with `--fseparate-units`, from `-O1`: optimize the
  units' code (the `<unit>.ll` each `-c` compile at `-O1`+ leaves next to its
  object) as part of the program and link no unit objects, so unused unit
  code is dropped and every unit routine can be inlined. A unit compiled at
  `-O0` has no `.ll`; then p3 warns and links the objects. Off by default:
  each program re-optimizes its units, which costs the compile time
  separate units save
- `-g` — generate debug information (off by default)
- `--target=<triple>` — target triple (default: host), written as
  `x86_64-pc-linux-gnu`, `aarch64-apple-darwin`, ... The release packages
  (archives, Scoop, winget) have LLVM's X86 and AArch64 targets; a p3 built
  from source, or Homebrew's, has every target LLVM 23 was built with (X86,
  AArch64, ARM, RISCV, Mips, PowerPC, WebAssembly, AMDGPU, NVPTX, AVR,
  MSP430, BPF, Hexagon, Lanai, LoongArch, M68k, Sparc, SystemZ, VE, XCore,
  Xtensa), unless configured with `-DP3_LLVM_TARGETS` ([docs/build.md](build.md)). For a
  non-host triple `-S`, `-c` and `--emit-llvm` always work; linking an
  executable needs the runtime built for that target (see Cross-compiling:
  Windows has it).
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
(`brew install mingw-w64`, or the distribution's `mingw-w64` package); a
build of p3 then includes the runtime for Windows programs
([docs/build.md](build.md#the-runtime-for-windows-programs)), as do the
release archives. Then:

```sh
build/bin/p3 --mode=objfpc --target=x86_64-w64-mingw32 hello.pas   # hello.exe
```

p3 links with the target's own toolchain (`x86_64-w64-mingw32-gcc`, or
`P3_CROSS_CC`), statically: the program needs no DLL but Windows' own. Which
C library that is depends on the toolchain: UCRT (Windows 10 and later; for
example Homebrew's MinGW-w64) or msvcrt.dll (any Windows; for example
Debian's and Ubuntu's). p3 asks the toolchain's headers which one it links
and uses the runtime built for it; `P3_WINDOWS_CRT=ucrt` or `msvcrt` says
so instead. The whole unit
library works there, tasks, iterators and the network units included:
`netasync`, `netloop` and `nethttp` use Winsock, and wait on sockets with
the bundled [wepoll](https://github.com/piscisaureus/wepoll) (epoll over
the AFD driver's polling, BSD-2-Clause, in `runtime/wepoll`), as on Linux.
Two things differ: `TNetListenOpts.ReusePort` raises `ENetError` (Windows
has no `SO_REUSEPORT`; `netloop` shares one listener among its loops, as on
macOS), and `TNetLoop.WatchFd` takes sockets only.

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
