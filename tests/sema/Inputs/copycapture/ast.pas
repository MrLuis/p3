program ast;
type
  TFn = reference to function: Integer;
var
  f: TFn;
  k: Integer;
begin
  f := function[k]: Integer => k;
end.
