program overloads;
type
  TA = class
    procedure M(x: integer); overload;
    procedure M(const s: string); overload;
  end;
procedure TA.M(x: integer); begin end;
procedure TA.M(const s: string); begin end;
function F(x: integer): integer; overload; begin F := x end;
function F(x: real): real; overload; begin F := x end;
var
  a: TA;
begin
  a := TA.Create;
  a.M(1);
  a.M('s');
  writeln(F(1), F(2.5));
end.
