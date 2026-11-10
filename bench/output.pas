{ Text output: many writeln calls with integers, strings and reals. }
program output;
var i: longint; x: real;
begin
  x := 0.5;
  for i := 1 to 1500000 do
  begin
    writeln('line ', i, ': value = ', i * 3:8, ' ', x:10:3);
    x := x + 0.25;
    if x > 1000.0 then
      x := 0.5;
  end;
end.
