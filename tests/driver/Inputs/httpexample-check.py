#!/usr/bin/env python3
"""Starts examples/httpasyncserver or examples/httploopserver on a free port,
sends it requests (one keep-alive connection for most of them) and checks
the answers: files, the status page, URL parameters, the JSON sub-router,
errors (403, 404, 405 with Allow), HEAD, a 1 MiB file byte for byte, and
--requests stopping the server.
Usage: httpexample-check.py <server executable> <root dir> [--async]"""
import http.client, os, re, subprocess, sys

server, root = sys.argv[1], sys.argv[2]
is_async = "--async" in sys.argv[3:]
blob = bytes(range(256)) * 4096  # 1 MiB with every byte value
with open(os.path.join(root, "blob.bin"), "wb") as f:
    f.write(blob)

HTML = "text/html; charset=utf-8"
TEXT = "text/plain; charset=utf-8"
checks = [
    ("GET", "/", 200, HTML, b"It works"),
    ("GET", "/hello.txt", 200, TEXT, b"Hello from a p3 server"),
    ("GET", "/hello%2Etxt?x=1", 200, TEXT, b"Hello from a p3 server"),
    ("GET", "/status", 200, HTML, b"requests served: 3"),
    ("GET", "/hello/ann", 200, TEXT, b"Hello, ann!"),
    ("GET", "/posts/42", 200, TEXT, b"post #42"),
    ("GET", "/posts/abc", 404, HTML, b"404 Not Found"),
    ("GET", "/api/echo/hi", 200, "application/json", b'{"word":"hi"}'),
    ("GET", "/missing", 404, HTML, b"404 Not Found"),
    ("GET", "/%2e%2e/p3.json", 403, HTML, b"403 Forbidden"),
    ("POST", "/", 405, HTML, b"405 Method Not Allowed"),
    ("HEAD", "/hello.txt", 200, TEXT, b""),
]
if is_async:
    checks.append(("GET", "/sleep/50", 200, TEXT, b"slept 50 ms"))
total = len(checks) + 1  # and the blob

proc = subprocess.Popen([server, "--port", "0", "--root", root, "--requests", str(total)],
                        stdout=subprocess.PIPE, text=True)
line = proc.stdout.readline()
assert "http://127.0.0.1:" in line, line
port = int(re.search(r"http://127\.0\.0\.1:(\d+)/", line).group(1))

conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
for method, path, status, ctype, text in checks:
    conn.request(method, path)
    r = conn.getresponse()
    body = r.read()
    assert r.status == status and r.getheader("Content-Type") == ctype and text in body, \
        (method, path, r.status, r.getheader("Content-Type"), body[:80])
    extra = ""
    if status == 405:
        extra = " Allow: " + r.getheader("Allow")
    if method == "HEAD":
        assert body == b"" and r.getheader("Content-Length") == "23", r.getheaders()
        extra = " Content-Length: " + r.getheader("Content-Length")
    print(method, path, r.status, extra.strip()) if extra else print(method, path, r.status)
conn.request("GET", "/blob.bin")
r = conn.getresponse()
body = r.read()
assert r.status == 200 and r.getheader("Content-Type") == "application/octet-stream" and body == blob
print("GET /blob.bin", r.status, len(body))
conn.close()
log = proc.stdout.read()
assert proc.wait(timeout=10) == 0
assert "GET /hello/ann 200" in log and "POST / 405" in log, log
assert "stopped after %d request(s)" % total in log, log
print("server stopped")
