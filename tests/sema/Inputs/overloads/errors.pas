program errors;
type
  TA = class
    procedure M(x: integer); overload;
    procedure M(const s: string); overload;
    procedure M(y: integer);
    procedure N(x: integer);
    function N(x: integer): integer;
  end;
  TP = procedure(x: char);
var

  w: word;
  a: TA;
  p: TP;

procedure P1(x: byte); overload; begin end;
procedure P1(x: word); overload; begin end;
procedure P2(x: integer); begin end;
procedure P2(x: real); begin end;
procedure P2(y: integer); begin end;
function F(x: integer): integer; begin F := x end;
function F(x: integer): real; begin F := x end;
procedure V(var x: integer); overload; begin end;
procedure V(var x: real); overload; begin end;

procedure TA.M(x: integer); begin end;
procedure TA.M(const s: string); begin end;
procedure TA.M(x: real); begin end;
procedure TA.N(x: integer); begin end;

begin
  P1(5);
  P1(w);
  P2('x');
  P2(1, 2);
  V(w);
  V(3);
  a.M(true);
  p := @P2;
end.
