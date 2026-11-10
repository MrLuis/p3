program errors;
type
  TE = procedure(x: longint) of object;
  TP = procedure(x: longint);
  TA = class
    FE: TE;
    constructor Make;
    procedure H(x: longint); virtual; abstract;
    procedure J(x: longint);
    function GetE: TE;
    property PE: TE read GetE;
  end;
constructor TA.Make; begin end;
procedure TA.J(x: longint); begin end;
function TA.GetE: TE; begin GetE := FE; end;
procedure C(const e: TE); begin TMethod(e).Data := nil; end;
var
  a: TA;
  e: TE;
  p: TP;
  m: TMethod;
  q: pointer;
begin
  e := m;
  m := e;
  m := TMethod(p);
  m := TMethod(q);
  m := TMethod(1);
  if m = m then ;
  TMethod(a.PE).Data := nil;
  TMethod(@a.J).Data := nil;
  e := @TA.J;
  q := @a.J;
  q := @TA.H;
  q := @TA.Make;
  q := @TA.Free;
end.
