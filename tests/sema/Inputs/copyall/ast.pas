program ast;
type
  TFn = reference to function(x: Integer): Integer;
var
  g: Integer;

function Make(k: Integer; const s: string): TFn;
var
  unused: Integer;
begin
  Make := function[=](x) => x + k + g + Length(s);
end;

begin
end.
