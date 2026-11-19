program tabcalls;
type TC = class
  constructor Create;
  procedure M;
end;
procedure P(n: longint); begin end;
constructor TC.Create; begin end;
procedure TC.M; begin end;
var C: TC; FP: procedure(n: longint);
begin
  FP := @P;
  try
    P(1);
    FP(2);
    C := TC.Create;
    C.M;
  except end
end.
