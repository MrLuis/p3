#!/usr/bin/env python3
"""Runs the benchmarks of bench/ and prints run time, executable size and
compile time (optimization.md, item 0).

Each bench/<name>.pas is compiled with --mode=objfpc in every configuration,
run --runs times (the best time counts) and its output compared with
bench/<name>.expected, so a benchmark that computes something else fails
instead of looking fast. Configurations:

  O0        -O0
  O2        -O2 (runtime checks on, as by default)
  O2-fast   -O2 -fno-range-checks -fno-overflow-checks

With --fpc, Free Pascal's -O2 (which has no checks: compare with O2-fast)
is measured too. With --selfhost-flags (default: build/selfhost/flags.txt
if it exists), the self-hosted compiler is built at -O2 and timed
compiling its own source to LLVM IR: the large, realistic case.

Usage:
  utils/bench.py --p3 build/bin/p3 [--base other/bin/p3] [--fpc]
      [--runs 3] [--configs O0,O2,O2-fast] [--only name,...]
      [--json out.json] [--update]

--base measures a second p3 (an older build) and prints how the first one
compares. --update rewrites the .expected files from the first
configuration's output. Exit status 1 if a benchmark fails to build, run
or give the expected output.
"""
import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = os.path.join(ROOT, "bench")
CONFIGS = {
    "O0": ["-O0"],
    "O2": ["-O2"],
    "O2-fast": ["-O2", "-fno-range-checks", "-fno-overflow-checks"],
}


def timed(cmd, stdout=subprocess.DEVNULL, **kw):
    start = time.perf_counter()
    r = subprocess.run(cmd, stdout=stdout, stderr=subprocess.PIPE, **kw)
    return time.perf_counter() - start, r


def best_run(exe, runs):
    """The best of `runs` times of exe (output discarded), in seconds."""
    best = None
    for _ in range(runs):
        t, r = timed([exe])
        if r.returncode != 0:
            return None
        best = t if best is None or t < best else best
    return best


def measure(compile_cmd, exe, expected, runs, update, env=None):
    """Compiles and runs one benchmark; a dict of its numbers or an error."""
    ctime, r = timed(compile_cmd, env=env)
    if r.returncode != 0 or not os.path.exists(exe):
        return {"error": "does not compile: " + r.stderr.decode(errors="replace").strip()[:200]}
    out = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode != 0:
        return {"error": "exit status %d" % out.returncode}
    if update:
        with open(expected, "wb") as f:
            f.write(out.stdout)
    elif os.path.exists(expected):
        with open(expected, "rb") as f:
            if f.read() != out.stdout:
                return {"error": "wrong output"}
    run = best_run(exe, runs)
    if run is None:
        return {"error": "fails when run again"}
    return {"run": run, "compile": ctime, "size": os.path.getsize(exe)}


def bench_programs(label, p3, configs, names, runs, work, update, env):
    results = {}
    for name in names:
        src = os.path.join(BENCH, name + ".pas")
        expected = os.path.join(BENCH, name + ".expected")
        for i, cfg in enumerate(configs):
            exe = os.path.join(work, "%s-%s-%s" % (label, name, cfg))
            cmd = [p3, "--no-project", "--mode=objfpc"] + CONFIGS[cfg] + [src, "-o", exe]
            results[(name, cfg)] = measure(cmd, exe, expected, runs, update and i == 0, env)
            sys.stderr.write(".")
            sys.stderr.flush()
    return results


def bench_fpc(fpc, names, runs, work):
    results = {}
    for name in names:
        d = os.path.join(work, "fpc-" + name)
        os.makedirs(d, exist_ok=True)
        shutil.copy(os.path.join(BENCH, name + ".pas"), d)
        exe = os.path.join(d, name)
        cmd = [fpc, "-Mobjfpc", "-O2", "-v0", "-o" + exe, os.path.join(d, name + ".pas")]
        results[(name, "fpc")] = measure(cmd, exe, os.path.join(BENCH, name + ".expected"),
                                         runs, False)
        sys.stderr.write(".")
        sys.stderr.flush()
    return results


def bench_selfhost(label, p3, flags_file, runs, work, env):
    """Builds the self-hosted compiler at -O2 and times it compiling itself."""
    with open(flags_file) as f:
        flags = shlex.split(f.read())
    src = os.path.join(ROOT, "selfhost", "p3.pas")
    exe = os.path.join(work, label + "-selfhost")
    cmd = [p3, "--no-project", "--mode=tp", "-O2"] + flags + \
          ["-I", os.path.join(ROOT, "selfhost"), src, "-o", exe]
    ctime, r = timed(cmd, env=env)
    if r.returncode != 0:
        return {"error": "does not compile: " + r.stderr.decode(errors="replace").strip()[:200]}
    # The include directories of the flags (generated LLVM tables), not
    # its link options.
    includes = []
    for i, flag in enumerate(flags):
        if flag == "-I" and i + 1 < len(flags):
            includes += ["-I", flags[i + 1]]
        elif flag.startswith("-I") and len(flag) > 2:
            includes.append(flag)
    best = None
    for _ in range(runs):
        t, r = timed([exe, "--no-project", "--mode=tp"] + includes +
                     ["-I", os.path.join(ROOT, "selfhost"), src, "--emit-llvm",
                      "-o", os.devnull], env=env)
        if r.returncode != 0:
            return {"error": "the compiler it builds fails: " +
                    r.stderr.decode(errors="replace").strip()[:200]}
        best = t if best is None or t < best else best
    return {"run": best, "compile": ctime, "size": os.path.getsize(exe)}


