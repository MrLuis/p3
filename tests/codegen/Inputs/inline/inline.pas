program inline_;
function Sq(x: integer): integer; inline;
begin
  Sq := x * x;
end;
{$inline off}
function Cube(x: integer): integer; inline;
begin
  Cube := x * x * x;
end;
{$inline on}
begin
  writeln(Sq(3), Cube(2));
end.
