#!/usr/bin/env python3
"""Turns the runtime's LLVM IR into libpascalrt.ll, which pcc links into
optimized programs so that the runtime's small functions are inlined
(optimization.md, item 3).

  utils/gen-runtime-ir.py <linked runtime .ll> <output .ll>

The input is what `clang -O2 -S -emit-llvm` writes for runtime/*.c, linked
into one module by llvm-link. The output differs in that:

  - functions and variables with external linkage are available_externally:
    the optimizer can inline or read them, and the program still links the
    library's copies (libpascalrt.a), which every call that is not inlined
    goes to;
  - the target triple and data layout are gone (pcc sets its module's), and
    so are the module flags and the target-cpu/target-features attributes,
    which would keep LLVM from inlining into pcc's functions.

A variable of the runtime that is not a constant must have external
linkage: a static one would get a second copy in the program, apart from
the library's. That is an error here.
"""
import re
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1]) as f:
        lines = f.read().split("\n")
    out = ["; The Pascal runtime as LLVM IR, made by utils/gen-runtime-ir.py."]
    bad = []
    for line in lines:
        if line.startswith(("target triple", "target datalayout", "source_filename",
                            "; ModuleID", "!llvm.module.flags", "!llvm.ident")):
            continue
        if line.startswith("define "):
            m = re.match(r"define (internal |private |dso_local )*", line)
            if "internal " not in m.group(0) and "private " not in m.group(0):
                line = "define available_externally " + line[m.end():]
        elif line.startswith("@"):
            m = re.match(r"(@\S+ = )((?:\w+ )*?)(global|constant) ", line)
            if m:
                words = m.group(2).split()
                local = "internal" in words or "private" in words
                if local and m.group(3) == "global":
                    bad.append(line)
                if not local and "external" not in words:
                    words = [w for w in words if w != "dso_local"]
                    line = (m.group(1) + " ".join(["available_externally"] + words) +
                            " " + m.group(3) + " " + line[m.end():])
        elif line.startswith("attributes #"):
            line = re.sub(r' "(target-cpu|target-features|tune-cpu)"="[^"]*"', "", line)
        out.append(line)
    if bad:
        sys.stderr.write("gen-runtime-ir: the runtime has static variables; give them "
                         "external linkage (see the comment in this script):\n")
        for line in bad:
            sys.stderr.write("  " + line[:100] + "\n")
        sys.exit(1)
    with open(sys.argv[2], "w") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
