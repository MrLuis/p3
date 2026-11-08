program errors; {$interfaces corba}
type
  IA = interface
    procedure P;
    function F(x: integer): integer;
  end;
  IB = interface(IA)
    procedure P;
    procedure Q; virtual;
  end;
  IC = interface(IMissing) end;
  ID = interface;
  TR = record x: integer; end;
  TX = class(TObject, IA)
    procedure P;
    function F(x: real): integer;
  end;
  TY = class(TObject, IA, IA, TR)
    procedure P;
    function F(x: integer): integer;
  end;
  TZ = class
  end;
var
  a: IA;
  b: IB;
  z: TZ;
  y: TY;
  n: integer;

procedure TX.P; begin end;
function TX.F(x: real): integer; begin F := 0 end;
procedure TY.P; begin end;
function TY.F(x: integer): integer; begin F := 1 end;

begin
  a := z;
  b := a;
  a := b;
  a := y;
  n := a;
  a.R;
  if a is IB then ;
  if y is IA then ;
  y := TY(a);
end.
