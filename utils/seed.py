#!/usr/bin/env python3
"""The stage-1 seed (SELFHOST.md S11): bootstrapping the self-hosted
compiler without the C++ pcc.

A seed is a directory (and a .tar.gz of it) named
pcc-seed-<version>-<triple>, holding

  bin/pcc              a stage-1 compiler for <triple> (linked with libLLVM)
  lib/libpascalrt.a    its runtime
  pcc.ll               the compiler as LLVM IR (selfhost/pcc.pas at -O0): a
                       stage 1 for <triple> can be built from it with clang
                       alone, where the binary does not run
  SEED.txt             version, triple, LLVM version, source commit, and
                       the SHA-256 of each file

  utils/seed.py make --pcc build/bin/pcc --runtime build/lib/libpascalrt.a \\
      [--flags build/selfhost/flags.txt] [--out build/seed]
      Builds a seed: stage 1 with the C++ pcc, the seed's compiler (stage 2)
      with stage 1, and the seed's IR with stage 2.

  utils/seed.py bootstrap --seed <dir or .tar.gz> [--from binary|ir]
      [--work build/seed-bootstrap] [--llvm-config llvm-config-19]
      Bootstraps from a seed: no C++ pcc and no CMake. The runtime is
      compiled from runtime/io.c with cc, the LLVM-specific parts come from
      llvm-config. Stage 1 is the seed's binary (or built from its IR with
      clang); stage 2 and stage 3 are built from selfhost/; stage 2 and
      stage 3 must write the same IR, and if the sources are those of the
      seed, the seed's IR too.

Paths are given relative to the repository root, so the IR (which names
source files in its runtime error messages) does not depend on where the
repository is.
"""
import argparse
import hashlib
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join("selfhost", "pcc.pas")  # relative to ROOT


def log(msg):
    print("seed: " + msg, flush=True)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, **kw)
    if r.returncode != 0:
        sys.stderr.write("seed: command failed (%d): %s\n%s%s" %
                         (r.returncode, " ".join(cmd), r.stdout, r.stderr))
        sys.exit(1)
    return r.stdout


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def compile_self(compiler, flags, out, extra=()):
    run([compiler, "--mode=tp"] + list(extra) + flags + ["-I", "selfhost", SOURCE, "-o", out])


def emit_ir(compiler, flags, out):
    compile_self(compiler, flags, out, ["--emit-llvm"])


def install(stage_dir, runtime):
    os.makedirs(os.path.join(stage_dir, "bin"), exist_ok=True)
    os.makedirs(os.path.join(stage_dir, "lib"), exist_ok=True)
    shutil.copyfile(runtime, os.path.join(stage_dir, "lib", "libpascalrt.a"))
    return os.path.join(stage_dir, "bin", "pcc")


def version_line(compiler):
    """'0.1.0', '19.1.7', 'aarch64-unknown-linux-gnu' from --version."""
    out = run([compiler, "--version"]).splitlines()
    first = out[0].split()
    version = first[2]
    llvm = first[4].rstrip(")")
    triple = out[1].split(":", 1)[1].strip()
    return version, llvm, triple


def git_commit():
    r = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True,
                       cwd=ROOT)
    dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True, cwd=ROOT).stdout.strip()
    commit = r.stdout.strip() if r.returncode == 0 else "unknown"
    return commit + (" (with local changes)" if dirty else "")


def cmd_make(args):
    flags = shlex.split(open(args.flags).read())
    work = tempfile.mkdtemp(prefix="pcc-seed")
    try:
        s1 = install(os.path.join(work, "stage1"), args.runtime)
        log("building stage 1 with the C++ pcc")
        compile_self(os.path.abspath(args.pcc), flags, s1, ["-O2"])
        s2 = install(os.path.join(work, "stage2"), args.runtime)
        log("building the seed compiler (stage 2) with stage 1")
        compile_self(s1, flags, s2, ["-O2"])
        version, llvm, triple = version_line(s2)
        name = "pcc-seed-%s-%s" % (version, triple)
        out = os.path.join(os.path.abspath(args.out), name)
        if os.path.exists(out):
            shutil.rmtree(out)
        os.makedirs(os.path.join(out, "bin"))
        os.makedirs(os.path.join(out, "lib"))
        shutil.copy2(s2, os.path.join(out, "bin", "pcc"))
        shutil.copyfile(args.runtime, os.path.join(out, "lib", "libpascalrt.a"))
        log("writing the seed IR")
        emit_ir(s2, flags, os.path.join(out, "pcc.ll"))
        files = ["bin/pcc", "lib/libpascalrt.a", "pcc.ll"]
        with open(os.path.join(out, "SEED.txt"), "w") as f:
            f.write("pcc stage-1 seed (SELFHOST.md S11)\n\n")
            f.write("version: %s\ntarget: %s\nLLVM: %s\nsource commit: %s\n\n" %
                    (version, triple, llvm, git_commit()))
            f.write("Bootstrap from it, in a pcc source tree:\n"
                    "  utils/seed.py bootstrap --seed <this directory or .tar.gz>\n\n")
            f.write("SHA-256:\n")
            for rel in files:
                f.write("  %s  %s\n" % (sha256(os.path.join(out, rel)), rel))
        tar = out + ".tar.gz"
        with tarfile.open(tar, "w:gz") as t:
            t.add(out, arcname=name)
        with open(tar + ".sha256", "w") as f:
            f.write("%s  %s\n" % (sha256(tar), os.path.basename(tar)))
        log("wrote %s and %s" % (out, tar))
    finally:
        shutil.rmtree(work, ignore_errors=True)


