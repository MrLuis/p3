program nolink;
type
  TFn = function(x: integer): integer;
function Twice(x: integer): integer;
begin
  Twice := x * 2;
end;
var
  f: TFn;
begin
  writeln(Twice(21));
  f := @Twice;
  writeln(f(20));
end.
