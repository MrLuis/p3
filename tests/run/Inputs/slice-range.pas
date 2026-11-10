program slicerange;
procedure P(const a: array of longint); begin writeln(length(a)); end;
var
  x: array[1..3] of longint;
begin
  P(slice(x, 2));
  P(slice(x, 4));
end.
