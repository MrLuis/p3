program tuple;
{$H+}
function Pair(a: longint; const s: string): (longint, string);
begin
  Pair := (a + 1, s);
end;

procedure run(x, y: longint);
var
  r: real;
  s: string;
begin
  (x, y) := (y, x);
  (r, s) := Pair(x, 'one');
  var (n, t) := Pair(y, s);
  x := n + length(t);
end;

begin
  run(1, 2);
end.
