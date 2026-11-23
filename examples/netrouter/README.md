# netrouter demo

A small HTTP/1.1 server whose routing comes from the **`netrouter` unit**,
p3's take on Go's [chi](https://go-chi.io/): routes by method and path
pattern, URL parameters, middleware, groups and sub-routers. The router does
no HTTP itself: this program reads the request line, hands the router the
method, the path and a `TConn` of its own, and the handlers answer through
`TConn`. Sockets come from `netasync`; each connection is a task on the pool.

What it shows:

- `specialize TRouter<TConn>` — the router is generic over the program's
  own context type
- `Get('/posts/{id:[0-9]+}')` next to `Get('/posts/{slug}')` — an expression
  parameter wins over a plain one; `R.URLParam('id')`
- `Use(@Logger)` — middleware wrapping every route (it prints the matched
  pattern, `R.RoutePattern`)
- `Using(@RequireToken).Get('/admin', ...)` — middleware for one route
- `Route('/api/v1', ...)` — a sub-router
- `NotFound` / `MethodNotAllowed` (with `R.AllowedMethods` for `Allow:`)
- `Walk` — `/api/v1/routes` lists every route
- `/slow/{ms}` — a handler that waits (`Wait(Slow(ms))`): it parks its
  connection's task, not the thread

## Build and run

```sh
cd examples/netrouter
p3                       # builds bin/netrouter_demo (p3.json)
bin/netrouter_demo       # or: bin/netrouter_demo PORT (default 8080)
```

```sh
curl localhost:8080/hello/ann
curl localhost:8080/posts/42          # post #42
curl localhost:8080/posts/abc         # post "abc"
curl -i -X DELETE localhost:8080/posts/42   # 405, Allow: GET
curl localhost:8080/admin -H 'X-Token: secret'
curl localhost:8080/api/v1/routes
```

The request reading here is deliberately minimal (no bodies, no chunking):
the point is the routing. See the header of `rtl/netrouter.pas` for the
pattern syntax and the full API.
