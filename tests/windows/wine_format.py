# -*- Python -*-
"""The run tests (tests/run/*.pas) again, as Windows programs under Wine.

Each program becomes the test windows/<name>.pas (and windows/self/<name>.pas
for the self-hosted compiler, when it is built). Its RUN lines are replayed
with these changes:

  %p3 <options> %s -o X     compiled with --target=<the Windows triple>, to X.exe
  X [args] [< in] [> out]   X.exe run under Wine (output with \r\n as \n)
  ... | diff - FILE         compared with FILE
  ... | FileCheck ...       checked with FileCheck
  diff A B, FileCheck ... < F, rm -f ...   as written

A program with any other RUN line (not, env, a cross-unit build, ...) or
listed in config.wine_unsupported is UNSUPPORTED: it tests something else
than what runs on Windows.
"""
import os
import shlex
import subprocess

import lit.formats
import lit.Test


class Unsupported(Exception):
    pass


class WineRunFormat(lit.formats.base.TestFormat):
    def __init__(self, run_dir, compilers, triple, wine, filecheck):
        self.run_dir = run_dir
        self.compilers = compilers  # [(subdir or '', command line)]
        self.triple = triple
        self.wine = wine
        self.filecheck = filecheck

    def getTestsInDirectory(self, testSuite, path_in_suite, litConfig, localConfig):
        if path_in_suite != ("windows",):
            return
        for name in sorted(os.listdir(self.run_dir)):
            if not name.endswith(".pas"):
                continue
            src = os.path.join(self.run_dir, name)
            for sub, _ in self.compilers:
                path = path_in_suite + ((sub, name) if sub else (name,))
                yield lit.Test.Test(testSuite, path, localConfig, file_path=src)

    # ---- running one test ------------------------------------------------

    def execute(self, test, litConfig):
        src = test.getFilePath()
        name = os.path.basename(src)[:-4]
        if name in test.config.wine_unsupported:
            return lit.Test.Result(lit.Test.UNSUPPORTED, "listed in wine_unsupported")
        sub = test.path_in_suite[-2] if len(test.path_in_suite) > 2 else ""
        compiler = dict(self.compilers)[sub]
        exec_dir = os.path.dirname(test.getExecPath())
        out_dir = os.path.join(exec_dir, "Output")
        os.makedirs(out_dir, exist_ok=True)
        tmp = os.path.join(out_dir, name + ".pas.tmp")
        with open(src, encoding="utf-8", errors="replace") as f:
            lines = [l.split("RUN:", 1)[1].strip() for l in f if "RUN:" in l]
        if not lines:
            return lit.Test.Result(lit.Test.UNSUPPORTED, "no RUN lines")
        env = dict(test.config.environment)
        subst = [("%s", src), ("%S", os.path.dirname(src)), ("%t", tmp)]
        log = []
        try:
            plan = [self.parse(l, subst) for l in lines]
            exes = set()
            for kind, *args in plan:
                if kind == "compile":
                    exes.add(args[1])
            for kind, *args in plan:
                if kind == "run" and args[0] not in exes:
                    raise Unsupported("runs something it did not compile")
            for step in plan:
                self.do(step, compiler, env, exec_dir, log)
        except Unsupported as e:
            return lit.Test.Result(lit.Test.UNSUPPORTED, str(e))
        except Failure as e:
            log.append(str(e))
            return lit.Test.Result(lit.Test.FAIL, "\n".join(log))
        return lit.Test.Result(lit.Test.PASS, "\n".join(log))

    def parse(self, line, subst):
        if line.endswith("\\"):
            raise Unsupported("a continued RUN line")
        # the longest substitutions first: %s and %S before %t's users
        for k, v in subst:
            line = line.replace(k, v)
        toks = shlex.split(line)
        stages, cur = [], []
        for t in toks:
            if t == "|":
                stages.append(cur)
                cur = []
            else:
                cur.append(t)
        stages.append(cur)
        first = stages[0]
        if not first:
            raise Unsupported("an empty RUN line")
        if first[0] == "%p3" and len(stages) == 1:
            args = first[1:]
            if "-o" not in args or any(a in ("-c", "-S", "--emit-llvm") or
                                       a.startswith("--emit") for a in args):
                raise Unsupported("a %p3 line that does not build a program")
            i = args.index("-o")
            out = args[i + 1]
            args = args[:i] + args[i + 2:]
            return ("compile", args, out)
        if first[0] == "rm" and len(stages) == 1:
            return ("rm", [a for a in first[1:] if not a.startswith("-")])
        if first[0] == "diff" and len(stages) == 1 and len(first) == 3:
            return ("diff", first[1], first[2])
        if first[0] == "FileCheck" and len(stages) == 1:
            args, inp = redirects(first[1:])[0:2]
            if inp is None:
                raise Unsupported("FileCheck without input")
            return ("filecheck", args, inp)
        if first[0].startswith("/"):  # a program the test built
            args, inp, out = redirects(first[1:])
            checks = []
            for st in stages[1:]:
                if st[:2] == ["diff", "-"] and len(st) == 3:
                    checks.append(("diff", st[2]))
                elif st and st[0] == "FileCheck":
                    checks.append(("filecheck", st[1:]))
                else:
                    raise Unsupported("a pipe into " + " ".join(st[:1]))
            if out is not None and checks:
                raise Unsupported("both > and |")
            return ("run", first[0], args, inp, out, checks)
        raise Unsupported("a RUN line with " + first[0])

    def do(self, step, compiler, env, cwd, log):
        kind = step[0]
        if kind == "compile":
            _, args, out = step
            cmd = shlex.split(compiler) + args + ["--target=" + self.triple,
                                                  "-o", out + ".exe"]
            log.append("$ " + " ".join(cmd))
            r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True,
                               text=True, errors="replace")
            if r.returncode != 0:
                raise Failure("compile failed:\n" + r.stdout + r.stderr)
        elif kind == "rm":
            for f in step[1]:
                if os.path.isfile(f):
                    os.remove(f)
        elif kind == "diff":
            compare(read(step[1]), read(step[2]), step[1], step[2])
        elif kind == "filecheck":
            _, args, inp = step
            self.filecheck_text(args, read(inp), env, cwd)
        elif kind == "run":
            _, exe, args, inp, out, checks = step
            cmd = [self.wine, exe + ".exe"] + args
            log.append("$ " + " ".join(cmd))
            stdin = open(inp, "rb") if inp else subprocess.DEVNULL
            try:
                r = subprocess.run(cmd, cwd=cwd, env=env, stdin=stdin,
                                   capture_output=True, timeout=120)
            except subprocess.TimeoutExpired:
                raise Failure("timed out: " + " ".join(cmd))
            finally:
                if inp:
                    stdin.close()
            text = r.stdout.replace(b"\r\n", b"\n").decode("utf-8", "replace")
            if r.returncode != 0:
                raise Failure("exit code %d\n--- stdout\n%s--- stderr\n%s" %
                              (r.returncode, text,
                               r.stderr.decode("utf-8", "replace")))
            if out is not None:
                with open(out, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
            for c in checks:
                if c[0] == "diff":
                    compare(text, read(c[1]), "(output)", c[1])
                else:
                    self.filecheck_text(c[1], text, env, cwd)

    def filecheck_text(self, args, text, env, cwd):
        r = subprocess.run([self.filecheck] + args, input=text, cwd=cwd,
                           env=env, capture_output=True, text=True,
                           errors="replace")
        if r.returncode != 0:
            raise Failure("FileCheck failed:\n" + r.stderr)


class Failure(Exception):
    pass


def redirects(toks):
    """args, stdin file, stdout file of one command."""
    args, inp, out = [], None, None
    i = 0
    while i < len(toks):
        t = toks[i]
        if t == "<":
            inp = toks[i + 1]
            i += 2
        elif t == ">":
            out = toks[i + 1]
            i += 2
        elif t.startswith("2>") or t in ("&>", ">>"):
            raise Unsupported("redirection " + t)
        else:
            args.append(t)
            i += 1
    return args, inp, out


def read(path):
    with open(path, "rb") as f:
        return f.read().replace(b"\r\n", b"\n").decode("utf-8", "replace")


def compare(a, b, na, nb):
    if a != b:
        import difflib
        d = "".join(difflib.unified_diff(a.splitlines(True), b.splitlines(True),
                                         na, nb))
        raise Failure("differs:\n" + d[:4000])
