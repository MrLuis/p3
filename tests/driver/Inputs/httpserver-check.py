#!/usr/bin/env python3
"""Starts examples/httpserver on a free port, sends it requests and checks
the answers.  Usage: httpserver-check.py <server executable> <root dir>"""
import http.client, os, subprocess, sys

server, root = sys.argv[1], sys.argv[2]
blob = bytes(range(256)) * 4096  # 1 MiB with every byte value
with open(os.path.join(root, "blob.bin"), "wb") as f:
    f.write(blob)

proc = subprocess.Popen([server, "--port", "0", "--root", root, "--requests", "8"],
                        stdout=subprocess.PIPE, text=True)
line = proc.stdout.readline()
assert "http://127.0.0.1:" in line, line
port = int(line.rsplit(":", 1)[1].strip().rstrip("/"))

def request(method, path):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request(method, path)
    r = c.getresponse()
    body = r.read()
    c.close()
    return r.status, r.getheader("Content-Type"), body

checks = [
    ("GET", "/", 200, "text/html; charset=utf-8", b"It works"),
    ("GET", "/hello.txt", 200, "text/plain; charset=utf-8", b"Hello from pcc httpserver\n"),
    ("GET", "/hello%2Etxt?x=1", 200, "text/plain; charset=utf-8", b"Hello"),
    ("GET", "/status", 200, "text/html; charset=utf-8", b"requests served: 3"),
    ("GET", "/missing", 404, "text/html; charset=utf-8", b"404 Not Found"),
    ("GET", "/%2e%2e/pcc.json", 403, "text/html; charset=utf-8", b"403 Forbidden"),
    ("POST", "/", 405, "text/html; charset=utf-8", b"405 Method Not Allowed"),
]
for method, path, status, ctype, text in checks:
    got = request(method, path)
    assert got[0] == status and got[1] == ctype and text in got[2], (method, path, got[:2])
    print(method, path, got[0])
status, ctype, body = request("GET", "/blob.bin")
assert status == 200 and ctype == "application/octet-stream" and body == blob
print("GET /blob.bin", status, len(body))
log = proc.stdout.read()
assert proc.wait(timeout=10) == 0
assert "stopped after 8 request(s)" in log, log
print("server stopped")
