{ An open-array argument built by a call: Copy of a dynamic array (the
  shape Mean(Copy(d, 0, 500)) exposed), with the count taken from the
  result's length; the set constructor and the plain open-array
  arguments beside it. Output identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-oaresult.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-oaresult.expected
}
program rtloaresult;
{$H+}
uses
  SysUtils, Math;

var
  total: longint;

procedure Show(const a: array of integer);
var
  k: longint;
begin
  total := total + 1;
  write('[', Length(a), ':');
  for k := 0 to High(a) do
    write(a[k], ',');
  writeln(']');
end;

function Fold(const a: array of double): double;
begin
  Fold := Sum(a);
end;

var
  d: array of double;
  i: array of integer;
  k: longint;
begin
  total := 0;
  setlength(i, 12);
  for k := 0 to 11 do
    i[k] := k * 3;
  setlength(d, 10);
  for k := 0 to 9 do
    d[k] := k + 0.5;

  { the call-result shapes }
  Show(copy(i, 0, 12));         { the whole array }
  Show(copy(i, 0, 5));          { a prefix }
  Show(copy(i, 3, 4));          { a middle }
  Show(copy(i, 9, 100));        { clamped at the end }
  Show(copy(i, 12, 5));         { at the end, empty }
  Show(copy(i, 0, 0));          { count 0 }

  { the shapes that found it: the math unit over copies }
  writeln('sum   ', Fold(copy(d, 0, 10)):0:1);
  writeln('mean  ', Mean(copy(d, 0, 10)):0:2);
  writeln('count ', Length(copy(i, 2, 7)));

  { a total, and the constructor and empty forms beside it (their own
    tests are in rtl-openarrays and the slice tests) }
  Show([1, 2, 3]);
  Show([]);
  writeln(total);
end.
