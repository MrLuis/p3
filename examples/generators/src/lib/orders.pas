unit orders;
{ Types handled by the generators of this project (generators/), and a
  table computed while compiling. }
{$modeswitch advancedrecords}
interface

uses jsonutil;

type
  [EnumNames]
  TStatus = (Pending, Paid, Shipped, Cancelled);

  [Json]
  TCustomer = partial record
    Id: Integer;
    Name: string;
    [JsonName('vip')] IsVip: Boolean;
  end;

  [Json]
  TOrder = partial record
    Number: Integer;
    Customer: TCustomer;
    Status: TStatus;
    Total: Double;
    Note: string;
    [Skip] Checked: Boolean;
  end;

  TCrcTable = array[0..255] of LongWord;

{ CRC-32 of S, with a table computed by the compiler }
function Crc32(const S: string): LongWord;

implementation

uses sysutils; { (the generated code uses IntToStr, FloatToStr, LowerCase) }

function BuildCrcTable: TCrcTable; compiletime;
var
  I, J: Integer;
  C: LongWord;
begin
  for I := 0 to 255 do
  begin
    C := I;
    for J := 1 to 8 do
      if Odd(C) then
        C := (C shr 1) xor $EDB88320
      else
        C := C shr 1;
    Result[I] := C;
  end;
end;

const
  CrcTable = BuildCrcTable;

function Crc32(const S: string): LongWord;
var
  I: Integer;
begin
  Result := $FFFFFFFF;
  for I := 1 to Length(S) do
    Result := CrcTable[(Result xor Ord(S[I])) and $FF] xor (Result shr 8);
  Result := not Result;
end;

end.
