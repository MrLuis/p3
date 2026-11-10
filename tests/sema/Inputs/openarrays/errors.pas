program errors;
type
  S = 1..5;
  TF = file of longint;
procedure p(const a: array of longint); begin end;
procedure v(var a: array of longint); begin end;
procedure c(const a: array of longint); cdecl; begin end;
procedure f(const a: array of TF); begin end;
procedure ch(const a: array of char); begin end;
procedure h(const a: array of longint; b: array of longint);
const K = high(a);
var w: array[0..1] of longint; n: longint;
begin
  a[0] := 1;
  b[0] := 1;
  b := w;
  w := b;
  v(a);
  n := a['x'];
  n := a;
end;
var
  ib: array[1..2] of byte;
  sr: array[1..2] of S;
  n: longint;
  str: string;
begin
  p(ib);
  p(sr);
  p(5);
  p(str);
  v([1, 2]);
  p([1..2]);
  p(['x']);
  c([1]);
  ch(str);
end.
