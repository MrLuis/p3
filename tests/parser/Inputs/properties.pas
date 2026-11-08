program properties;
type
  TA = class
  strict private
    F: integer;
  strict protected
    function G(i: integer; const s: string): integer;
    function H(k: integer): integer;
  public
    property P[i: integer; const s: string]: integer read G; default;
    property X: integer index 2 read H default 4;
    property Y: integer read F nodefault;
  end;
function TA.G(i: integer; const s: string): integer; begin G := i end;
function TA.H(k: integer): integer; begin H := k end;
var
  a: TA;
begin
  a := TA.Create;
  writeln(a.P[1, 'x'], a[2, 'y'], a.X);
end.
