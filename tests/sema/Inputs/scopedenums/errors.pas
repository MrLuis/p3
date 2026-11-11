program errors;
{$scopedenums on}
type
  TColor = (Red, Green, Red);
  TOuter = class
  type
    TKind = (One, Two);
    function KindOf: TKind;
  end;
{$scopedenums off}
var
  c: TColor;
function TOuter.KindOf: TKind;
begin
  KindOf := One;
end;
begin
  c := Green;
  c := TColor.Nope;
  c := TColor.Green;
end.
