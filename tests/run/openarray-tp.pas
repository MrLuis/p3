{ Open arrays in --mode=tp; an index outside 0..high is a runtime error.
  RUN: %pcc --mode=tp %s -o %t
  RUN: not %t 2>&1 | FileCheck %s
  CHECK: 6 2
  CHECK-NEXT: 3.50
  CHECK-NEXT: Runtime error: array index out of bounds at
}
program openarraytp;
uses math;
var
  v: array[1..3] of longint;
function Total(const a: array of longint): longint;
var
  i, t: longint;
begin
  t := 0;
  for i := low(a) to high(a) do
    t := t + a[i];
  Total := t;
end;
procedure Bad(const a: array of longint; i: longint); begin writeln(a[i]); end;
begin
  v[1] := 1; v[2] := 2; v[3] := 3;
  writeln(Total(v), ' ', high(v) - 1);
  writeln(MaxValue([1.5, 3.5]):0:2);
  Bad(v, 3);
end.
