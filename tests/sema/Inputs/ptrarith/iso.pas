program iso(output);
type
  PInt = ^integer;
var
  p, q: PInt;
  n: integer;
begin
  new(p);
  q := p + 1;
  n := p[0];
  if p < q then
    flush(output)
end.
