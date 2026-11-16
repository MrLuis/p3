program syntax;
type
  TFn = function(x: Integer): Integer;
var
  f: TFn;
begin
  f := function(x) x + 1;
  f := function(x => x;
  f := function(x): => x;
  f := procedure(x): Integer => x;
end.
