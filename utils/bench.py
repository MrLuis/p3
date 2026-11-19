#!/usr/bin/env python3
"""Runs the benchmarks of bench/ and prints run time, executable size and
compile time (optimization.md, item 0).

Each bench/<name>.pas is compiled with --mode=objfpc in every configuration,
run --runs times (the best time counts) and its output compared with
bench/<name>.expected, so a benchmark that computes something else fails
instead of looking fast. A large output (over 4 KB) is kept as its SHA-256
instead, in bench/<name>.expected.sha256. Configurations:

  O0        -O0
  O2        -O2 (runtime checks on, as by default)
  O2-fast   -O2 -fno-range-checks -fno-overflow-checks

With --fpc, Free Pascal's -O2 (which has no checks: compare with O2-fast)
is measured too.

Compile time (optimization3.md, item 3): the self-hosted compiler is built
at -O2 by each p3, then both compilers, the C++ p3 and the self-hosted one
it built, compile selfhost/p3.pas (~56,000 lines) at each level of
--selfhost-levels, best of --selfhost-runs (at least 5 for a compile
under 5 s, where one run is noisy):

  IR        -O0 --emit-llvm (the front end)
  O0, O1, O2  that level, to an object file (-c)

The build flags are --selfhost-flags, by default selfhost/flags.txt of each
p3's build directory (<build>/bin/p3), else build/selfhost/flags.txt;
without them (or with --no-selfhost) this part is skipped.

Usage:
  utils/bench.py --p3 build/bin/p3 [--base other/bin/p3] [--fpc]
      [--runs 3] [--configs O0,O2,O2-fast] [--only name,...]
      [--selfhost-runs 1] [--selfhost-levels IR,O0,O1,O2]
      [--json out.json] [--update]

--base measures a second p3 (an older build) and prints how the first one
compares, in run time and in compile time. --update rewrites the .expected (or .expected.sha256) files from
the first configuration's output. Exit status 1 if a benchmark fails to build, run
or give the expected output.
"""
import argparse
import hashlib
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
SELFHOST_LEVELS = {
    "IR": ["-O0", "--emit-llvm"],
    "O0": ["-O0", "-c"],
    "O1": ["-O1", "-c"],
    "O2": ["-O2", "-c"],
}
# The two compilers of the compile-time table: the p3 under test, and the
# self-hosted compiler it builds.
SELFHOST_COMPILERS = [("cpp", "C++ p3"), ("self", "self-hosted p3")]
# A compile of selfhost/p3.pas shorter than QUICK_BELOW seconds is
# repeated at least QUICK_RUNS times.
QUICK_BELOW = 5.0
QUICK_RUNS = 5


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


# Outputs larger than this are kept as their SHA-256 (<name>.expected.sha256).
HASH_ABOVE = 4096


def write_expected(expected, output):
    """Records a benchmark's output: the text, or its hash if large."""
    hashed = expected + ".sha256"
    if len(output) > HASH_ABOVE:
        with open(hashed, "w") as f:
            f.write(hashlib.sha256(output).hexdigest() + "\n")
        if os.path.exists(expected):
            os.remove(expected)
    else:
        with open(expected, "wb") as f:
            f.write(output)
        if os.path.exists(hashed):
            os.remove(hashed)


def matches_expected(expected, output):
    """Does output match the recorded one (True when none is recorded)?"""
    if os.path.exists(expected):
        with open(expected, "rb") as f:
            return f.read() == output
    hashed = expected + ".sha256"
    if os.path.exists(hashed):
        with open(hashed) as f:
            return f.read().strip() == hashlib.sha256(output).hexdigest()
    return True


def measure(compile_cmd, exe, expected, runs, update, env=None):
    """Compiles and runs one benchmark; a dict of its numbers or an error."""
    ctime, r = timed(compile_cmd, env=env)
    if r.returncode != 0 or not os.path.exists(exe):
        return {"error": "does not compile: " + r.stderr.decode(errors="replace").strip()[:200]}
    out = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if out.returncode != 0:
        return {"error": "exit status %d" % out.returncode}
    if update:
        write_expected(expected, out.stdout)
    elif not matches_expected(expected, out.stdout):
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


