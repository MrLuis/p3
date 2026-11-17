{ The inline directive (--mode=objfpc, where $inline on is the
  default): inline routines, methods, record operators, nested and
  recursive routines, and one under $inline off. Inlining changes no results: the output is
  identical to Free Pascal 3.2.2's (fpc -Mobjfpc) at every level.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/inline.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/inline.expected %t.O2.out
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/inline.expected %t.g.out
}
program inline_;

{$modeswitch advancedrecords}

type
  TPair = record
    A, B: integer;
    class operator +(const x, y: TPair): TPair; inline;
    function Sum: integer; inline;
  end;

  TCounter = class
    N: integer;
    procedure Bump(by: integer); inline;
    function Value: integer; inline;
  end;

class operator TPair.+(const x, y: TPair): TPair;
begin
  Result.A := x.A + y.A;
  Result.B := x.B + y.B;
end;

function TPair.Sum: integer;
begin
  Sum := A + B;
end;

procedure TCounter.Bump(by: integer);
begin
  N := N + by;
end;

function TCounter.Value: integer;
begin
  Value := N;
end;

function Sq(x: integer): integer; inline;
begin
  Sq := x * x;
end;

function Fact(n: integer): int64; inline;
begin
  if n <= 1 then
    Fact := 1
  else
    Fact := n * Fact(n - 1);
end;

function Clamp(x, lo, hi: integer): integer; inline;
begin
  if x < lo then
    exit(lo);
  if x > hi then
    exit(hi);
  Clamp := x;
end;

{$inline off}
function Cube(x: integer): integer; inline;
begin
  Cube := x * x * x;
end;
{$inline on}

procedure Outer(start: integer);
var
  total: integer;

  procedure Add(x: integer); inline;
  begin
    total := total + x;
  end;

begin
  total := start;
  Add(10);
  Add(Sq(3));
  writeln('outer ', total);
end;

var
  p, q: TPair;
  c: TCounter;
  i: integer;

begin
  writeln(Sq(7), ' ', Fact(10), ' ', Cube(3));
  for i := -2 to 12 do
    if i mod 5 = 0 then
      write(Clamp(i, 0, 10), ' ');
  writeln;
  p.A := 1; p.B := 2;
  q.A := 10; q.B := 20;
  writeln((p + q).Sum);
  c := TCounter.Create;
  c.Bump(3);
  c.Bump(4);
  writeln(c.Value);
  c.Free;
  Outer(5);
end.
