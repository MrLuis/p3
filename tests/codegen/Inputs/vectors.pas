program vectors;
{$H+}
type
  TV = vector[4] of double;
  TVI = vector[4] of longint;
var
  v, w: TV;
  a, b: TVI;
  i: longint;
begin
  for i := 1 to 4 do
  begin
    v[i] := i * 1.5;
    w[i] := 2.0;
    a[i] := i;
    b[i] := i * 2;
  end;
  writeln((v[1] * w[1]):0:2, ' ', (v[1] + w[1]):0:2);
  writeln(a[2] * b[2], ' ', a[2] + b[2]);
  b := a + a;
  writeln(b[1], ' ', b[4]);
  v[i] := 3.0;  { an in-bounds i }
end.
