program main;
{$H+}
uses shapes;
var
  s: TShapes;
  sh: TShapes.TShape;
  p: TShapes.TPoint;
begin
  s := TShapes.Create;
  sh := s.Make(High(TShapes.TKind));
  writeln(sh.Name, ' ', sh.ClassName, ' ', TShapes.Sides, ' ', TShapes.Count);
  sh.Free;
  sh := TShapes.TShape.Create(Low(TShapes.TKind));
  writeln(sh.Name, ' ', TShapes.Count);
  sh.Free;
  p.x := -3;
  p.y := 4;
  writeln(p.Dist);
  s.Free;
end.
