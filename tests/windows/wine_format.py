# -*- Python -*-
"""The run tests (tests/run/*.pas) again, as Windows programs under Wine.

Each program becomes the test windows/<name>.pas (and windows/self/<name>.pas
for the self-hosted compiler, when it is built). Its RUN lines are replayed
with these changes:

  %p3 <options> %s -o X     compiled with --target=<the Windows triple>, to X.exe
  [echo text |] [not] [env V=x] X [args] [< in] [> out] [2>&1]
                            X.exe run under Wine (output with \r\n as \n)
  ... | diff - FILE         compared with FILE (X.windows.expected in place
                            of X.expected, when there is one)
  ... | FileCheck ...       checked with FileCheck
  diff A B, FileCheck ... < F, rm, mkdir, cd, &&   as written
  not %p3 ..., %p3 -c/-S/--emit-*   left out: diagnostics and compile-only
                            output are the same for any target

A program with any other RUN line, or listed in config.wine_unsupported, is
UNSUPPORTED: it tests something else than what runs on Windows.
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
        state = {"cwd": exec_dir, "exes": set()}
        try:
            plan = [step for l in lines for step in self.parse(l, subst)]
            if not any(st[0] == "run" for st in plan):
                raise Unsupported("runs no program")
            for step in plan:
                self.do(step, compiler, env, state, log)
        except Unsupported as e:
            return lit.Test.Result(lit.Test.UNSUPPORTED, str(e))
        except Failure as e:
            log.append(str(e))
            return lit.Test.Result(lit.Test.FAIL, "\n".join(log))
        return lit.Test.Result(lit.Test.PASS, "\n".join(log))

    def parse(self, line, subst):
        """The steps of one RUN line (commands joined by &&)."""
        if line.endswith("\\"):
            raise Unsupported("a continued RUN line")
        for k, v in subst:
            line = line.replace(k, v)
        toks = shlex.split(line)
        cmds, cur = [], []
        for t in toks:
            if t == "&&":
                cmds.append(cur)
                cur = []
            else:
                cur.append(t)
        cmds.append(cur)
        return [self.parse_command(c) for c in cmds]

    def parse_command(self, toks):
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
        # Compiler diagnostics and compile-only output are the same for any
        # target: those lines are left to the other tests.
        if first[0] == "not" and len(first) > 1 and first[1] == "%p3":
            return ("skip",)
        if first[0] == "%p3":
            args = [a for a in first[1:] if a != "2>&1"]
            if len(stages) > 1 or "-o" not in args or any(
                    a in ("-c", "-S") or a.startswith("--emit") for a in args):
                return ("skip",)
            i = args.index("-o")
            out = args[i + 1]
            args = args[:i] + args[i + 2:]
            return ("compile", args, out)
        if len(stages) == 1:
            if first[0] == "rm":
                return ("rm", [a for a in first[1:] if not a.startswith("-")])
            if first[0] == "mkdir":
                return ("mkdir", [a for a in first[1:] if not a.startswith("-")])
            if first[0] == "cd" and len(first) == 2:
                return ("cd", first[1])
            if first[0] == "diff" and len(first) == 3:
                return ("diff", first[1], first[2])
            if first[0] == "FileCheck":
                args, inp = redirects(first[1:])[0:2]
                if inp is None:
                    raise Unsupported("FileCheck without input")
                return ("filecheck", args, inp)
        stdin_text = None
        if first[0] == "echo" and len(stages) > 1:
            words = first[1:]
            nl = "\n"
            if words and words[0] == "-n":
                words, nl = words[1:], ""
            stdin_text = " ".join(words) + nl
            stages = stages[1:]
            first = stages[0]
        expect_fail = False
        if first and first[0] == "not":
            expect_fail = True
            first = first[1:]
        run_env = {}
        if first and first[0] == "env":
            first = first[1:]
            while first and "=" in first[0] and not is_program(first[0]):
                k, v = first.pop(0).split("=", 1)
                run_env[k] = v
        if first and is_program(first[0]):  # a program the test built
            merge = "2>&1" in first
            args, inp, out = redirects([t for t in first[1:] if t != "2>&1"])
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
            if stdin_text is not None and inp is not None:
                raise Unsupported("both echo | and <")
            return ("run", first[0], args, inp, out, checks, run_env,
                    expect_fail, merge, stdin_text)
        raise Unsupported("a RUN line with " + (first[0] if first else "not"))

    def do(self, step, compiler, env, state, log):
        kind = step[0]
        cwd = state["cwd"]
        at = lambda p: os.path.join(cwd, p)  # (absolute paths stay)
        if kind == "skip":
            return
        if kind == "compile":
            _, args, out = step
            cmd = shlex.split(compiler) + args + ["--target=" + self.triple,
                                                  "-o", at(out) + ".exe"]
            log.append("$ " + " ".join(cmd))
            r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True,
                               text=True, errors="replace")
            if r.returncode != 0:
                raise Failure("compile failed:\n" + r.stdout + r.stderr)
            state["exes"].add(os.path.normpath(at(out)))
        elif kind == "rm":
            for f in step[1]:
                f = at(f)
                if os.path.isdir(f):
                    import shutil
                    shutil.rmtree(f)
                elif os.path.exists(f):
                    os.remove(f)
        elif kind == "mkdir":
            for d in step[1]:
                os.makedirs(at(d), exist_ok=True)
        elif kind == "cd":
            state["cwd"] = at(step[1])
        elif kind == "diff":
            a, b = windows_variant(at(step[1])), windows_variant(at(step[2]))
            compare(read(a), read(b), a, b)
        elif kind == "filecheck":
            _, args, inp = step
            self.filecheck_text(args, read(at(inp)), env, cwd)
        elif kind == "run":
            (_, exe, args, inp, out, checks, run_env, expect_fail, merge,
             stdin_text) = step
            exe = os.path.normpath(at(exe))
            if exe not in state["exes"]:
                raise Unsupported("runs something it did not compile")
            cmd = [self.wine, exe + ".exe"] + args
            log.append("$ " + " ".join(cmd))
            io = {"input": stdin_text.encode()} if stdin_text is not None else {
                "stdin": open(at(inp), "rb") if inp else subprocess.DEVNULL}
            try:
                r = subprocess.run(cmd, cwd=cwd, env=dict(env, **run_env),
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT if merge else subprocess.PIPE,
                                   timeout=120, **io)
            except subprocess.TimeoutExpired:
                raise Failure("timed out: " + " ".join(cmd))
            finally:
                if inp:
                    io["stdin"].close()
            text = r.stdout.replace(b"\r\n", b"\n").decode("utf-8", "replace")
            if (r.returncode != 0) != expect_fail:
                raise Failure("exit code %d\n--- stdout\n%s--- stderr\n%s" %
                              (r.returncode, text,
                               (r.stderr or b"").decode("utf-8", "replace")))
            if out is not None:
                with open(at(out), "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
            for c in checks:
                if c[0] == "diff":
                    f = windows_variant(at(c[1]))
                    compare(text, read(f), "(output)", f)
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


def is_program(tok):
    """A program the test runs: a path (absolute, or ./name)."""
    return tok.startswith("/") or tok.startswith("./")


def windows_variant(path):
    """X.windows.expected in place of X.expected, when a test has one: what
    differs on Windows by design (path delimiters, ...)."""
    if path.endswith(".expected"):
        w = path[:-len(".expected")] + ".windows.expected"
        if os.path.exists(w):
            return w
    return path


def read(path):
    with open(path, "rb") as f:
        return f.read().replace(b"\r\n", b"\n").decode("utf-8", "replace")


def compare(a, b, na, nb):
    if a != b:
        import difflib
        d = "".join(difflib.unified_diff(a.splitlines(True), b.splitlines(True),
                                         na, nb))
        raise Failure("differs:\n" + d[:4000])
