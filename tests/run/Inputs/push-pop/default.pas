program dflt;
var
  b: byte;
  i: integer;
begin
  i := 300;
  {$push}{$R-}{$pop}
  b := i;
  writeln(b);
end.
