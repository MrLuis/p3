program p;
type
  PA = ^Missing;
  S1 = set of integer;
  S2 = set of real;
  PI = ^integer;
  Small = set of 1..5;
var
  p: PI; q: ^char; i: integer; s: Small; c: char;
begin
  p := q;
  i := p;
  new(i);
  dispose(p, 1);
  i^ := 1;
  s := [1, 'a'];
  s := [300];
  if c in s then;
  if s < [1] then;
  writeln(p)
end.
