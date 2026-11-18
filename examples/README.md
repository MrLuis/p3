# Examples

Projects built with the **self-hosted compiler**: p3 written in Pascal
(`selfhost/`), compiled by the C++ p3 into `build/bin/p3-self`. Each
project is a directory with a `p3.json` (see "Project files" in the
top-level README), its program in `src/` and its units in `src/lib/`.

## Building and running a project

```sh
cd examples/hello
../p3              # builds bin/hello as p3.json says
bin/hello Ada
```

`examples/p3` runs `build/bin/p3-self` with the arguments it is given. It
first brings the self-hosted compiler up to date (`cmake --build build
--target p3-self`, about a minute after a change to the compiler, nothing
otherwise), so the compiler must have been configured once:

```sh
cmake -S . -B build -G Ninja && cmake --build build
```

Set `P3_BUILD_DIR` to use another build directory. Anything p3 accepts
works through the wrapper: `../p3 -O2`, `../p3 --emit-llvm -o -`,
`../p3 src/other.pas -o bin/other`.

## Creating a project

```sh
examples/new shapes            # objfpc mode; or: examples/new shapes tp
cd examples/shapes
../p3 && bin/shapes
```

This creates `examples/shapes/` with a `p3.json`, `src/shapes.pas` and an
empty `src/lib/` for its units. New projects start at `-O0` with debug
information (`"optimize": 0, "debug": true`); edit `p3.json` to change
that. Executables go to the project's `bin/`, which git ignores.

## Projects

| Project | Shows |
|---|---|
| `hello` | a program with a class in a unit of its own and a unit of p3's library (`sysutils`) |
| `httpserver` | a basic HTTP server: C library sockets through `external` routines, ansistrings, units; see its [README](httpserver/README.md) |
| `sqlite` | SQLite through `external` routines, a small wrapper with tagged SQL templates, tuples and an enumerator; see its [README](sqlite/README.md) |
