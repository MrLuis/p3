program tp;
type
  TRec = record a, b: integer; end;
var
  g: integer;
  r: TRec;
  constref: integer; { still a name }
procedure Show(constref x: integer; constref t: TRec);
begin
  g := g + 1;
  writeln(x, ' ', t.a + t.b, ' ', @x = @g);
end;
begin
  g := 1;
  r.a := 2;
  r.b := 3;
  constref := 7;
  Show(g, r);
  Show(constref * 2, r);
end.
