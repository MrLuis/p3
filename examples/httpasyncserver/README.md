# httpasyncserver

An HTTP/1.1 server on p3's **async engine**: each connection is a
`netasync` task on the thread pool, written as straight-line code (read a
request, route it, send the answer, again while the connection is kept
alive). Its routes come from the **`netrouter`** unit. The same site, on an
event loop instead, is [httploopserver](../httploopserver).

```sh
cd examples/httpasyncserver
p3                            # builds bin/httpasyncserver (p3.json)
bin/httpasyncserver            # http://127.0.0.1:8080/, serving public/
```

```
httpasyncserver [--port N] [--root DIR] [--any] [--requests N]
```

| Option | Meaning |
|---|---|
| `--port N` | the port to listen on (default 8080; 0: any free port, printed at the start) |
| `--root DIR` | the directory to serve (default `public`) |
| `--any` | listen on every address, not only 127.0.0.1 |
| `--requests N` | stop after N requests (default: run until interrupted) |

## The site

| Route | Answer |
|---|---|
| `GET /`, `GET /*` | a file under the root (a directory: its `index.html`); `..` is a 403 |
| `GET /status` | a page the server makes: requests served, time running |
| `GET /hello/{name}` | a URL parameter |
| `GET /posts/{id:[0-9]+}` | a parameter with an expression (`/posts/abc` falls through to `/*`) |
| `GET /api/echo/{word}`, `GET /api/routes` | a sub-router whose middleware answers JSON; `routes` lists them all (`Walk`) |
| `GET /sleep/{ms}` | **this engine only**: a handler that waits (`Wait(Pause(ms))`) |

`HEAD` works wherever `GET` does; another method on a known path is a 405
with `Allow:`; an unknown path a 404 page. Each request is logged
(`GET /hello/ann 200`) by a middleware.

## How it works

The program builds the site's router and hands it to the RTL:

```pascal
Wait(HttpServeAsync(listener, RouterHandler(SiteRouter), @Answered));
```

- `HttpServeAsync` (unit `nethttp`) accepts on the main thread and starts
  each connection as a task on the thread pool.
- The connection's task keeps its unused bytes, parses whole requests from
  them (`HttpParseRequest`: keep-alive, pipelining, `Content-Length` and
  chunked bodies, limits) and reads more with `await NetReadInto` only when
  it needs to; a quiet connection closes after 75 s.
- `RouterHandler` routes each request (`netrouter`): the handler gets the
  connection's `THttpContext`, reads the request from it and fills the
  answer in (`C.Text`, `C.Html`, `C.SetHeader`, ...); the task sends it.
  `@Answered` runs after each answer (it counts them, for `--requests`).
- `/sleep/{ms}` waits with `Wait(...)` inside its handler: that parks the
  connection's task, not the thread — run `curl localhost:8080/sleep/2000`
  twice at once and both answer in two seconds.

## Sources

| File | Contents |
|---|---|
| `src/httpasyncserver.pas` | the program: options, `/sleep`, serving |
| `src/lib/site.pas` | the site: its routes, handlers and middleware (shared with httploopserver) |
| `public/` | the pages served by default |

## Limits

An example, not a server to put on the internet: no TLS, no chunked
bodies, a file is read into memory before it is sent, and a task (with its
stack) per connection: see httploopserver (netloop) for the cheaper idle
connection.
