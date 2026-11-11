program bad;
type
  TColor = (Red, Green);
var
  c: TColor;
  i: integer;
begin
  i := 5;
  c := TColor(i);
  writeln(c);
end.
