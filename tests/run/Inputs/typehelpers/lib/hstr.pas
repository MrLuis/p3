unit hstr;
{$H+}
{$modeswitch typehelpers}
interface
type
  TStrHelper = type helper for AnsiString
    function Shout: AnsiString;
    procedure Twice;
    class function Make(n: longint): AnsiString; static;
  end;
  TIntHelper = type helper for longint
    function Square: longint; inline;
  end;
implementation
function TStrHelper.Shout: AnsiString;
begin
  result := Self + '!';
end;
procedure TStrHelper.Twice;
begin
  Self := Self + Self;
end;
class function TStrHelper.Make(n: longint): AnsiString;
var i: longint;
begin
  result := '';
  for i := 1 to n do result := result + '*';
end;
function TIntHelper.Square: longint;
begin
  result := Self * Self;
end;
end.
