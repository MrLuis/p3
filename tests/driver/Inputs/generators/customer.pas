unit customer;
{$modeswitch advancedrecords}
interface

type
  [Serializable]
  TCustomer = partial record
    Id: Integer;
    Name: string;
    Active: Boolean;
    [Skip] Cache: Pointer;
  end;

function BoolToJson(B: Boolean): string;

implementation

uses sysutils;

function BoolToJson(B: Boolean): string;
begin
  if B then
    Result := 'true'
  else
    Result := 'false';
end;

end.
