program errors;
type
  TStd = procedure(x: integer); stdcall;
  TC = procedure(x: integer) cdecl;
  TF = function(x: integer): integer; cdecl; cdecl;
  IFoo = interface
    procedure A(x: integer); stdcall;
    procedure B(x: integer); cdecl;
  end;
  TK = class(TInterfacedObject, IFoo)
    procedure A(x: integer); cdecl;
    procedure B(x: integer);
    procedure M(x: integer); virtual; stdcall;
    procedure N(x: integer); virtual; cdecl;
  end;
  TL = class(TK)
    procedure M(x: integer); cdecl; override;
    procedure N(x: integer); override;
  end;
procedure X(x: integer); cdecl; forward;
procedure X(x: integer); begin end;
procedure Y(x: integer); stdcall; cdecl; begin end;
procedure D(x: integer); begin end;
procedure E(x: integer); cdecl; begin end;
procedure Takes(procedure p(x: integer)); begin end;
procedure TK.A(x: integer); begin end;
procedure TK.B(x: integer); begin end;
procedure TK.M(x: integer); begin end;
procedure TK.N(x: integer); cdecl; begin end;
procedure TL.M(x: integer); cdecl; begin end;
procedure TL.N(x: integer); begin end;
var
  s: TStd;
  c: TC;
begin
  s := @D;
  s := @E;
  c := @E;
  c := @D;
  c := s;
  Takes(@E);
end.
