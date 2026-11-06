program t;
type Color = (red, green, blue);
var s: 1..10; a: array [1..3] of integer; i: integer; c: Color; r: real;
begin
  i := 11;
  s := i;
  writeln('not reached')
end.
