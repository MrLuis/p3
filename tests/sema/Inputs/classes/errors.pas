program errors;
type
  TA = class
  private
    FX: integer;
  public
    procedure P; virtual;
    procedure Q;
    function F: integer; virtual; abstract;
    procedure R; abstract;
    property X: integer read FX;
    property Y: integer write FX;
    property Z: integer read Nope;
    property W: real read FX;
  end;
  TB = class(TA)
    procedure Q; override;
    procedure P(I: integer); override;
    FX: integer;
    constructor Make; virtual;
  end;
  TC = class(TNoSuch)
  end;
  TD = class;
  TE = class(TD) end;
  TF = class(integer) end;
  TG = class(TG) end;
var
  A: TA;
  B: TB;
  I: integer;
  R: record x: integer end;
procedure TA.P; begin end;
procedure TA.Nope; begin end;
function TA.P: integer; begin end;
procedure TZ.P; begin end;
procedure TA.Q(K: integer); begin end;
procedure TA.F; begin end;
begin
  A := TA.Create;
  A.X := 1;
  I := A.Y;
  I := A.Missing;
  A.Q(1);
  B := A;
  A := B;
  I := A;
  if A is integer then;
  if I is TA then;
  if A is TB then;
  if R is TA then;
  A := TA.P;
  inherited P;
  A.FX := 3;
  writeln(A);
  I := A.Prop;
  A.P := 2;
  with R do A.Q;
end.