def fmt_time(res):
    return "%8.3f" % res["run"] if "error" not in res else "    FAIL"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--p3", required=True)
    ap.add_argument("--base", help="a second p3 to compare with")
    ap.add_argument("--fpc", action="store_true", help="also measure Free Pascal (-O2)")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--configs", default="O0,O2,O2-fast")
    ap.add_argument("--only", help="comma-separated benchmark names")
    ap.add_argument("--selfhost-flags",
                    default=os.path.join(ROOT, "build", "selfhost", "flags.txt"))
    ap.add_argument("--no-selfhost", action="store_true")
    ap.add_argument("--json", help="write the numbers to this file")
    ap.add_argument("--update", action="store_true",
                    help="rewrite the .expected files")
    args = ap.parse_args()

    configs = args.configs.split(",")
    for c in configs:
        if c not in CONFIGS:
            ap.error("unknown configuration '%s' (%s)" % (c, ", ".join(CONFIGS)))
    names = sorted(f[:-4] for f in os.listdir(BENCH) if f.endswith(".pas"))
    if args.only:
        names = [n for n in names if n in args.only.split(",")]
    selfhost = not args.no_selfhost and os.path.exists(args.selfhost_flags) and \
        (not args.only or "selfhost" in args.only.split(","))

    compilers = [("p3", os.path.abspath(args.p3))]
    if args.base:
        compilers.append(("base", os.path.abspath(args.base)))
    work = tempfile.mkdtemp(prefix="p3-bench-")
    results = {}
    failed = False
    try:
        for label, p3 in compilers:
            # Each p3 with the unit library and runtime installed next to it.
            env = dict(os.environ)
            env.pop("P3_UNIT_DIR", None)
            env.pop("P3_RUNTIME_LIB", None)
            results[label] = bench_programs(label, p3, configs, names, args.runs, work,
                                            args.update and label == "p3", env)
            if selfhost:
                results[label][("selfhost", "O2")] = bench_selfhost(
                    label, p3, args.selfhost_flags, args.runs, work, env)
        if args.fpc:
            fpc = shutil.which("fpc")
            if not fpc:
                sys.stderr.write("\nbench: fpc not found\n")
            else:
                results["fpc"] = bench_fpc(fpc, names, args.runs, work)
        sys.stderr.write("\n")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    # Run times.
    cols = [(label, cfg) for label, _ in compilers for cfg in configs]
    if "fpc" in results:
        cols.append(("fpc", "fpc"))
    print("Run time, seconds (best of %d)" % args.runs)
    print("%-12s" % "" + "".join("%13s" % (cfg if label == "p3" else label + " " + cfg
                                             if label == "base" else "fpc -O2")
                                 for label, cfg in cols))
    rows = names + (["selfhost"] if selfhost else [])
    for name in rows:
        line = "%-12s" % name
        for label, cfg in cols:
            res = results[label].get((name, cfg))
            if res is None:
                line += "%13s" % "-"
                continue
            if "error" in res:
                failed = True
                sys.stderr.write("bench: %s (%s %s): %s\n" % (name, label, cfg, res["error"]))
            line += "     " + fmt_time(res)
        print(line)
    if selfhost:
        print("(selfhost: the self-hosted compiler, built at -O2, compiling itself to IR)")

    if args.base:
        print("\np3 against base (run time; below 1.00 is faster)")
        print("%-12s" % "" + "".join("%10s" % cfg for cfg in configs))
        for name in rows:
            line = "%-12s" % name
            for cfg in configs:
                a = results["p3"].get((name, cfg))
                b = results["base"].get((name, cfg))
                if not a or not b or "error" in a or "error" in b:
                    line += "%10s" % "-"
                else:
                    line += "%10.2f" % (a["run"] / b["run"])
            print(line)

    print("\nCompile time, seconds, and executable size, KiB (p3)")
    print("%-12s" % "" + "".join("%18s" % cfg for cfg in configs))
    for name in rows:
        line = "%-12s" % name
        for cfg in configs:
            res = results["p3"].get((name, cfg))
            if res is None or "error" in res:
                line += "%18s" % "-"
            else:
                line += "%9.2f %8d" % (res["compile"], res["size"] // 1024)
        print(line)

    if args.json:
        with open(args.json, "w") as f:
            json.dump({label: {"%s/%s" % k: v for k, v in r.items()}
                       for label, r in results.items()}, f, indent=1, sort_keys=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
