#!/usr/bin/env python3
"""Drives netasync_demo: starts it (port 0), connects 4 clients (two
in flight at the same time, two after), verifies the echoes and the
clean stop. Prints the lines the lit test checks."""

import json
import os
import socket
import subprocess
import sys
import threading
import time

binary = sys.argv[1]
proc = subprocess.Popen([binary, "--requests", "4"],
                        stdout=subprocess.PIPE, text=True)

port = None
for _ in range(100):
    line = proc.stdout.readline()
    if line.startswith("listening on port "):
        port = int(line.split()[-1])
        break
if port is None:
    print("listening: FAIL")
    sys.exit(1)
print("listening: ok")

results = {}

def client(nth, text):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.sendall((text + "\n").encode())
    got = b""
    while not got.endswith(b"\r\n"):
        d = s.recv(1024)
        if not d:
            break
        got += d
    s.close()
    results[nth] = got.decode().strip().strip("\r")

# two at once, then two more (they are handled concurrently either way)
ts = [threading.Thread(target=client, args=(1, "one")),
      threading.Thread(target=client, args=(2, "second line"))]
for t in ts:
    t.start()
ts[0].join(); ts[1].join()
for n, s in ((3, "third"), (4, "the fourth!")):
    client(n, s)

for n in sorted(results):
    print(f"echo {n}: {results[n]}")

# the server stops itself once --requests are served
for _ in range(100):
    line = proc.stdout.readline()
    if line.startswith("server stopped"):
        print("server stopped: ok")
        break
else:
    print("server stopped: FAIL")
    proc.kill()
    sys.exit(1)
rc = proc.wait(timeout=10)
if rc != 0:
    print("exit status: FAIL")
    sys.exit(1)
