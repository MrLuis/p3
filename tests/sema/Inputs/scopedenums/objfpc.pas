program obj;
{$modeswitch typehelpers}
{$scopedenums on}
type
  TColor = (Red, Green, Blue);
  TColorHelper = type helper for TColor
    function Code: integer;
    class function Count: integer; static;
  end;
  TOuter = class
  type
    TKind = (One, Two);
  public
    function KindOf: TKind;
  end;
function TColorHelper.Code: integer;
begin
  Code := Ord(Self) * 10;
end;
class function TColorHelper.Count: integer;
begin
  Count := 3;
end;
function TOuter.KindOf: TKind;
begin
  KindOf := TKind.Two;
end;
function Pick(x: TColor = TColor.Green): integer;
begin
  Pick := Ord(x);
end;
var
  o: TOuter;
begin
  writeln(Pick, ' ', Pick(TColor.Blue));
  writeln(TColor.Blue.Code, ' ', TColor.Count);
  o := TOuter.Create;
  writeln(Ord(o.KindOf), ' ', Ord(TOuter.TKind.One));
  o.Free;
end.
