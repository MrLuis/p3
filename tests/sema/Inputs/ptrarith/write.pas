program wr(output);
type
  PCh = ^char;
  PInt = ^integer;
var
  c: PCh;
  p: PInt;
begin
  new(c);
  new(p);
  c^ := 'x';
  writeln(c);
  writeln(p)
end.
