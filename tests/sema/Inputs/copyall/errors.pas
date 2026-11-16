program errors;
type
  TFn = reference to function: Integer;
  TProc = reference to procedure;
  TPlain = function: Integer;
var
  f: TFn;
  q: TProc;
  p: TPlain;

procedure Run(k: Integer);
begin
  p := function[=]: Integer => k;
  q := procedure[=] => k := 1;
end;

begin
end.
