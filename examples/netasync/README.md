# netasync demo

A concurrent TCP echo server built with the **`netasync` unit**: the whole
server is async/await — the accept loop and one task per connection, all
parked on socket readiness through the runtime's task scheduler instead of
blocking threads. No threads are ever blocked in a syscall: `epoll`/`kevent`
readiness is delivered into the tasks by one poller thread.

What it shows:

- `NetListen` / `NetAccept` / `NetReadSome` / `NetSendAll` (unit `netasync`)
- `scope` / `spawn` for the accept loop, `detached(...)` for connection
  tasks, `TAtomic<Integer>` for the shared counter (unit `threading`)
- cancellation-free parking: many connections, one thread per CPU core

## Build and run

```sh
cd examples/netasync
../p3                        # with the self-hosted compiler, or: cmake --build build && build/bin/p3 -fdiagnostics-color=never
bin/netasync_demo            # or: bin/netasync_demo --requests N
```

Then connect with anything:

```sh
nc 127.0.0.1 PORT            # type lines; they come back
```

With `--requests N` the server stops itself after N served connections and
prints a summary — that is how the driver test
(`tests/driver/examples-netasync.test`) checks it.

Requires Linux or macOS (Windows later). See `docs/todo.md` for the netasync
design notes.
