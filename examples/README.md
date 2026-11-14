# Examples

Projects built with the **self-hosted compiler**: pcc written in Pascal
(`selfhost/`), compiled by the C++ pcc into `build/bin/pcc-self`. Each
project is a directory with a `pcc.json` (see "Project files" in the
top-level README), its program in `src/` and its units in `src/lib/`.

## Building and running a project

```sh
cd examples/hello
../pcc              # builds bin/hello as pcc.json says
bin/hello Ada
```

`examples/pcc` runs `build/bin/pcc-self` with the arguments it is given. It
first brings the self-hosted compiler up to date (`cmake --build build
--target pcc-self`, about a minute after a change to the compiler, nothing
otherwise), so the compiler must have been configured once:

```sh
cmake -S . -B build -G Ninja && cmake --build build
```

Set `PCC_BUILD_DIR` to use another build directory. Anything pcc accepts
works through the wrapper: `../pcc -O2`, `../pcc --emit-llvm -o -`,
`../pcc src/other.pas -o bin/other`.

## Creating a project

```sh
examples/new shapes            # objfpc mode; or: examples/new shapes tp
cd examples/shapes
../pcc && bin/shapes
```

This creates `examples/shapes/` with a `pcc.json`, `src/shapes.pas` and an
empty `src/lib/` for its units. New projects start at `-O0` with debug
information (`"optimize": 0, "debug": true`); edit `pcc.json` to change
that. Executables go to the project's `bin/`, which git ignores.

## Projects

| Project | Shows |
|---|---|
| `hello` | a program with a class in a unit of its own and a unit of pcc's library (`sysutils`) |
