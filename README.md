# p3 — a Pascal compiler targeting LLVM

`p3` compiles Pascal to native executables through LLVM. It starts from
ISO 7185 and adds Turbo/Free Pascal extensions (units, strings, classes,
exceptions, ...) plus a few features of its own (null-safe references,
owned variables, template strings, iterators with `yield`, lazy sequences).

There are two compilers, kept in parity:

- **C++ `p3`** (`src/`, `include/`): the original compiler, built with CMake.
- **Self-hosted `p3`** (`selfhost/`): the same compiler written in Pascal,
  which can compile itself.

## Building

Requires CMake, a C++ compiler and LLVM.

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

## Usage

```sh
build/bin/p3 hello.pas -o hello          # ISO 7185
build/bin/p3 --mode=tp hello.pas         # Turbo/Free Pascal extensions
build/bin/p3 --mode=objfpc hello.pas     # plus classes, exceptions, ...
```

See the [language and compiler reference](docs/README.md) for the full list
of modes, options and supported features.

## Features

Beyond ISO 7185 and the Turbo/Free Pascal extensions, p3 has features of its
own. They need `--mode=objfpc` unless noted; each works in both compilers
(most come from the "Modernizing Pascal" proposals). The details and
examples are in the [reference](docs/README.md).

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
  routine and unpacked on assignment.
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
- **Reachability pruning**: code nothing refers to is left out of the
  executable, also at `-O0`.

## Testing

Tests are [lit](https://llvm.org/docs/CommandGuide/lit.html) files under
`tests/`:

```sh
ninja -C build check
```

## Repository layout

| Path | Contents |
| --- | --- |
| `src/`, `include/` | the C++ compiler (lexer, parser, sema, codegen, driver) |
| `selfhost/` | the self-hosted compiler, written in Pascal |
| `runtime/`, `rtl/` | the C runtime and the Pascal unit library |
| `tests/`, `bench/` | lit tests and benchmarks |
| `examples/` | example projects built with the self-hosted compiler |
| `utils/`, `tools/` | helper scripts |
| `docs/` | documentation (below) |

## Documentation

All in [`docs/`](docs/):

- [docs/README.md](docs/README.md): language and compiler reference
- [docs/PLAN.md](docs/PLAN.md): design and roadmap
- [docs/SELFHOST.md](docs/SELFHOST.md): plan for the self-hosted compiler
- [docs/optimization.md](docs/optimization.md): optimizations considered and done
- [docs/optimization2.md](docs/optimization2.md): executable-speed analysis after those items

Also: [selfhost/README.md](selfhost/README.md),
[selfhost/DIALECT.md](selfhost/DIALECT.md) and
[examples/README.md](examples/README.md).

## License

See [LICENSE](LICENSE).
