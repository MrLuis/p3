program errors;
type
  TRec = record x: integer; end;
  TBad = class of TRec;
  TNone = class of TMissing;
  TA = class
    F: integer;
    class var CV: integer;
    procedure M;
    class procedure CM;
    class function CF: integer; virtual;
    property P: integer read F;
  end;
  TAClass = class of TA;
  TB = class(TA)
    class function CF: integer;
    function CG: integer; override;
  end;
  TOther = class
  end;
  TOtherClass = class of TOther;
var
  a: TA;
  c: TAClass;
  o: TOtherClass;
  n: integer;

procedure TA.M; begin end;
class procedure TA.CM;
begin
  F := 1;
  M;
  n := P;
  CV := 2;
  CF;
end;
class function TA.CF: integer; begin CF := 0 end;
procedure TA.CM; begin end;
class function TB.CF: integer; begin CF := 1 end;
function TB.CG: integer; begin CG := 2 end;
class procedure Free;
begin
end;

begin
  c := TA;
  c := TB;
  c := TOther;
  o := c;
  c := a;
  n := TA.F;
  TA.M;
  c.M;
  n := c.F;
  TA.CM;
  c.CV := 3;
  if c = o then ;
  if c = TOther then ;
end.
