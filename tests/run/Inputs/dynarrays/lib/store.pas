unit store;
{$H+}
interface
type
  TNames = array of string;
  TBox = class
    Items: TNames;
    procedure Add(const s: string);
  end;
var
  Shared: TNames;
function Make(n: longint): TNames;
implementation
procedure TBox.Add(const s: string);
begin
  SetLength(Items, length(Items) + 1);
  Items[high(Items)] := s;
end;
function Make(n: longint): TNames;
var i: longint;
begin
  SetLength(Result, n);
  for i := 0 to n - 1 do
    Result[i] := 'item';
end;
initialization
  SetLength(Shared, 2);
  Shared[1] := 'unit';
end.
