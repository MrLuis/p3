#!/usr/bin/env python3
"""diff for the lit tests on Windows (tests/lit.cfg.py uses it there):

    lit-diff-crlf.py <file or -> <file or ->

Compares the two as bytes, reading CR LF as LF in a program's output (stdin,
or any file but an .expected one): on Windows the C library writes a
program's text in text mode, turning every LF into CR LF, also the LF of a
CR LF the program printed itself (rtl-trim's), while the expected files
hold what the program meant, with LF. lit's own diff has
--strip-trailing-cr, but it strips both sides and compares output that is
not text in the locale's encoding or UTF-8 (bytes above 127) as binary,
where that option does nothing. Exit status: 0 when the same, 1 (with a
unified diff) when not, 2 on error.
"""
import difflib
import sys


def lines(path):
    if path == "-":
        data = sys.stdin.buffer.read()
    else:
        with open(path, "rb") as f:
            data = f.read()
    if not path.endswith(".expected"):
        data = data.replace(b"\r\n", b"\n")      # undo text mode
    return data.splitlines(keepends=True)


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
