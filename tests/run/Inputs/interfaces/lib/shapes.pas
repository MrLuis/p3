unit shapes;
{ An interface and a class implementing it, in a unit. }
interface
type
  IShape = interface ['IShape']
    function Area: integer;
    function Name: string;
  end;
  TRect = class(TObject, IShape)
    W, H: integer;
    constructor Create(aw, ah: integer);
    function Area: integer; virtual;
    function Name: string; virtual;
  end;
function Describe(s: IShape): string;
implementation
constructor TRect.Create(aw, ah: integer);
begin
  W := aw;
  H := ah;
end;
function TRect.Area: integer;
begin
  Area := W * H;
end;
function TRect.Name: string;
begin
  Name := 'rect';
end;
function Describe(s: IShape): string;
begin
  Describe := s.Name + chr(ord('0') + s.Area mod 10);
end;
end.
