unit mathx;
interface
function Twice(x: integer): integer; inline;
implementation
function Twice(x: integer): integer;
begin
  Twice := x * 2;
end;
end.
