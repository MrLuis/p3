#!/usr/bin/env python3
"""Windows programs to run on Windows itself, built anywhere p3 cross-compiles.

    utils/windows-bundle.py <p3 command> <tests/run> <bundle dir>

Compiles each test of tests/run whose RUN lines only build the program,
run it, and compare its output with an .expected file:

    RUN: %p3 <options> %s -o %t
    RUN: [cd %t.dir &&] %t | diff - %S/NAME.expected
      or %t > %t.out, then diff %S/NAME.expected %t.out
    (and rm -rf / mkdir -p of %t.dir)

to <bundle dir>/NAME.exe, with the output it must print as NAME.expected
(NAME.windows.expected when the test has one). Programs with other RUN
lines are left to the Wine tests (tests/windows). The bundle is run with
utils/windows-run-bundle.sh. Only the first build of a program is taken.
"""
import os
import re
import shlex
import shutil
import subprocess
import sys

TRIPLE = "x86_64-w64-mingw32"


def plan(src):
    """The compile options and expected file of a simple test, or None."""
    with open(src, encoding="utf-8", errors="replace") as f:
        lines = [l.split("RUN:", 1)[1].strip() for l in f if "RUN:" in l]
    options, expected, ran = None, None, False
    for line in lines:
        if re.fullmatch(r"rm -rf %t\.dir( && mkdir -p %t\.dir)?|mkdir -p %t\.dir", line):
            continue
        toks = shlex.split(line)
        if toks[:1] == ["%p3"]:
            if toks[-3:] == ["%s", "-o", "%t"] or toks[-2:] == ["-o", "%t"]:
                if options is None:
                    options = [t for t in toks[1:] if t not in ("%s", "-o", "%t")]
                continue
            if any(t.startswith("%t.") for t in toks):
                continue  # another build (-O2 into %t.fast): its run is skipped below
            return None
        if toks[:3] == ["cd", "%t.dir", "&&"]:
            toks = toks[3:]
        if toks[:1] == ["%t"] and toks[1:4] == ["|", "diff", "-"] and len(toks) == 5:
            exp = toks[4].replace("%S/", "")
            if expected not in (None, exp):
                return None
            expected = exp
            continue
        if toks == ["%t", ">", "%t.out"]:
            ran = True
            continue
        if len(toks) == 3 and toks[0] == "diff" and toks[2] == "%t.out" and ran:
            exp = toks[1].replace("%S/", "")
            if expected not in (None, exp):
                return None
            expected = exp
            continue
        if toks and toks[0].startswith("%t.") and (
                toks[1:4] == ["|", "diff", "-"] or
                (len(toks) == 3 and toks[1] == ">" and toks[2] == toks[0] + ".out")):
            continue  # the run of another build
        if len(toks) == 3 and toks[0] == "diff" and re.fullmatch(r"%t\.\w+\.out", toks[2]):
            continue  # its output
        return None
    if options is None or expected is None:
        return None
    return options, expected


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    p3, run_dir, out = shlex.split(sys.argv[1]), sys.argv[2], sys.argv[3]
    os.makedirs(out, exist_ok=True)
    built, failed = 0, []
    for name in sorted(os.listdir(run_dir)):
        if not name.endswith(".pas"):
            continue
        src = os.path.join(run_dir, name)
        p = plan(src)
        if p is None:
            continue
        options, expected = p
        stem = name[:-4]
        exp = os.path.join(run_dir, expected)
        win = exp[:-len(".expected")] + ".windows.expected"
        if os.path.exists(win):
            exp = win
        options = [o.replace("%S", run_dir) for o in options]
        cmd = p3 + options + [src, "--target=" + TRIPLE, "-o",
                              os.path.join(out, stem + ".exe")]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            failed.append(stem)
            sys.stderr.write("$ %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))
            continue
        shutil.copyfile(exp, os.path.join(out, stem + ".expected"))
        built += 1
    print("windows-bundle: %d programs in %s" % (built, out))
    if failed:
        sys.exit("windows-bundle: could not build " + " ".join(failed))


if __name__ == "__main__":
    main()
