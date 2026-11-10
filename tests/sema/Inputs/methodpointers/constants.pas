program constants;
type
  TE = procedure(x: longint) of object;
  TA = class
    procedure H(x: longint);
    class procedure C(x: longint);
  end;
procedure TA.H(x: longint); begin end;
class procedure TA.C(x: longint); begin end;
procedure Plain(x: longint); begin end;
var
  a: TA;
const
  K1: TE = 1;
  K2: TE = @a.H;
  K3: TE = @TA.C;
  K4: TE = @Plain;
  K5: array[0..0] of TE = (2);
var
  V1: TE = @a.H;
begin
end.
