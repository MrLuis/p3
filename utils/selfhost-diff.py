#!/usr/bin/env python3
"""Differential test: runs the C++ pcc and the self-hosted pcc with the same
arguments on many inputs and compares stdout, stderr and the exit status.

Usage:
  utils/selfhost-diff.py --ref build/bin/pcc --new <selfhost pcc> \\
      [--action=--emit-tokens] [--modes=iso,tp] [--jobs=N] [--show=5] PATH...

PATHs are files or directories (searched recursively for *.pas, or for
every file with --all-files, e.g. a fuzz corpus). Exit status 1 if any
input differs.
"""
import argparse, concurrent.futures, os, subprocess, sys

def inputs(paths, all_files):
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for f in sorted(files):
                    if all_files or f.endswith(".pas"):
                        yield os.path.join(root, f)
        else:
            yield p

def run(cmd):
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=60)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return "timeout", b"", b""

def compare(args, path, mode):
    common = [args.action, "--mode=" + mode, "-fdiagnostics-color=never", path]
    ref = run([args.ref] + common)
    new = run([args.new] + common)
    if ref == new:
        return None
    what = [n for n, a, b in zip(("exit status", "stdout", "stderr"), ref, new) if a != b]
    return (path, mode, what, ref, new)

def first_diff(a, b):
    al, bl = a.splitlines(), b.splitlines()
    for i, (x, y) in enumerate(zip(al, bl)):
        if x != y:
            return "line %d:\n      ref: %r\n      new: %r" % (i + 1, x[:200], y[:200])
    return "lengths differ: ref %d lines, new %d lines" % (len(al), len(bl))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--action", default="--emit-tokens")
    ap.add_argument("--modes", default="iso,tp")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--show", type=int, default=5)
    ap.add_argument("--all-files", action="store_true")
    ap.add_argument("paths", nargs="+")
    args = ap.parse_args()
    jobs = [(p, m) for p in inputs(args.paths, args.all_files)
            for m in args.modes.split(",")]
    diffs = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for d in ex.map(lambda j: compare(args, *j), jobs):
            if d:
                diffs.append(d)
    for path, mode, what, ref, new in diffs[:args.show]:
        print("DIFF %s (--mode=%s): %s" % (path, mode, ", ".join(what)))
        if ref[0] != new[0]:
            print("    exit: ref %s, new %s" % (ref[0], new[0]))
        for i, name in ((1, "stdout"), (2, "stderr")):
            if ref[i] != new[i]:
                print("    %s %s" % (name, first_diff(ref[i], new[i])))
    print("%d runs, %d differ" % (len(jobs), len(diffs)))
    return 1 if diffs else 0

if __name__ == "__main__":
    sys.exit(main())
