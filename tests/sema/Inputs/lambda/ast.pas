program ast;
type
  TFn = function(x: Integer): Integer;
function Apply(f: TFn; v: Integer): Integer;
begin
  Apply := f(v);
end;
procedure Run(k: Integer);
begin
  Apply(function(x) => x + k, 1);
end;
begin
  Run(1);
end.
