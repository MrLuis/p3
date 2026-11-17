#!/usr/bin/env python3
"""Runs the Pascal-P5 ISO 7185 conformance test (iso7185pat.pas) with p3.

The test is self-checking: each line prints `<result> s/b <expected>`. Real
numbers are compared numerically (their exponent format is implementation
defined), everything else as whitespace-normalized text.

Usage:  utils/iso7185pat.py <p3> [workdir]
Downloads iso7185pat.pas/.inp from github.com/samiam95124/Pascal-P5 into
workdir (default: ./iso7185pat-work) unless already present.
"""
import os, re, subprocess, sys, urllib.request

BASE = "https://raw.githubusercontent.com/samiam95124/Pascal-P5/master/standard_tests/"

def norm(tok):
    try:
        return float(tok)
    except ValueError:
        return tok.lower()  # boolean case is implementation defined

def same(got, exp):
    g = [norm(t) for t in got.split()]
    e = [norm(t) for t in exp.split()]
    if len(g) != len(e):
        return False
    for a, b in zip(g, e):
        if isinstance(a, float) and isinstance(b, float):
            if abs(a - b) > 1e-4 * max(1.0, abs(b)):
                return False
        elif a != b:
            return False
    return True

# Lines whose "s/b" text is not a plain expectation (checked by hand):
QUIRKS = {
    # The test itself prints ' start s/b start good' after 'start good'.
    "Control17: start good start s/b start good",
    # Pointer21 prints nothing by design; Pointer22 continues the line.
    "Pointer21:  Pointer22:  done s/b done",
}

def main():
    p3 = sys.argv[1]
    work = sys.argv[2] if len(sys.argv) > 2 else "iso7185pat-work"
    os.makedirs(work, exist_ok=True)
    for name in ("iso7185pat.pas", "iso7185pat.inp"):
        path = os.path.join(work, name)
        if not os.path.exists(path):
            urllib.request.urlretrieve(BASE + name, path)
    exe = os.path.join(work, "iso7185pat")
    subprocess.run([p3, os.path.join(work, "iso7185pat.pas"), "-o", exe], check=True)
    with open(os.path.join(work, "iso7185pat.inp")) as inp:
        out = subprocess.run([exe], stdin=inp, capture_output=True, text=True, check=True).stdout
    lines = out.splitlines()
    checks, bad = 0, []
    for i, line in enumerate(lines):
        if " s/b " not in line:
            continue
        checks += 1
        if line.strip() in QUIRKS:
            continue
        got, exp = line.split(" s/b ", 1)
        if ":" in got:
            got = got.split(":", 1)[1]
        elif got.split() and re.fullmatch(r"[A-Za-z]+[0-9]+", got.split()[0]):
            # A test name without its colon, e.g. "Real147   1.21E+005".
            got = got.split(None, 1)[1] if len(got.split()) > 1 else ""
        # Some tests print their result on the previous line.
        if not got.strip() and i > 0:
            got = lines[i - 1].split(":", 1)[-1]
        if not same(got, exp):
            bad.append(line)
    print(f"{checks} checks, {len(bad)} differences")
    for line in bad:
        print("  " + line)

if __name__ == "__main__":
    main()
