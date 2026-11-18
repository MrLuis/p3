program fastmath;
var
  a, b, c: real;
begin
  a := 1.5;
  b := 2.5;
  c := a + b * a - b / a;
  c := -c;
  if c < a then
    writeln('lt');
end.
