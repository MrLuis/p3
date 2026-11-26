#!/usr/bin/env python3
"""diff for the lit tests on Windows (tests/lit.cfg.py uses it there):

    lit-diff-crlf.py <file or -> <file or ->

Compares the two as bytes, with CR LF read as LF: Windows programs end
their lines with CR LF and the expected files with LF. lit's own diff has
--strip-trailing-cr, but it compares output that is not text in the
locale's encoding or UTF-8 (a program that writes bytes above 127) as
binary, where that option does nothing. Exit status: 0 when the same, 1
(with a unified diff) when not, 2 on error.
"""
import difflib
import sys


def lines(path):
    if path == "-":
        data = sys.stdin.buffer.read()
    else:
        with open(path, "rb") as f:
            data = f.read()
    return data.replace(b"\r\n", b"\n").splitlines(keepends=True)


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    a, b = sys.argv[1], sys.argv[2]
    try:
        la, lb = lines(a), lines(b)
    except OSError as e:
        sys.stderr.write("lit-diff-crlf: %s\n" % e)
        return 2
    if la == lb:
        return 0
    for line in difflib.diff_bytes(difflib.unified_diff, la, lb,
                                   a.encode(), b.encode()):
        sys.stdout.buffer.write(line if line.endswith(b"\n") else line + b"\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
