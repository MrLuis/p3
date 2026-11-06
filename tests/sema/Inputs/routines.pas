program r;
type Arr = array [1..3] of integer;
var g: integer; a: Arr;

procedure p(x: integer; var y: integer);
begin
  y := x
end;

function f(n: integer): integer;
begin
  n := n + 1
end;

function bad: Arr;
begin
end;

procedure q(a, a: integer);
begin
end;

function later(x: integer): integer; forward;
procedure missing; forward;

function later(x: char): integer;
begin
  later := 1
end;

procedure loops;
begin
  for g := 1 to 2 do writeln
end;

begin
  p(1);
  p(1, 2);
  p(1, g + 1);
  p('c', g);
  g := p;
  f(1);
  f := 3;
  g := f;
  writeln(f(1, 2))
end.
