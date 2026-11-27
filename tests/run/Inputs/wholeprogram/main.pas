program main;
uses counter;
var i: integer;
begin
  for i := 1 to 5 do Bump;
  writeln(Count, ' ', CAdd(2, 3));
end.