def build_dir(p3):
    """The build directory of a p3 at <build>/bin/p3."""
    return os.path.dirname(os.path.dirname(os.path.abspath(p3)))


def selfhost_flags_file(p3, override):
    """The self-hosted build flags for this p3, or None."""
    if override:
        return override
    for f in (os.path.join(build_dir(p3), "selfhost", "flags.txt"),
              os.path.join(ROOT, "build", "selfhost", "flags.txt")):
        if os.path.exists(f):
            return f
    return None


def error_text(what, r):
    return what + ": " + r.stderr.decode(errors="replace").strip()[:200]


def bench_selfhost(label, p3, flags_file, levels, runs, work, env):
    """Builds the self-hosted compiler at -O2 with p3, then times p3 and
    the compiler it built compiling selfhost/p3.pas at each level.
    Results: ("selfhost:build", "O2") and ("selfhost:<compiler>", level)."""
    with open(flags_file) as f:
        flags = shlex.split(f.read())
    srcdir = os.path.join(ROOT, "selfhost")
    src = os.path.join(srcdir, "p3.pas")
    exe = os.path.join(work, label + "-selfhost")
    cmd = [p3, "--no-project", "--mode=tp", "-O2"] + flags + ["-I", srcdir, src, "-o", exe]
    ctime, r = timed(cmd, env=env)
    if r.returncode != 0:
        return {("selfhost:build", "O2"): {"error": error_text("does not compile", r)}}
    results = {("selfhost:build", "O2"): {"compile": ctime, "size": os.path.getsize(exe)}}
    sys.stderr.write(".")
    sys.stderr.flush()
    # The include directories of the flags (generated LLVM tables), not
    # its link options.
    includes = []
    for i, flag in enumerate(flags):
        if flag == "-I" and i + 1 < len(flags):
            includes += ["-I", flags[i + 1]]
        elif flag.startswith("-I") and len(flag) > 2:
            includes.append(flag)
    # The self-hosted compiler is not next to a lib/ directory: give it p3's.
    lib = os.path.join(build_dir(p3), "lib")
    self_env = dict(env, P3_RUNTIME_LIB=os.path.join(lib, "libpascalrt.a"),
                    P3_UNIT_DIR=os.path.join(lib, "pascal"))
    for key, compiler, cenv in (("cpp", p3, env), ("self", exe, self_env)):
        for level in levels:
            out = os.path.join(work, "%s-%s-%s.out" % (label, key, level))
            cmd = [compiler, "--no-project", "--mode=tp"] + SELFHOST_LEVELS[level] + \
                includes + ["-I", srcdir, src, "-o", out]
            best = None
            n = 0
            # Short compiles are noisy: at least QUICK_RUNS of them.
            while n < max(runs, 1) or (n < QUICK_RUNS and best["compile"] < QUICK_BELOW):
                t, r = timed(cmd, env=cenv)
                if r.returncode != 0:
                    best = {"error": error_text("fails", r)}
                    break
                best = {"compile": t} if best is None or t < best["compile"] else best
                n += 1
            results[("selfhost:" + key, level)] = best
            sys.stderr.write(".")
            sys.stderr.flush()
    return results


def fmt_time(res):
    return "%8.3f" % res["run"] if "error" not in res else "    FAIL"


