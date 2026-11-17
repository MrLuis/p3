#!/usr/bin/env python3
"""Generates random valid Pascal programs for differential testing of the
two compilers' code generation (SELFHOST.md S10): the C++ p3 and the
self-hosted one must write identical --emit-llvm output for each.

  utils/gen-pascal.py [--mode iso|tp] [--seed N] [--count N] --out DIR

Programs use typed expressions over integers of several sizes, reals,
booleans, chars, enumerations, subranges, sets, arrays, records, pointers,
short strings (Turbo mode), nested routines with value/var parameters and
functions, every statement form (if, case, while, repeat, for, with, goto
in ISO mode), and write the values they compute. Loops are bounded, so the
programs terminate; runtime errors (range, overflow) may happen and are
part of the behavior.
"""
import argparse
import os
import random


class Gen:
    def __init__(self, rng, mode):
        self.r = rng
        self.tp = mode == "tp"
        self.lines = []
        self.indent = 0
        self.counter = 0
        # name -> type (simple type names: integer, real, boolean, char,
        # color, small, int64/byte/word/... (tp), str (tp), arr, rec, set, ptr)
        self.scopes = [{}]
        self.loop_vars = []
        self.params = set()  # not allowed as for control variables
        self.depth = 0

    # --- output ---
    def emit(self, s):
        self.lines.append("  " * self.indent + s)

    def fresh(self, prefix):
        self.counter += 1
        return "%s%d" % (prefix, self.counter)

    def vars_of(self, types):
        out = []
        for scope in self.scopes:
            for n, t in scope.items():
                if t in types and n not in self.loop_vars:
                    out.append(n)
        return out

    # --- types ---
    def int_types(self):
        if self.tp:
            return ["integer", "int64", "byte", "word", "shortint", "smallint", "cardinal"]
        return ["integer"]

    # --- expressions ---
    def int_lit(self):
        return str(self.r.choice([0, 1, 2, 3, 7, 10, 42, 100, 255, 1000, 32767]))

    def int_expr(self, d=0):
        r = self.r
        if d > 3 or r.random() < 0.3:
            vs = self.vars_of(self.int_types() + ["small"])
            if vs and r.random() < 0.6:
                return r.choice(vs)
            return self.int_lit()
        k = r.randrange(12)
        a = self.int_expr(d + 1)
        b = self.int_expr(d + 1)
        if k == 0:
            return "(%s + %s)" % (a, b)
        if k == 1:
            return "(%s - %s)" % (a, b)
        if k == 2:
            return "(%s * %s)" % (a, b)
        if k == 3:
            return "(%s div %s)" % (a, r.choice(["3", "7", "(abs(%s) + 1)" % b]))
        if k == 4:
            return "(%s mod %s)" % (a, r.choice(["5", "16", "(abs(%s) + 1)" % b]))
        if k == 5:
            return "abs(%s)" % a
        if k == 6:
            return "sqr(%s mod 100)" % a
        if k == 7:
            return "ord(%s)" % self.char_expr(d + 1)
        if k == 8:
            return "trunc(%s)" % self.real_expr(d + 1)
        if k == 9:
            return "(-%s)" % a
        if k == 10 and self.tp:
            return "(%s %s %s)" % (a, r.choice(["and", "or", "xor", "shl", "shr"]),
                                   r.choice(["1", "3", b]))
        if k == 11:
            vs = self.vars_of(["arr"])
            if vs:
                return "%s[%d]" % (r.choice(vs), r.randint(1, 5))
            vs = self.vars_of(["rec"])
            if vs:
                return "%s.count" % r.choice(vs)
        return "ord(%s)" % self.bool_expr(d + 1)

    def real_expr(self, d=0):
        r = self.r
        if d > 3 or r.random() < 0.3:
            vs = self.vars_of(["real"])
            if vs and r.random() < 0.6:
                return r.choice(vs)
            return r.choice(["0.5", "1.0", "2.5", "3.14159", "0.1", "100.0", "1e3",
                             "123456.0", "1234567.0"])
        k = r.randrange(8)
        a = self.real_expr(d + 1)
        b = self.real_expr(d + 1)
        if k == 0:
            return "(%s + %s)" % (a, b)
        if k == 1:
            return "(%s - %s)" % (a, b)
        if k == 2:
            return "(%s * %s)" % (a, b)
        if k == 3:
            return "(%s / (abs(%s) + 1.0))" % (a, b)
        if k == 4:
            return "sqrt(abs(%s))" % a
        if k == 5:
            return r.choice(["sin", "cos", "arctan"]) + "(%s)" % a
        if k == 6:
            return "(%s + %s)" % (self.int_expr(d + 1), a)
        return "abs(%s)" % a

    def bool_expr(self, d=0):
        r = self.r
        if d > 3 or r.random() < 0.25:
            vs = self.vars_of(["boolean"])
            if vs and r.random() < 0.5:
                return r.choice(vs)
            return r.choice(["true", "false"])
        k = r.randrange(9)
        if k == 0:
            return "(%s %s %s)" % (self.int_expr(d + 1), r.choice(["<", "<=", "=", "<>", ">", ">="]),
                                   self.int_expr(d + 1))
        if k == 1:
            return "(%s and %s)" % (self.bool_expr(d + 1), self.bool_expr(d + 1))
        if k == 2:
            return "(%s or %s)" % (self.bool_expr(d + 1), self.bool_expr(d + 1))
        if k == 3:
            return "not %s" % self.bool_expr(d + 1)
        if k == 4:
            return "odd(%s)" % self.int_expr(d + 1)
        if k == 5:
            return "(%s < %s)" % (self.real_expr(d + 1), self.real_expr(d + 1))
        if k == 6:
            vs = self.vars_of(["set"])
            if vs:
                return "(%d in %s)" % (r.randint(0, 40), r.choice(vs))
        if k == 7:
            return "(%s = %s)" % (self.char_expr(d + 1), self.char_expr(d + 1))
        if k == 8:
            vs = self.vars_of(["color"])
            if vs:
                return "(%s > %s)" % (r.choice(vs), r.choice(["red", "green", "blue"]))
        return "true"

    def char_expr(self, d=0):
        r = self.r
        vs = self.vars_of(["char"])
        k = r.randrange(4)
        if k == 0 and vs:
            return r.choice(vs)
        if k == 1:
            return "chr(%d + %s mod 26)" % (65, "abs(%s)" % self.int_expr(d + 1))
        if k == 2 and self.tp:
            return "upcase(%s)" % r.choice(["'a'", "'z'", "'Q'"])
        return r.choice(["'a'", "'b'", "'Z'", "' '", "'0'"])

    def set_expr(self, d=0):
        r = self.r
        vs = self.vars_of(["set"])
        k = r.randrange(5)
        if k == 0 and vs:
            return r.choice(vs)
        if k == 1:
            return "[%d..%d]" % (r.randint(0, 10), r.randint(5, 40))
        if k == 2 and vs:
            return "(%s %s %s)" % (r.choice(vs), r.choice(["+", "*", "-"]), self.set_expr(d + 1))
        return "[%s]" % ", ".join(str(r.randint(0, 40)) for _ in range(r.randint(0, 4)))

    def str_expr(self, d=0):
        r = self.r
        vs = self.vars_of(["str"])
        k = r.randrange(5)
        if k == 0 and vs:
            return r.choice(vs)
        if k == 1 and vs and d < 2:
            return "(%s + %s)" % (r.choice(vs), self.str_expr(d + 1))
        if k == 2 and vs:
            return "copy(%s, 1, %d)" % (r.choice(vs), r.randint(0, 5))
        return r.choice(["'hello'", "'x'", "''", "'Pascal!'"])

    # --- statements ---
    def assign(self):
        r = self.r
        choices = []
        for n in self.vars_of(self.int_types()):
            choices.append((n, "int"))
        for n in self.vars_of(["real"]):
            choices.append((n, "real"))
        for n in self.vars_of(["boolean"]):
            choices.append((n, "bool"))
        for n in self.vars_of(["char"]):
            choices.append((n, "char"))
        for n in self.vars_of(["set"]):
            choices.append((n, "set"))
        for n in self.vars_of(["str"]):
            choices.append((n, "str"))
        for n in self.vars_of(["arr"]):
            choices.append(("%s[%d]" % (n, r.randint(1, 5)), "int"))
        for n in self.vars_of(["rec"]):
            choices.append(("%s.count" % n, "int"))
            choices.append(("%s.weight" % n, "real"))
        for n in self.vars_of(["small"]):
            choices.append((n, "small"))
        for n in self.vars_of(["color"]):
            choices.append((n, "color"))
        if not choices:
            return
        target, kind = r.choice(choices)
        if kind == "int":
            v = self.int_expr()
            if self.tp:
                v = "(%s) mod 100" % v  # keep narrow types mostly in range
            self.emit("%s := %s;" % (target, v))
        elif kind == "real":
            self.emit("%s := %s;" % (target, self.real_expr()))
        elif kind == "bool":
            self.emit("%s := %s;" % (target, self.bool_expr()))
        elif kind == "char":
            self.emit("%s := %s;" % (target, self.char_expr()))
        elif kind == "set":
            self.emit("%s := %s;" % (target, self.set_expr()))
        elif kind == "str":
            self.emit("%s := %s;" % (target, self.str_expr()))
        elif kind == "small":
            self.emit("%s := %d + abs(%s) mod 10;" % (target, 1, self.int_expr()))
        else:
            self.emit("%s := %s;" % (target, r.choice(["red", "green", "blue", "succ(red)"])))

    def write_stmt(self):
        r = self.r
        items = []
        for _ in range(r.randint(1, 3)):
            k = r.randrange(6)
            if k == 0:
                items.append(self.int_expr())
            elif k == 1:
                items.append("%s:0:3" % self.real_expr())
            elif k == 2:
                items.append(self.bool_expr())
            elif k == 3:
                items.append(self.char_expr())
            elif k == 4 and self.tp and self.vars_of(["str"]):
                items.append(r.choice(self.vars_of(["str"])))
            else:
                items.append("'%s'" % r.choice(["a", "value", " = ", "ok"]))
        self.emit("writeln(%s);" % ", ".join(items))

    def statement(self):
        r = self.r
        self.depth += 1
        k = r.randrange(14) if self.depth < 4 else r.randrange(3)
        if k in (0, 1):
            self.assign()
        elif k == 2:
            self.write_stmt()
        elif k == 3:
            self.emit("if %s then" % self.bool_expr())
            self.block_stmt()
            if r.random() < 0.5:
                self.emit("else")
                self.block_stmt()
            self.lines[-1] += ""
            self.emit(";")
        elif k == 4:
            # ISO: the control variable is a local of the current routine
            vs = [v for v, t in self.scopes[-1].items()
                  if t == "integer" and v not in self.loop_vars and v not in self.params]
            if vs:
                v = r.choice(vs)
                self.loop_vars.append(v)
                lo, hi = r.randint(-2, 3), r.randint(0, 6)
                if r.random() < 0.5:
                    self.emit("for %s := %d to %d do" % (v, lo, hi))
                else:
                    self.emit("for %s := %d downto %d do" % (v, hi, lo))
                self.block_stmt()
                self.emit(";")
                self.loop_vars.remove(v)
        elif k == 5:
            c = self.fresh_counter()
            self.emit("%s := 0;" % c)
            self.emit("while (%s < %d) and %s do" % (c, r.randint(1, 5), self.bool_expr()))
            self.emit("begin")
            self.indent += 1
            self.emit("%s := %s + 1;" % (c, c))
            self.statement()
            self.indent -= 1
            self.emit("end;")
        elif k == 6:
            c = self.fresh_counter()
            self.emit("%s := 0;" % c)
            self.emit("repeat")
            self.indent += 1
            self.emit("%s := %s + 1;" % (c, c))
            self.statement()
            self.indent -= 1
            self.emit("until (%s >= %d) or %s;" % (c, r.randint(1, 4), self.bool_expr()))
        elif k == 7:
            self.emit("case abs(%s) mod 6 of" % self.int_expr())
            self.indent += 1
            labels = list(range(6))
            r.shuffle(labels)
            used = 0
            while used < len(labels):
                n = r.randint(1, 2)
                ls = labels[used:used + n]
                used += n
                self.emit("%s:" % ", ".join(str(x) for x in ls))
                self.indent += 1
                self.block_stmt()
                self.emit(";")
                self.indent -= 1
            self.indent -= 1
            self.emit("end;")
        elif k == 8:
            vs = self.vars_of(["rec"])
            if vs:
                self.emit("with %s do" % r.choice(vs))
                self.emit("begin")
                self.indent += 1
                self.emit("count := count + %s;" % self.int_lit())
                self.emit("weight := weight * 0.5;")
                self.indent -= 1
                self.emit("end;")
        elif k == 9 and self.routines:
            self.call_stmt()
        elif k == 10:
            vs = self.vars_of(["ptr"])
            if vs:
                p = r.choice(vs)
                self.emit("new(%s);" % p)
                self.emit("%s^ := %s;" % (p, self.int_expr()))
                self.emit("writeln(%s^);" % p)
                self.emit("dispose(%s);" % p)
        elif k == 11 and self.tp:
            vs = self.vars_of(["str"])
            if vs:
                s = r.choice(vs)
                self.emit("writeln(length(%s), ' ', pos('l', %s));" % (s, s))
        elif k == 12:
            vs = self.vars_of(["integer"])
            if vs and self.tp:
                self.emit("inc(%s, %d);" % (r.choice(vs), r.randint(1, 5)))
        else:
            self.assign()
        self.depth -= 1

    def fresh_counter(self):
        # one counter per nesting depth: inner loops never reset outer ones
        return self.counters[-1][min(self.depth, 7)]

    def block_stmt(self):
        self.emit("begin")
        self.indent += 1
        for _ in range(self.r.randint(1, 3)):
            self.statement()
        self.indent -= 1
        self.emit("end")

    def call_stmt(self):
        r = self.r
        name, params, is_func = r.choice(self.routines)
        args = []
        for kind, byref in params:
            if byref:
                vs = self.vars_of(["integer"])
                if not vs:
                    return
                args.append(r.choice(vs))
            elif kind == "integer":
                args.append(self.int_expr())
            else:
                args.append(self.real_expr())
        call = name + ("(%s)" % ", ".join(args) if args else "")
        if is_func:
            self.emit("writeln(%s);" % call)
        else:
            self.emit("%s;" % call)

    # --- declarations ---
    def var_decls(self, scope, n):
        r = self.r
        decls = []
        for _ in range(n):
            t = r.choice(self.int_types() * 2 + ["real", "boolean", "char", "set", "arr",
                                                  "rec", "small", "color", "ptr"] +
                         (["str"] if self.tp else []))
            name = self.fresh("v")
            scope[name] = t
            pascal = {"set": "set of 0..63", "arr": "array[1..5] of integer",
                      "rec": "rec", "small": "1..10", "color": "color",
                      "ptr": "^integer", "str": "string[20]"}.get(t, t)
            decls.append("%s: %s;" % (name, pascal))
        return decls

    def routine(self, depth):
        r = self.r
        is_func = r.random() < 0.5
        name = self.fresh("f" if is_func else "p")
        params = []
        ptext = []
        local = {}
        for _ in range(r.randint(0, 3)):
            pn = self.fresh("a")
            kind = r.choice(["integer", "real"])
            byref = kind == "integer" and r.random() < 0.4
            params.append((kind, byref))
            local[pn] = kind
            self.params.add(pn)
            ptext.append(("var " if byref else "") + "%s: %s" % (pn, kind))
        if is_func and not params:
            # parameterless functions are fine in Pascal, but keep calls simple
            pn = self.fresh("a")
            params.append(("integer", False))
            local[pn] = "integer"
            self.params.add(pn)
            ptext.append("%s: integer" % pn)
        head = ("function " if is_func else "procedure ") + name
        if ptext:
            head += "(" + "; ".join(ptext) + ")"
        if is_func:
            head += ": integer"
        self.emit(head + ";")
        self.scopes.append(local)
        decls = self.var_decls(local, r.randint(1, 3))
        counter = [self.fresh("c") for _ in range(8)]
        for c in counter:
            local[c] = "counter"
        decls.append("%s: integer;" % ", ".join(counter))
        self.emit("var")
        self.indent += 1
        for d in decls:
            self.emit(d)
        self.indent -= 1
        saved = self.routines
        self.routines = list(saved)
        if depth < 2 and r.random() < 0.4:
            self.routine(depth + 1)
        self.counters.append(counter)
        self.emit("begin")
        self.indent += 1
        for n, t in local.items():
            if t == "ptr":
                self.emit("%s := nil;" % n)
        for _ in range(r.randint(1, 4)):
            self.statement()
        if is_func:
            self.emit("%s := %s;" % (name, self.int_expr()))
        self.indent -= 1
        self.emit("end;")
        self.counters.pop()
        self.routines = saved
        self.scopes.pop()
        self.routines.append((name, params, is_func))

    def program(self):
        r = self.r
        self.routines = []
        self.counters = []
        self.emit("program gen(output);")
        self.emit("type")
        self.emit("  color = (red, green, blue, yellow);")
        self.emit("  rec = record count: integer; weight: real; tag: char end;")
        g = self.scopes[0]
        decls = self.var_decls(g, r.randint(3, 8))
        counter = [self.fresh("c") for _ in range(8)]
        for c in counter:
            g[c] = "counter"
        decls.append("%s: integer;" % ", ".join(counter))
        self.emit("var")
        self.indent += 1
        for d in decls:
            self.emit(d)
        self.indent -= 1
        for _ in range(r.randint(0, 3)):
            self.routine(0)
        self.counters.append(counter)
        self.emit("begin")
        self.indent += 1
        for n, t in g.items():
            if t == "ptr":
                self.emit("%s := nil;" % n)
        for _ in range(r.randint(3, 10)):
            self.statement()
        self.indent -= 1
        self.emit("end.")
        return "\n".join(self.lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--mode", default="iso", choices=["iso", "tp"])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for i in range(args.count):
        seed = args.seed * 1000003 + i
        text = Gen(random.Random(seed), args.mode).program()
        with open(os.path.join(args.out, "gen%d-%s.pas" % (seed, args.mode)), "w") as f:
            f.write(text)


if __name__ == "__main__":
    main()
