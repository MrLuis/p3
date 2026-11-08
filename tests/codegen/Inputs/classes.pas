program classes;
type
  TA = class
    X: integer;
    procedure V(N: integer); virtual;
    procedure S;
  end;
  TB = class(TA)
    Y: real;
    procedure V(N: integer); override;
  end;
procedure TA.V(N: integer); begin X := N end;
procedure TA.S; begin V(1) end;
procedure TB.V(N: integer); begin inherited V(N + 1) end;
var A: TA;
begin
  A := TB.Create;
  A.V(2);
  A.S;
  if A is TB then A.Free;
end.
