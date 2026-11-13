unit config;
interface
type
  TConfig = class
  private
    class var FLevel: integer;
    class var FName: string;
    class function GetLabel: string; static;
    class procedure SetLevel(v: integer); static;
  public
    class property Level: integer read FLevel write SetLevel;
    class property Name: string read FName write FName;
    class property Caption: string read GetLabel;
  end;
implementation
class function TConfig.GetLabel: string;
begin
  GetLabel := FName + '#' + chr(ord('0') + FLevel);
end;

class procedure TConfig.SetLevel(v: integer);
begin
  if v > 9 then
    v := 9;
  FLevel := v;
end;
end.
