{ M6: procedures, functions, var parameters, recursion, nested routines
  with up-level access (static links), forward declarations.
  RUN: %pcc %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/routines.expected %t.out
  RUN: %pcc -O2 %s -o %t.O2
  RUN: %t.O2 > %t.O2.out
  RUN: diff %S/routines.expected %t.O2.out
}
program Routines(output);
var g, a, b: integer;

function fact(n: integer): integer;
begin
  if n <= 1 then fact := 1 else fact := n * fact(n - 1)
end;

function fib(n: integer): integer;
begin
  if n < 2 then fib := n else fib := fib(n - 1) + fib(n - 2)
end;

procedure swap(var x, y: integer);
var t: integer;
begin
  t := x; x := y; y := t
end;

function ack(m, n: integer): integer;
begin
  if m = 0 then ack := n + 1
  else if n = 0 then ack := ack(m - 1, 1)
  else ack := ack(m - 1, ack(m, n - 1))
end;

procedure outer;
var count: integer;
  procedure inc;
  begin
    count := count + 1
  end;
  procedure twice;
    procedure deeper;
    begin
      inc; count := count + 10
    end;
  begin
    inc; deeper
  end;
begin
  count := 0;
  inc; twice; twice;
  writeln('count = ', count)
end;

{ a nested function inside a recursive function must see its own activation }
function depthSum(n: integer): integer;
  function here: integer;
  begin
    here := n * 100
  end;
begin
  if n = 0 then depthSum := 0
  else depthSum := here + depthSum(n - 1)
end;

{ the result of a function assigned from a nested procedure }
function viaNested(x: integer): integer;
  procedure setIt;
  begin
    viaNested := x * 2
  end;
begin
  setIt
end;

function isOdd(n: integer): boolean; forward;
function isEven(n: integer): boolean;
begin
  if n = 0 then isEven := true else isEven := isOdd(n - 1)
end;
function isOdd;
begin
  if n = 0 then isOdd := false else isOdd := isEven(n - 1)
end;

function fortyTwo: integer;
begin
  fortyTwo := 42
end;

procedure modifyGlobal;
begin
  g := g + 1
end;

begin
  writeln('fact(10) = ', fact(10));
  writeln('fib(20) = ', fib(20));
  a := 1; b := 2; swap(a, b);
  writeln('a = ', a, ', b = ', b);
  writeln('ack(2, 3) = ', ack(2, 3));
  outer;
  writeln('depthSum(3) = ', depthSum(3));
  writeln('viaNested(21) = ', viaNested(21));
  writeln('isEven(10) = ', isEven(10), ', isOdd(7) = ', isOdd(7));
  writeln('fortyTwo + 1 = ', fortyTwo + 1);
  g := 5; modifyGlobal; modifyGlobal;
  writeln('g = ', g)
end.
