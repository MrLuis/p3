program tp;
type
  TFn = function(x: integer): integer;
var
  f: TFn;
begin
  f := function(x) => x + 1;
end.
