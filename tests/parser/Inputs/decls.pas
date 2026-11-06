program a;
const N = 3;
type V = array [1..N] of integer;
var x: V;
function sq(k: integer): integer;
begin
  sq := k * k
end;
begin
  x[1] := sq(N)
end.
