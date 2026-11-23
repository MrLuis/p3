program bad;
var
  g: integer;
function F: integer; compiletime;
begin
  writeln('side effect');
  F := g;
end;
const
  X = F;
begin
  writeln(X);
end.