def print_selfhost(compilers, results, levels, runs):
    """Prints the compile-time table of selfhost/p3.pas (and, with a base,
    the ratios); True if anything failed."""
    failed = False
    print("\nCompiling selfhost/p3.pas, seconds (best of %d, of %d under %g s; "
          "IR: -O0 --emit-llvm, else to an object file)"
          % (runs, max(runs, QUICK_RUNS), QUICK_BELOW))
    print("%-22s" % "" + "".join("%9s" % lv for lv in levels))
    for label, _ in compilers:
        build = results[label].get(("selfhost:build", "O2"))
        if "error" in build:
            failed = True
            sys.stderr.write("bench: selfhost (%s, building it): %s\n" % (label, build["error"]))
        for key, name in SELFHOST_COMPILERS:
            line = "%-22s" % ((label + " " if label != "p3" else "") + name)
            for lv in levels:
                res = results[label].get(("selfhost:" + key, lv))
                if res is None:
                    line += "%9s" % "-"
                elif "error" in res:
                    failed = True
                    sys.stderr.write("bench: selfhost (%s %s %s): %s\n"
                                     % (label, name, lv, res["error"]))
                    line += "%9s" % "FAIL"
                else:
                    line += "%9.2f" % res["compile"]
            print(line)
    for label, _ in compilers:
        build = results[label][("selfhost:build", "O2")]
        if "error" not in build:
            print("(%sself-hosted p3: built at -O2 in %.1f s, %d KiB)"
                  % (label + " " if label != "p3" else "", build["compile"],
                     build["size"] // 1024))

    if len(compilers) > 1:
        print("\np3 against base (compile time of selfhost/p3.pas; below 1.00 is faster)")
        print("%-22s" % "" + "".join("%9s" % lv for lv in levels))
        for key, name in SELFHOST_COMPILERS:
            line = "%-22s" % name
            for lv in levels:
                a = results["p3"].get(("selfhost:" + key, lv))
                b = results["base"].get(("selfhost:" + key, lv))
                if not a or not b or "error" in a or "error" in b:
                    line += "%9s" % "-"
                else:
                    line += "%9.2f" % (a["compile"] / b["compile"])
            print(line)
        a = results["p3"][("selfhost:build", "O2")]
        b = results["base"][("selfhost:build", "O2")]
        if "error" not in a and "error" not in b:
            print("%-22s%9.2f" % ("build (-O2, linked)", a["compile"] / b["compile"]))
    return failed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--p3", required=True)
    ap.add_argument("--base", help="a second p3 to compare with")
    ap.add_argument("--fpc", action="store_true", help="also measure Free Pascal (-O2)")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--configs", default="O0,O2,O2-fast")
    ap.add_argument("--only", help="comma-separated benchmark names")
    ap.add_argument("--selfhost-flags",
                    help="build flags of the self-hosted compiler (default: "
                         "selfhost/flags.txt of each p3's build directory)")
    ap.add_argument("--selfhost-runs", type=int, default=1)
    ap.add_argument("--selfhost-levels", default="IR,O0,O1,O2")
    ap.add_argument("--no-selfhost", action="store_true")
    ap.add_argument("--json", help="write the numbers to this file")
    ap.add_argument("--update", action="store_true",
                    help="rewrite the .expected (.expected.sha256) files")
    args = ap.parse_args()

    configs = args.configs.split(",")
    for c in configs:
        if c not in CONFIGS:
            ap.error("unknown configuration '%s' (%s)" % (c, ", ".join(CONFIGS)))
    names = sorted(f[:-4] for f in os.listdir(BENCH) if f.endswith(".pas"))
    if args.only:
        names = [n for n in names if n in args.only.split(",")]
    levels = args.selfhost_levels.split(",")
    for lv in levels:
        if lv not in SELFHOST_LEVELS:
            ap.error("unknown level '%s' (%s)" % (lv, ", ".join(SELFHOST_LEVELS)))

    compilers = [("p3", os.path.abspath(args.p3))]
    if args.base:
        compilers.append(("base", os.path.abspath(args.base)))
    flags_files = {label: selfhost_flags_file(p3, args.selfhost_flags)
                   for label, p3 in compilers}
    selfhost = not args.no_selfhost and flags_files["p3"] is not None and \
        (not args.only or "selfhost" in args.only.split(","))
    if not args.no_selfhost and flags_files["p3"] is None:
        sys.stderr.write("bench: no selfhost/flags.txt: compile time of "
                         "selfhost/p3.pas not measured\n")
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
                if flags_files[label] is None:
                    results[label][("selfhost:build", "O2")] = {
                        "error": "no selfhost/flags.txt for " + p3}
                else:
                    results[label].update(bench_selfhost(
                        label, p3, flags_files[label], levels, args.selfhost_runs,
                        work, env))
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
    rows = names
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

    if selfhost and print_selfhost(compilers, results, levels, args.selfhost_runs):
        failed = True

    if args.json:
        with open(args.json, "w") as f:
            json.dump({label: {"%s/%s" % k: v for k, v in r.items()}
                       for label, r in results.items()}, f, indent=1, sort_keys=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
