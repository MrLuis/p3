program errors;
{$H+}
{$modeswitch typehelpers}
type
  TC = class end;
  TBad = type helper(TC) for longint end;
  TH = type helper for longint
    function Twice: longint;
    procedure NoBody;
    procedure SetIt(v: longint);
    class function Make: longint; static;
    property Doubled: longint read Twice write SetIt;
    function Virt: longint; virtual;
  end;
  TBad2 = type helper(TH) for byte end;
  TCH = type helper for TC end;
  TOuter = class
  type
    TN = type helper for longint end;
  end;
var
  v: TH;
  i: longint;
procedure Local;
type
  TL = type helper for byte end;
begin
end;
function TH.Twice: longint;
begin
  result := Self * 2;
end;
procedure TH.SetIt(v: longint);
begin
  Self := v;
end;
class function TH.Make: longint;
begin
  result := 1;
end;
function TH.Virt: longint;
begin
  result := 0;
end;
begin
  i := TH.Make;
  i := longint.Twice;
  i := longint.Nope;
  i.Doubled := 3;
  i := i.Nope;
end.