def llvm_settings(llvm_config, include_out):
    """The flags that build selfhost/pcc.pas, without CMake: the target
    initializers and the libLLVM link options, from llvm-config."""
    includedir = run([llvm_config, "--includedir"]).strip()
    libdir = run([llvm_config, "--libdir"]).strip()
    libs = run([llvm_config, "--link-shared", "--libs"]).split()
    os.makedirs(include_out, exist_ok=True)
    run([sys.executable, os.path.join(ROOT, "utils", "gen-llvm-targets.py"), includedir,
         os.path.join(include_out, "llvmtargets.inc")])
    return ["-I", include_out, "-L" + libdir] + libs, libdir, libs


def build_runtime(work):
    """libpascalrt.a from runtime/io.c with the C compiler."""
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("clang") or "gcc"
    obj = os.path.join(work, "io.o")
    lib = os.path.join(work, "libpascalrt.a")
    run([cc, "-std=c11", "-O2", "-fPIC", "-I", os.path.join("runtime", "include"),
         "-c", os.path.join("runtime", "io.c"), "-o", obj])
    if os.path.exists(lib):
        os.remove(lib)
    run([shutil.which("ar") or "ar", "rcs", lib, obj])
    return lib


def find_seed(path, work):
    """The seed directory (a .tar.gz is unpacked into work)."""
    if os.path.isdir(path):
        return os.path.abspath(path)
    with tarfile.open(path) as t:
        names = t.getnames()
        top = names[0].split("/")[0]
        for m in t.getmembers():
            if m.name.startswith("/") or ".." in m.name.split("/"):
                sys.exit("seed: unsafe path in %s: %s" % (path, m.name))
        if hasattr(tarfile, "data_filter"):
            t.extractall(work, filter="data")
        else:
            t.extractall(work)
    return os.path.join(work, top)


def verify_seed(seed):
    """The SHA-256 sums in SEED.txt."""
    ok = True
    with open(os.path.join(seed, "SEED.txt")) as f:
        lines = f.read().split("SHA-256:\n", 1)[1].splitlines()
    for line in lines:
        if not line.strip():
            continue
        digest, rel = line.split()
        if sha256(os.path.join(seed, rel)) != digest:
            log("checksum mismatch: " + rel)
            ok = False
    return ok


def cmd_bootstrap(args):
    work = os.path.abspath(args.work)
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    seed = find_seed(args.seed, work)
    if not verify_seed(seed):
        sys.exit("seed: the seed's files do not match SEED.txt")
    log("seed %s: checksums match" % os.path.basename(seed))

    runtime = build_runtime(work)
    log("built the runtime with the C compiler")
    flags, libdir, libs = llvm_settings(args.llvm_config, os.path.join(work, "selfhost"))

    s1 = install(os.path.join(work, "stage1"), runtime)
    if args.from_ == "binary":
        shutil.copy2(os.path.join(seed, "bin", "pcc"), s1)
        log("stage 1: the seed's binary")
    else:
        clang = args.clang or shutil.which("clang-19") or shutil.which("clang")
        if not clang:
            sys.exit("seed: --from ir needs clang (set --clang)")
        run([clang, "-w", "-O2", os.path.join(seed, "pcc.ll"), runtime,
             "-L" + libdir] + libs + ["-lm", "-o", s1])
        log("stage 1: built from the seed's IR with %s" % clang)

    s2 = install(os.path.join(work, "stage2"), runtime)
    compile_self(s1, flags, s2, ["-O2"])
    s3 = install(os.path.join(work, "stage3"), runtime)
    compile_self(s2, flags, s3, ["-O2"])
    log("built stage 2 and stage 3")
    ir2, ir3 = os.path.join(work, "stage2.ll"), os.path.join(work, "stage3.ll")
    emit_ir(s2, flags, ir2)
    emit_ir(s3, flags, ir3)
    failures = []
    if open(ir2, "rb").read() == open(ir3, "rb").read():
        log("stage 2 and stage 3 write the same IR")
    else:
        failures.append("stage 2 and stage 3 write different IR")
    seed_ir = open(os.path.join(seed, "pcc.ll"), "rb").read()
    if open(ir2, "rb").read() == seed_ir:
        log("stage 2 writes the seed's IR: the sources are the seed's")
    else:
        log("stage 2's IR differs from the seed's: the sources changed since the seed")
        if args.require_same:
            failures.append("the sources are not those of the seed")
    for f in failures:
        log("FAIL: " + f)
    if not failures:
        log("PASS (the compiler is %s)" % s3)
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    mk = sub.add_parser("make", help="build a seed")
    mk.add_argument("--pcc", required=True, help="the C++ pcc")
    mk.add_argument("--runtime", required=True, help="libpascalrt.a")
    mk.add_argument("--flags", default=os.path.join(ROOT, "build", "selfhost", "flags.txt"))
    mk.add_argument("--out", default=os.path.join(ROOT, "build", "seed"))
    bs = sub.add_parser("bootstrap", help="bootstrap from a seed")
    bs.add_argument("--seed", required=True, help="a seed directory or .tar.gz")
    bs.add_argument("--from", dest="from_", choices=["binary", "ir"], default="binary")
    bs.add_argument("--work", default=os.path.join(ROOT, "build", "seed-bootstrap"))
    bs.add_argument("--llvm-config", default=shutil.which("llvm-config-19") or "llvm-config")
    bs.add_argument("--clang", help="for --from ir")
    bs.add_argument("--require-same", action="store_true",
                    help="fail unless the sources are those of the seed")
    args = ap.parse_args()
    if args.cmd == "make":
        cmd_make(args)
        return 0
    return cmd_bootstrap(args)


if __name__ == "__main__":
    sys.exit(main())
