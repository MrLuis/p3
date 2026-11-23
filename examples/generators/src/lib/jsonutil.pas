unit jsonutil;
{ What the code of the [Json] generator calls. }
interface

function JsonString(const S: string): string;
function JsonBool(B: Boolean): string;

implementation

uses sysutils;

function JsonString(const S: string): string;
var
  I: Integer;
begin
  Result := '"';
  for I := 1 to Length(S) do
    case S[I] of
      '"': Result := Result + '\"';
      '\': Result := Result + '\\';
      #10: Result := Result + '\n';
      #0..#9, #11..#31: Result := Result + '\u00' + IntToHex(Ord(S[I]), 2);
    else
      Result := Result + S[I];
    end;
  Result := Result + '"';
end;

function JsonBool(B: Boolean): string;
begin
  if B then
    Result := 'true'
  else
    Result := 'false';
end;

end.
