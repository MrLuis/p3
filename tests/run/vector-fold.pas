{ Wrapping integer vector accumulation folding with constant & runtime trip counts.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/vector-fold.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/vector-fold.expected
}
program vector_fold;
type
  TVI = vector[4] of longint;

procedure RunAdd(n: longint; initVal, stepVal: TVI; var outVal: TVI);
var
  i: longint;
  acc: TVI;
begin
  acc := initVal;
  for i := 1 to n do
    acc := acc + stepVal;
  outVal := acc;
end;

procedure RunSub(n: longint; initVal, stepVal: TVI; var outVal: TVI);
var
  i: longint;
  acc: TVI;
begin
  acc := initVal;
  for i := 1 to n do
    acc := acc - stepVal;
  outVal := acc;
end;

var
  a, b, r1, r2, r3: TVI;
  i: longint;
begin
  a[1] := 12345; a[2] := -67890; a[3] := 246809; a[4] := -135791;
  b[1] := 3; b[2] := -5; b[3] := 11; b[4] := 17;
  RunAdd(5000000, a, b, r1);
  RunSub(0, a, b, r2);
  RunSub(123456, a, b, r3);
  for i := 1 to 100000 do
    a := a + b * b;
  writeln(r1[1], ' ', r1[2], ' ', r1[3], ' ', r1[4]);
  writeln(r2[1], ' ', r2[2], ' ', r2[3], ' ', r2[4]);
  writeln(r3[1], ' ', r3[2], ' ', r3[3], ' ', r3[4]);
  writeln(a[1], ' ', a[2], ' ', a[3], ' ', a[4]);
end.
