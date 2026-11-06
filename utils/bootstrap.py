#!/usr/bin/env python3
"""Bootstraps the self-hosted compiler (SELFHOST.md S9) and checks it.

  stage 0  the C++ pcc
  stage 1  selfhost/pcc.pas built by stage 0
  stage 2  selfhost/pcc.pas built by stage 1
  stage 3  selfhost/pcc.pas built by stage 2

Each stage is installed as <work>/stageN/bin/pcc with the runtime in
<work>/stageN/lib, where the compiler looks for it. Checks:

  1. stages 1, 2 and 3 write the same --emit-llvm output for their own
     source (stage 2 = stage 3 is the fixed point; stage 1 must agree too,
     as the Pascal compiler's output may not depend on who compiled it);
  2. stages 2 and 3 write the same IR for every .pas file of the
     repository, in both modes (--modules);
  3. the stage 2 and stage 3 executables are identical (where the linker
     is deterministic; --no-exe-compare skips this);
  4. stage 3 builds and runs hello.pas;
  5. with --fpc: an independent stage 1 built by Free Pascal leads to the
     same stage 2 IR.

Usage:
  utils/bootstrap.py --pcc build/bin/pcc --runtime build/lib/libpascalrt.a \\
      [--work build/bootstrap] [-O N] [--fpc] [--modules] [--no-exe-compare]
Exit status 0 if every check passes.
"""
import argparse
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SELFHOST = os.path.join(ROOT, "selfhost")
SOURCE = os.path.join(SELFHOST, "pcc.pas")


def log(msg):
    print("bootstrap: " + msg, flush=True)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write("bootstrap: command failed (%d): %s\n%s%s" %
                         (r.returncode, " ".join(cmd), r.stdout, r.stderr))
        sys.exit(1)
    return r


def install(work, name, runtime):
    """The directory layout of a stage; returns the compiler's path."""
    stage = os.path.join(work, name)
    os.makedirs(os.path.join(stage, "bin"), exist_ok=True)
    os.makedirs(os.path.join(stage, "lib"), exist_ok=True)
    shutil.copyfile(runtime, os.path.join(stage, "lib", "libpascalrt.a"))
    return os.path.join(stage, "bin", "pcc")


def build_stage(compiler, out, opt):
    run([compiler, "--mode=tp", "-O%d" % opt, "-I", SELFHOST, SOURCE, "-o", out])


def emit_ir(compiler, out):
    run([compiler, "--mode=tp", "-I", SELFHOST, SOURCE, "--emit-llvm", "-o", out])


def pas_files():
    for top in ("tests", "selfhost", "examples"):
        for dirpath, _, files in os.walk(os.path.join(ROOT, top)):
            for f in sorted(files):
                if f.endswith(".pas"):
                    yield os.path.join(dirpath, f)


def module_ir(compiler, path, mode):
    """--emit-llvm output, diagnostics and exit status for one file."""
    extra = []
    # Units of the run tests live next to their programs.
    units = os.path.join(ROOT, "tests", "run", "Inputs", "units", "lib")
    if mode == "tp":
        extra = ["-I", units]
    r = subprocess.run([compiler, "--mode=" + mode, "--emit-llvm"] + extra +
                       [path, "-o", "-"], capture_output=True, timeout=60)
    return r.returncode, r.stdout, r.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pcc", required=True, help="the C++ pcc (stage 0)")
    ap.add_argument("--runtime", required=True, help="libpascalrt.a")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "bootstrap"))
    ap.add_argument("-O", dest="opt", type=int, default=2,
                    help="optimization level of the stages (default 2)")
    ap.add_argument("--fpc", action="store_true",
                    help="also bootstrap from a Free Pascal stage 1")
    ap.add_argument("--modules", action="store_true",
                    help="compare stage 2 and 3 IR for every .pas file")
    ap.add_argument("--no-exe-compare", action="store_true",
                    help="do not require identical stage 2 and 3 executables")
    args = ap.parse_args()

    work = os.path.abspath(args.work)
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    failures = []

    stages = {0: os.path.abspath(args.pcc)}
    for n in (1, 2, 3):
        stages[n] = install(work, "stage%d" % n, args.runtime)
        log("building stage %d with stage %d" % (n, n - 1))
        build_stage(stages[n - 1], stages[n], args.opt)

    # 1. The compiler's own IR.
    irs = {}
    for n in (1, 2, 3):
        irs[n] = os.path.join(work, "stage%d.ll" % n)
        emit_ir(stages[n], irs[n])
    for a, b in ((1, 2), (2, 3)):
        if filecmp.cmp(irs[a], irs[b], shallow=False):
            log("stage %d and stage %d IR for pcc.pas: identical" % (a, b))
        else:
            failures.append("stage %d and stage %d write different IR for pcc.pas" % (a, b))

    # 2. Every module of the repository.
    if args.modules:
        n = differ = 0
        for path in pas_files():
            for mode in ("iso", "tp"):
                n += 1
                if module_ir(stages[2], path, mode) != module_ir(stages[3], path, mode):
                    differ += 1
                    if differ <= 5:
                        log("IR differs: %s (--mode=%s)" % (path, mode))
        log("stage 2 and stage 3 IR for %d module compilations: %d differ" % (n, differ))
        if differ:
            failures.append("%d module compilations differ" % differ)

    # 3. The executables.
    if filecmp.cmp(stages[2], stages[3], shallow=False):
        log("stage 2 and stage 3 executables: identical")
    elif args.no_exe_compare:
        log("stage 2 and stage 3 executables differ (not checked)")
    else:
        failures.append("stage 2 and stage 3 executables differ")

    # 4. Stage 3 works.
    with tempfile.TemporaryDirectory(prefix="pcc-bootstrap") as tmp:
        hello = os.path.join(tmp, "hello")
        run([stages[3], os.path.join(ROOT, "tests", "driver", "Inputs", "hello.pas"),
             "-o", hello])
        out = run([hello]).stdout
        if out.strip() != "Hello, World!":
            failures.append("stage 3's hello printed %r" % out)
        else:
            log("stage 3 builds and runs hello.pas")

    # 5. The Free Pascal path.
    if args.fpc:
        fpc = shutil.which("fpc")
        if not fpc:
            failures.append("--fpc: fpc not found")
        else:
            units = os.path.join(work, "fpc-units")
            os.makedirs(units)
            s1 = install(work, "stage1-fpc", args.runtime)
            log("building stage 1 with Free Pascal")
            run([fpc, "-v0", "-Mobjfpc", "-Sh-", "-Cr", "-Co", "-FU" + units,
                 "-Fu" + SELFHOST, "-o" + s1, SOURCE])
            s2 = install(work, "stage2-fpc", args.runtime)
            build_stage(s1, s2, args.opt)
            ir1 = os.path.join(work, "stage1-fpc.ll")
            ir2 = os.path.join(work, "stage2-fpc.ll")
            emit_ir(s1, ir1)
            emit_ir(s2, ir2)
            if filecmp.cmp(ir1, irs[2], shallow=False) and \
               filecmp.cmp(ir2, irs[2], shallow=False):
                log("the Free Pascal path gives the same IR")
            else:
                failures.append("the Free Pascal path gives different IR")

    for f in failures:
        log("FAIL: " + f)
    if not failures:
        log("PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
