program main;
uses shapes;

procedure Run;
var
  mine: TShape owned;
begin
  mine := MakeShape('triangle');
  UseShapes(false);
  try
    UseShapes(true);
  except
    on e: Exception do
      writeln('caught ', e.Message);
  end;
end;

begin
  Run;
  writeln('done');
end.
