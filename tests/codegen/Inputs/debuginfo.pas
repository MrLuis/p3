program Dbg(output);
type
  Color = (red, green, blue);
  Point = record x, y: integer; c: Color end;
  Grid = array [1..3] of integer;
var
  p: Point;
  g: Grid;
  total: integer;

procedure accumulate(var acc: integer; step: integer);
var doubled: integer;
  procedure bump;
  begin
    acc := acc + doubled
  end;
begin
  doubled := step * 2;
  bump
end;

begin
  p.x := 3; p.y := 4; p.c := green;
  g[1] := 10; g[2] := 20; g[3] := 30;
  total := 0;
  accumulate(total, 5);
  writeln(total)
end.
