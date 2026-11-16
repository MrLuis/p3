program subrange;
type
  TSmall = 1..10;
var
  s: TSmall;
  x: Integer;
begin
  x := 300;
  unchecked
    s := x;   { a subrange: still range-checked }
  end;
end.
