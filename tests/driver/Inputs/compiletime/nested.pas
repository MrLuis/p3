program nested;
function Twice(n: integer): integer; compiletime;
begin
  Twice := 2 * n;
end;
procedure P;
const
  Y = Twice(1);
begin
  writeln(Y);
end;
begin
  writeln(Twice(4));
end.
