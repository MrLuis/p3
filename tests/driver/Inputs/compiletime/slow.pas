program slow;
function Loop: integer; compiletime;
var
  i: int64;
begin
  i := 0;
  while i >= 0 do
    i := (i + 1) mod 1000;
  Loop := 0;
end;
const
  X = Loop;
begin
  writeln(X);
end.
