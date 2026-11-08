program more;
type
  TA = class
    procedure P;
    procedure V; virtual;
    function G: integer;
    constructor Create(I: integer);
  end;
  TB = class(TA)
    procedure V; override;
    procedure Shw;
  end;
procedure TA.P;
begin
  inherited;
  inherited P;
  Sh;
end;
procedure TA.V; begin end;
function TA.G: integer; begin end;
constructor TA.Create(I: integer); begin end;
procedure TB.V;
begin
  inherited V(1);
  Self := nil;
end;
procedure TB.Shw;
var x: TB;
begin
  x := TB.Create;
  x := TB.Create(1, 2);
  x.Show;
  P := 1;
  Q := @V;
end;
procedure TB.P; begin end;
type TC = class end;
var A: TA; I: integer; Q: pointer;
begin
  A := TA.Create(1);
  I := A.G;
  if A = 1 then;
  if A = nil then;
  if TB(A) = A then;
  A := TA(I);
  with A do G;
  x.Free;
end.
