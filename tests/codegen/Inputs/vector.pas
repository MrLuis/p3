program vtest;
{$H+}
type
  TV = vector[4] of double;
var
  v: TV;
  i: longint;
begin
  for i := 1 to 4 do
    v[i] := i * 1.5;
  writeln(v[1]:0:2, ' ', v[4]:0:2);
end.
