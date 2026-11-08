program rterrors;
type
  TA = class
    F: integer;
    procedure P; virtual; abstract;
  end;
  TB = class(TA) end;
  TC = class(TA) procedure P; override; end;
procedure TC.P; begin writeln('C.P') end;
var A, N: TA; W: string;
begin
  A := TC.Create;
  A.P;
  W := paramstr(1);
  if W = 'as' then (A as TB).P;
  if W = 'abstract' then TB.Create.P;
  if W = 'nil' then N.P;
  if W = 'field' then N.F := 1;
end.
