{ External (C) routines, as in Free Pascal: library and symbol names as
  string constants, the library linked with the program, varargs
  (printf), C callbacks (cdecl procedural types: qsort, bsearch) and cdecl
  routines, external or not, as procedural values. C's printf writes to the
  same stdout as writeln. The output is identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc), but for the lines C prints, which FPC does not flush.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/externals.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 > %t.O2.out
  RUN: diff %S/externals.expected %t.O2.out
}
program externals;

const
  libc = 'c';
  libm = 'm';
  sinName = 'sin';
  fmt = 'printf: %d %s %.2f %c %lld'#10;

type
  PLongint = ^longint;
  TArr = array[0..6] of longint;
  TCompare = function(a, b: pointer): longint; cdecl;
  TUnary = function(x: double): double; cdecl;
  TIntFn = function(x: longint): longint; cdecl;

function strlen(s: pchar): int64; cdecl; external libc;
function c_abs(x: longint): longint; cdecl; external libc name 'abs';
function c_sin(x: double): double; cdecl; external libm name sinName;
function cos(x: double): double; cdecl; external libm;
function printf(format: pchar): longint; cdecl; varargs; external libc;
procedure qsort(base: pointer; n, size: int64; cmp: TCompare); cdecl;
  external libc;
function bsearch(key, base: pointer; n, size: int64;
  cmp: TCompare): pointer; cdecl; external libc;

var
  calls: longint;

function Compare(a, b: pointer): longint; cdecl;
begin
  calls := calls + 1;
  Compare := PLongint(a)^ - PLongint(b)^;
end;

function Descending(a, b: pointer): longint; cdecl;
begin
  Descending := PLongint(b)^ - PLongint(a)^;
end;

function Twice(x: longint): longint; cdecl;
begin
  Twice := 2 * x;
end;

function Apply(f: TUnary; x: double): double;
begin
  Apply := f(x);
end;

procedure Show(var a: TArr);
var
  i: longint;
begin
  for i := 0 to 6 do
    write(a[i], ' ');
  writeln;
end;

var
  a: TArr = (42, 7, 19, -3, 100, 0, 7);
  key: longint;
  found: PLongint;
  cmp: TCompare;
  f: TIntFn;
  n: integer;
  c: char;
begin
  writeln(strlen('hello, world'), ' ', c_abs(-5));
  writeln(c_sin(0.5):0:4, ' ', cos(0.5):0:4);

  n := printf('plain'#10);
  writeln('printf returned ', n);
  c := 'z';
  printf(fmt, 42, 'text', 2.5, c, int64(1234567890123));

  calls := 0;
  qsort(@a[0], 7, sizeof(longint), @Compare);
  Show(a);
  writeln('compare called: ', calls > 0);
  key := 19;
  found := bsearch(@key, @a[0], 7, sizeof(longint), @Compare);
  writeln('found 19: ', found <> nil, ' ', found^);
  key := 5;
  writeln('found 5: ', bsearch(@key, @a[0], 7, sizeof(longint), @Compare) <> nil);

  cmp := @Descending; { a cdecl procedural value: callable from Pascal too }
  qsort(@a[0], 7, sizeof(longint), cmp);
  Show(a);
  key := 3;
  writeln(cmp(@key, @a[0]));

  f := @Twice;
  writeln(f(21));
  f := @c_abs; { an external routine as a procedural value }
  writeln(f(-8));
  writeln(Apply(@c_sin, 0.0):0:1, ' ', Apply(@cos, 0.0):0:1);
end.
