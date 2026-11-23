program fail;
type
  TA = array[1..3] of integer;
function F(n: integer): integer; compiletime;
var
  a: TA;
begin
  a[n] := 1;
  F := a[1];
end;
const
  X = F(5);
begin
  writeln(X);
end.
