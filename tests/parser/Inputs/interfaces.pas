program interfaces;
type
  IA = interface;
  IA = interface ['{A-GUID}']
    procedure P;
  end;
  IB = interface(IA)
    function F(x: integer): integer; overload;
    function F(const s: string): integer; overload;
  end;
  TC = class(TObject, IB)
    procedure P;
    function F(x: integer): integer; overload;
    function F(const s: string): integer; overload;
  end;
procedure TC.P; begin end;
function TC.F(x: integer): integer; begin F := x end;
function TC.F(const s: string): integer; begin F := length(s) end;
var
  b: IB;
  a: IA;
begin
  b := TC.Create;
  a := b;
  a.P;
  writeln(b.F(2), b.F('x'), TObject(nil) is IA);
end.
