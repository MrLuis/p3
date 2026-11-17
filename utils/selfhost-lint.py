#!/usr/bin/env python3
"""Checks the mechanical rules of selfhost/DIALECT.md in Pascal sources:

  * a comment must not contain '{', '}', '(*' or '*)' (they would end it
    early; FPC's objfpc mode would nest them);
  * the identifiers integer, with, goto, label and result, and FPC's
    objfpc reserved words and modifiers, are not used;
  * a function without parameters does not use its own name in its body
    except as an assignment target ("f := ..."): FPC's objfpc mode reads
    the name as the result variable, p3 as a recursive call.

Usage: utils/selfhost-lint.py file.pas ...   (exit 1 on a violation)
"""
import re, sys

BANNED = {"integer", "with", "goto", "label", "result",
          # reserved words / modifiers of FPC's other modes (DIALECT.md section 8)
          "as", "class", "except", "exports", "finally", "inherited", "is",
          "library", "on", "operator", "out", "property", "raise",
          "resourcestring", "threadvar", "try"}

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

def lint_self_reference(path, text):
    """Top-level functions without parameters (\"function f: T;\" at the
    start of a line) must not mention f in their bodies except as "f :=".
    A body ends at the next routine or section at the start of a line."""
    errors = []
    lines = text.split("\n")
    header = re.compile(r"^function\s+([A-Za-z_]\w*)\s*:")
    stop = re.compile(r"^(function|procedure|implementation|begin|var|const|type|end\.)\b", re.I)
    i = 0
    while i < len(lines):
        m = header.match(lines[i])
        if not m:
            i += 1
            continue
        name = m.group(1)
        j = i + 1
        while j < len(lines) and not stop.match(lines[j]):
            code = re.sub(r"'(?:[^']|'')*'", "''", lines[j].split("//")[0])
            code = re.sub(r"\{[^}]*\}", "", code)
            for occ in re.finditer(r"\b%s\b" % re.escape(name), code, re.I):
                if not re.match(r"\s*:=", code[occ.end():]):
                    errors.append("%s:%d: '%s' has no parameters and refers to "
                                  "itself (a recursive call in p3, the result "
                                  "in FPC objfpc)" % (path, j + 1, name))
            j += 1
        i = j
    return errors

def main():
    errors = []
    for path in sys.argv[1:]:
        errors += lint(path)
        errors += lint_self_reference(path, open(path, encoding="latin-1").read())
    for e in errors:
        print(e)
    return 1 if errors else 0

if __name__ == "__main__":
    sys.exit(main())
