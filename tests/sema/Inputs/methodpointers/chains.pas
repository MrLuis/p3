program chains;
type
  TE = procedure(x: longint) of object;
  TA = class
    procedure H(x: longint);
  end;
procedure TA.H(x: longint); begin end;
function Get(a: TA): TE; begin Get := @a.H; end;
function Num(x: longint): longint; begin Num := x; end;
var
  a: TA;
begin
  Num(1)(2);
  Get(a)();
  Get(a)('x');
  Get(a)(1)(2);
  Get(a, a)(1);
end.
