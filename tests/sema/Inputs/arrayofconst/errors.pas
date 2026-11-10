program errors;
type
  TColor = (red, green);
  TRec = record x: longint; end;
procedure p(const a: array of const); begin end;
procedure c(const a: array of const); cdecl; begin end;
procedure h(const a: array of const);
const K = high(a);
var v: longint;
begin
  v := a['x'].VInteger;
  v := a;
end;
var
  r: TRec;
  n: longint;
begin
  p([1..2]);
  p([red]);
  p([r]);
  p(n);
  p(1);
  c([1]);
  n := n + high(r);
end.
