program errors;
type
  TF = procedure(out a: longint);
  TG = procedure(var a: longint);
  TC = class
    procedure M(out a: longint); virtual;
    procedure N(out a: longint);
  end;
  TD = class(TC)
    procedure M(var a: longint); override;
  end;
  I = interface
    procedure Q(out a: longint);
  end;
  TI = class(TInterfacedObject, I)
    procedure Q(var a: longint);
  end;
procedure TC.M(out a: longint); begin end;
procedure TC.N(var a: longint); begin end;
procedure TD.M(var a: longint); begin end;
procedure TI.Q(var a: longint); begin end;
procedure P1(out a: longint); begin a := 1 end;
procedure P2(out a: int64); begin a := 1 end;
procedure F1(out a: longint); forward;
procedure F1(var a: longint); begin end;
procedure F2(var a: longint); forward;
procedure F2(out a: longint); begin end;
var
  f: TF;
  g: TG;
  x: longint;
begin
  P1(1);
  P2(x);
  f := @P1;
  g := @P1;
  for x := 1 to 1 do
    P1(x);
end.
