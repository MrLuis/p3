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
| `httpasyncserver` | an HTTP/1.1 server on the async engine: `netasync` tasks (one per connection, keep-alive), HTTP by `nethttp`, routes by `netrouter`, a handler that waits; see its [README](httpasyncserver/README.md) |
| `httploopserver` | the same site on the loop engine: the `netloop` unit's per-thread epoll/kqueue event loops, HTTP by `nethttp`, routes by `netrouter`; see its [README](httploopserver/README.md) |
| `generators` | source generators and compiletime functions: `[EnumNames]` gives an enumeration `ToString`/`TryParse`, `[Json]` gives records `ToJson`, a CRC table computed by the compiler; see its [README](generators/README.md) |
| `sqlite` | SQLite through `external` routines, a small wrapper with tagged SQL templates, tuples and an enumerator; see its [README](sqlite/README.md) |
| `netasync` | a concurrent TCP echo server built on the `netasync` unit: async/await sockets, `scope`/`spawn`/`detached`, unit-library `threading`; see its [README](netasync/README.md) |
| `netrouter` | chi-style routing with the `netrouter` unit: URL parameters, expressions, middleware, sub-routers, on a small netasync server; see its [README](netrouter/README.md) |
