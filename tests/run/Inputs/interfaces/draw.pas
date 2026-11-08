program draw;
uses shapes;
type
  { A descendant overriding the methods, and a class of the program
    implementing the unit's interface. }
  TSquare = class(TRect)
    function Name: string; override;
  end;
  TDot = class(TObject, IShape)
    function Area: integer;
    function Name: string;
  end;
function TSquare.Name: string;
begin
  Name := 'square';
end;
function TDot.Area: integer;
begin
  Area := 1;
end;
function TDot.Name: string;
begin
  Name := 'dot';
end;
var
  s: IShape;
  o: TObject;
begin
  writeln(Describe(TRect.Create(2, 3)), ' ', Describe(TSquare.Create(3, 3)),
          ' ', Describe(TDot.Create));
  o := TSquare.Create(1, 1);
  s := o as IShape;
  writeln(s.Name, ' ', o is IShape);
end.
