# httploopserver

An HTTP/1.1 server on p3's **loop engine**: the **`netloop`** unit, event
loops over non-blocking sockets (epoll on Linux, kqueue on macOS), one per
thread. Its routes come from the **`netrouter`** unit: the same site as
[httpasyncserver](../httpasyncserver), which runs it on `netasync` tasks.

```sh
cd examples/httploopserver
p3                            # builds bin/httploopserver (p3.json)
bin/httploopserver             # http://127.0.0.1:8080/, serving public/
```

```
httploopserver [--port N] [--root DIR] [--any] [--threads N] [--requests N]
```

The options and the routes are httpasyncserver's (see its README), plus
`--threads N`, the number of loops (default: one per CPU), and minus
`/sleep/{ms}`: a handler here runs on its loop's thread and must not wait,
or every connection of that loop waits with it.

## How it works

The program builds the site's router and hands it to the RTL:

```pascal
Srv := TNetLoopServer.Create(Threads);
Srv.Listen(host, Port);
HttpServeLoop(Srv, RouterHandler(SiteRouter), @Answered);
Srv.Run;
```

`HttpServeLoop` (unit `nethttp`) installs the server's three callbacks:

- `OnOpen`: a new connection gets a `THttpContext`, reused for each of its
  requests;
- `OnData`: every whole request in the bytes received so far (keep-alive,
  pipelining, `Content-Length` and chunked bodies) is parsed without
  copying, routed by `RouterHandler` (`netrouter`: the handler reads the
  request from the context and fills the answer in) and its answer written;
  what was used is consumed, the rest waits for more bytes;
- `OnClose`: the context is freed.

netloop does the rest: every loop accepts on the listeners itself (on
Linux each loop has its own, with `SO_REUSEPORT`, and the kernel spreads
the connections), reads until `EAGAIN`, calls `OnData`, sends what the
socket takes and waits for writability only while output is pending. A
connection never leaves its loop's thread. No task and no stack per
connection: an idle keep-alive connection costs its object, which is why
this is the engine for many connections. Idle connections are closed
after 75 s (`IdleTimeoutMs`).

## Sources

| File | Contents |
|---|---|
| `src/httploopserver.pas` | the program: options and serving |
| `src/lib/site.pas` | the site: its routes, handlers and middleware (shared with httpasyncserver) |
| `public/` | the pages served by default |

## Limits

An example: no TLS, no chunked bodies, a file is read into memory (on the
loop's thread) before it is sent, and every request is logged with a
flush — fine for a demo, not for a benchmark.
