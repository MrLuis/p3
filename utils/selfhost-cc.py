#!/usr/bin/env python3
"""A pcc-compatible front for the self-hosted compiler until it has its own
driver (SELFHOST.md S8).

The self-hosted compiler writes LLVM IR (--emit-llvm). For -S, -c and
executables this script runs it with --emit-llvm, checks the IR with
`opt -passes=verify`, and lets clang optimize, assemble and link it with
the Pascal runtime, as the C++ pcc does. Dumps (--emit-tokens, --emit-ast,
...) and --emit-llvm go straight to the self-hosted compiler.

Usage (e.g. as %pcc for lit: PCC_UNDER_TEST=utils/selfhost-cc.py):
  SELFHOST_PCC=<self-hosted pcc> selfhost-cc.py [pcc options] file.pas

Environment:
  SELFHOST_PCC     the self-hosted compiler (required)
  LLVM_BIN         directory of clang and opt (default /usr/lib/llvm-19/bin)
  PCC_RUNTIME_LIB  libpascalrt.a (default: build/lib next to this script)
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DUMPS = {"--emit-tokens", "-emit-tokens", "--emit-ast", "-emit-ast",
         "--emit-parse-tree", "-emit-parse-tree", "--emit-llvm", "-emit-llvm"}


def tool(name):
    return os.path.join(os.environ.get("LLVM_BIN", "/usr/lib/llvm-19/bin"), name)


def stem(path):
    base = os.path.basename(path)
    s = os.path.splitext(base)[0]
    return s if s and path != "-" else "a"


def main():
    pcc = os.environ.get("SELFHOST_PCC")
    if not pcc:
        sys.exit("selfhost-cc.py: set SELFHOST_PCC to the self-hosted compiler")
    args = sys.argv[1:]
    if any(a in DUMPS for a in args):
        sys.exit(subprocess.call([pcc] + args))

    # Split off what clang handles: -o, -O<n>, -c, -S, --target.
    passed, output, opt, kind, inputs = [], None, "0", "exe", []
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("-o", "--o"):
            output = args[i + 1] if i + 1 < len(args) else None
            i += 2
            continue
        if a.startswith("-o="):
            output = a[3:]
        elif len(a) == 3 and a.startswith("-O") and a[2].isdigit():
            opt = a[2]
            passed.append(a)
        elif a in ("-c", "-S"):
            kind = a
        else:
            passed.append(a)
            if not a.startswith("-") or a == "-":
                inputs.append(a)
        i += 1
    src = inputs[0] if inputs else "-"
    if output is None:
        output = {"-c": stem(src) + ".o", "-S": stem(src) + ".s"}.get(
            kind, "a.out" if src == "-" else stem(src))

    with tempfile.TemporaryDirectory(prefix="selfhost-cc") as tmp:
        ll = os.path.join(tmp, "out.ll")
        rc = subprocess.call([pcc, "--emit-llvm"] + passed + ["-o", ll])
        if rc != 0:
            return rc
        verify = subprocess.run([tool("opt"), "-passes=verify", "-disable-output", ll],
                                capture_output=True, text=True)
        if verify.returncode != 0:
            sys.stderr.write("selfhost-cc.py: invalid IR:\n" + verify.stderr)
            return 70
        cmd = [tool("clang"), "-w", "-O" + opt]
        if kind != "exe":
            cmd += [kind, ll, "-o", output]
        else:
            rt = os.environ.get("PCC_RUNTIME_LIB",
                                os.path.join(HERE, "..", "build", "lib", "libpascalrt.a"))
            cmd += [ll, rt, "-lm", "-o", output]
        return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
