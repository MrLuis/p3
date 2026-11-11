program errors;
{$H+}
{$modeswitch advancedrecords}
type
  TR = record a: longint; end;
  TS = record b: longint; end;
  TC = class end;
  TInt = record helper for longint end;
  TBad = record helper(TC) for TR end;
  TH = record helper for TR
    function Twice: longint;
    procedure NoBody;
    procedure SetIt(v: longint);
    class function Make: longint; static;
    property Doubled: longint read Twice write SetIt;
    function Virt: longint; virtual;
  end;
  TBad2 = record helper(TH) for TS end;
  TOuter = class
  type
    TN = record helper for TR end;
  end;
var
  v: TH;
  r: TR;
  i: longint;
  x: record helper for TR end;
procedure Local;
type
  TL = record helper for TR end;
begin
end;
function TH.Twice: longint;
begin
  result := a * 2;
end;
procedure TH.SetIt(v: longint);
begin
  a := v;
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
  i := TR.Twice;
  i := TR.Nope;
  r.Doubled := 3;
  i := r.Nope;
end.
