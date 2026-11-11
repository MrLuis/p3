unit shapes;
{$H+}
{$modeswitch advancedrecords}
interface
type
  TShapes = class
  public
    const Sides = 4;
    type
      TKind = (skSquare, skTriangle);
      TPoint = record
        x, y: longint;
        function Dist: longint;
      end;
      TShape = class
        Kind: TKind;
        constructor Create(k: TKind);
        function Name: string; virtual;
      end;
    class var Count: longint;
    function Make(k: TKind): TShape;
  end;
implementation
function TShapes.TPoint.Dist: longint;
begin
  result := abs(x) + abs(y);
end;
constructor TShapes.TShape.Create(k: TKind);
begin
  Kind := k;
  inc(Count);
end;
function TShapes.TShape.Name: string;
begin
  if Kind = skSquare then
    result := 'square'
  else
    result := 'triangle';
end;
function TShapes.Make(k: TKind): TShape;
begin
  result := TShape.Create(k);
end;
end.
