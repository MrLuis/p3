program code;
function Pick(c: boolean; a, b: longint): longint;
begin
  Pick := if c then a else b;
end;
begin
  writeln(Pick(true, 1, 2));
end.
