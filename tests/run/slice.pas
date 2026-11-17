{ slice(a, n) passed to open array parameters: value, const and var, static
  and open arrays, managed elements, overloads. Output identical to Free
  Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/slice.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/slice.expected %t.O2.out
}
program slice;
{$H+}
procedure P(const a: array of longint);
var i: longint;
begin write(high(a), ':'); for i := 0 to high(a) do write(' ', a[i]); writeln; end;
procedure V(var a: array of longint);
var i: longint;
begin for i := 0 to high(a) do a[i] := a[i] * 10; end;
procedure C(a: array of longint);
begin if length(a) > 0 then a[0] := 99; P(a); end;
procedure S(const a: array of string);
var i: longint;
begin for i := 0 to high(a) do write('<', a[i], '>'); writeln; end;
procedure SV(a: array of string);
begin a[0] := a[0] + '!'; S(a); end;
procedure Fw(const a: array of longint; n: longint);
begin P(slice(a, n)); P(slice(a, n - 1)); end;
procedure Fv(var a: array of longint);
begin V(slice(a, 1)); end;
function Ov(const a: array of longint): string; overload; begin Ov := 'ints'; end;
function Ov(const a: array of string): string; overload; begin Ov := 'strs'; end;
function Ov(x: longint): string; overload; begin Ov := 'one'; end;
var
  x: array[1..5] of longint = (1, 2, 3, 4, 5);
  y: array[-1..1] of string;
  n: longint = 3;
begin
  P(x); P(slice(x, 2)); P(slice(x, n)); P(slice(x, 0)); P(slice(x, 5)); P(slice(x, n + 1));
  V(slice(x, 2)); P(x);
  C(slice(x, 2)); P(x);
  y[-1] := 'a'; y[0] := 'b'; y[1] := 'c';
  S(slice(y, 2)); SV(slice(y, 2)); S(y);
  Fw(x, 3); Fv(x); P(x);
  writeln(Ov(slice(x, 2)), Ov(slice(y, 1)), Ov(5));
end.
