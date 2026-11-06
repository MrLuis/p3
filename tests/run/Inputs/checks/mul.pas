program t;
type Color = (red, green, blue);
var s: 1..10; a: array [1..3] of integer; i: integer; c: Color; r: real;
begin
  i := 2;
  i := i * maxint;
  writeln('not reached')
end.
