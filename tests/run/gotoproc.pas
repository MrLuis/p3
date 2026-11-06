{ M10: procedural/functional parameters (incl. nested routines and
  forwarding), local and non-local goto.
  RUN: %pcc %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/gotoproc.expected %t.out
  RUN: %pcc -O2 %s -o %t.O2
  RUN: %t.O2 > %t.O2.out
  RUN: diff %S/gotoproc.expected %t.O2.out
}
program GotoProc(output);
label 98, 99;
var i, n: integer;

function apply(function f(x: real): real; v: real): real;
begin
  apply := f(v)
end;

function square(x: real): real;
begin
  square := x * x
end;

function half(x: real): real;
begin
  half := x / 2
end;

procedure each(procedure p(k: integer); lo, hi: integer);
var k: integer;
begin
  for k := lo to hi do p(k)
end;

procedure counter;
var total: integer;
  procedure add(k: integer);
  begin
    total := total + k   { a nested routine passed as a parameter }
  end;
begin
  total := 0;
  each(add, 1, 10);
  writeln('total = ', total)
end;

procedure show(k: integer);
begin
  write(k, ' ')
end;

{ passing a procedural parameter on }
procedure twice(procedure p(k: integer); k: integer);
begin
  each(p, k, k + 1)
end;

procedure search(depth: integer);
begin
  if depth = 3 then goto 98;   { non-local: leaves all activations }
  writeln('depth ', depth);
  search(depth + 1)
end;

procedure local;
label 1, 2;
var k: integer;
begin
  k := 0;
1:
  k := k + 1;
  if k < 3 then goto 1;
  writeln('k = ', k);
  goto 2;
  writeln('skipped');
2:
end;

begin
  writeln(apply(square, 3):4:1, ' ', apply(half, 16):4:1);
  each(show, 1, 5); writeln;
  twice(show, 7); writeln;
  counter;
  local;
  n := 0;
  for i := 1 to 10 do begin
    if i = 4 then goto 99;
    n := n + i
  end;
  writeln('not reached');
99:
  writeln('n = ', n);
  search(1);
  writeln('not reached either');
98:
  writeln('done')
end.
