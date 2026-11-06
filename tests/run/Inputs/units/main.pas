program UnitsDemo;
uses Shapes, MathUtils;
var q: TSquare;
begin
  q.side := 7;
  describe(q);
  writeln('gcd(84, 36) = ', gcd(84, 36));
  writeln('square called ', calls, ' times; 2*pi ~ ', Pi2:4:2)
end.
