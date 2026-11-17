# httpserver

A basic HTTP server written in Pascal: it serves the files of a directory
and one page of its own (`/status`).

```sh
cd examples/httpserver
../p3                     # builds bin/httpserver
bin/httpserver             # http://127.0.0.1:8080/, serving public/
```

```
httpserver [--port N] [--root DIR] [--any] [--requests N]
```

| Option | Meaning |
|---|---|
| `--port N` | the port to listen on (default 8080; 0: any free port, printed at the start) |
| `--root DIR` | the directory to serve (default `public`) |
| `--any` | listen on every address, not only 127.0.0.1 |
| `--requests N` | stop after N requests (default: run until interrupted) |

It answers `GET` and `HEAD`: a file under the root with a content type by
its extension, a directory's `index.html`, `/status` (requests served, time
running), and an error page otherwise (400, 403 for a path with `..`, 404,
405 for other methods). Each request is logged as `GET /hello.txt 200`.

## Sources

| File | Contents |
|---|---|
| `src/httpserver.pas` | the program: options, the accept loop, files and the status page |
| `src/lib/http.pas` | reading and parsing a request, sending a response, URL decoding, content types |
| `src/lib/sockets.pas` | TCP sockets through the C library (`socket`, `bind`, `listen`, `accept`, `recv`, `send`), for Linux and macOS |
| `public/` | the pages served by default |

## Limits

It is an example, not a server to put on the internet: one connection at
a time (a slow client holds up the others, and there are no timeouts), a
request's headers are read but not used and its body is ignored, each
connection is
closed after one response, a file is read into memory before it is sent,
and there is no TLS.
