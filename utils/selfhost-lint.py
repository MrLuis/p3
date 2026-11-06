#!/usr/bin/env python3
"""Checks the mechanical rules of selfhost/DIALECT.md in Pascal sources:

  * a comment must not contain '{', '}', '(*' or '*)' (FPC's objfpc mode
    nests comments, Turbo Pascal and pcc do not);
  * the identifiers integer, with, goto, label and result are not used.

Usage: utils/selfhost-lint.py file.pas ...   (exit 1 on a violation)
"""
import re, sys

BANNED = {"integer", "with", "goto", "label", "result"}

def lint(path):
    text = open(path, encoding="latin-1").read()
    errors = []
    line = 1
    i, n = 0, len(text)
    def err(msg):
        errors.append("%s:%d: %s" % (path, line, msg))
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1; i += 1
        elif c == "'":                       # string literal
            j = text.find("'", i + 1)
            while j != -1 and j + 1 < n and text[j + 1] == "'":
                j = text.find("'", j + 2)
            if j == -1:
                break
            line += text.count("\n", i, j); i = j + 1
        elif text.startswith("//", i):       # line comment
            j = text.find("\n", i)
            i = n if j == -1 else j
        elif c == "{" or text.startswith("(*", i):
            close = "}" if c == "{" else "*)"
            start = i + (1 if c == "{" else 2)
            j = text.find(close, start)
            if j == -1:
                err("unterminated comment"); break
            body = text[start:j]
            for bad in ("{", "}", "(*", "*)"):
                if bad in body and not (bad == close):
                    err("comment contains '%s'" % bad)
            line += text.count("\n", i, j); i = j + len(close)
        elif c.isalpha() or c == "_":
            m = re.match(r"[A-Za-z_][A-Za-z0-9_]*", text[i:])
            word = m.group(0)
            if word.lower() in BANNED:
                err("'%s' is not allowed in the selfhost dialect" % word)
            i += len(word)
        else:
            i += 1
    return errors

def main():
    errors = []
    for path in sys.argv[1:]:
        errors += lint(path)
    for e in errors:
        print(e)
    return 1 if errors else 0

if __name__ == "__main__":
    sys.exit(main())
