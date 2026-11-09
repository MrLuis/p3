program advrecords;
{$modeswitch advancedrecords}
type
  TP = record
  strict private
    FX: integer;
  public
    constructor Create(x: integer);
    function Get: integer;
    class function One: TP; static;
    class operator +(const a, b: TP): TP;
    class operator =(const a, b: TP): boolean;
    class operator :=(x: integer): TP;
    property X: integer read FX;
  end;
constructor TP.Create(x: integer); begin FX := x; end;
function TP.Get: integer; begin Get := FX; end;
class function TP.One: TP; begin Result.FX := 1; end;
class operator TP.+(const a, b: TP): TP; begin Result.FX := a.FX + b.FX; end;
class operator TP.=(const a, b: TP): boolean; begin Result := a.FX = b.FX; end;
class operator TP.:=(x: integer): TP; begin Result.FX := x; end;
var
  p: TP;
begin
  p := TP.Create(2) + 3;
  if p <> TP.One then
    writeln(p.Get, p.X);
end.
