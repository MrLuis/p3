program kinds;
type
  TOld = integer deprecated;
  TMsg = integer deprecated 'use TNew';
  TExp = integer experimental;
  TPlat = integer platform;
  TC = class
    F: integer deprecated;
    procedure M; deprecated 'use N';
    property P: integer read F; deprecated;
  end;
  TR = record
    a: integer deprecated;
  end;
const
  K = 5 deprecated;
  KT: integer = 6 deprecated 'gone';
var
  v: integer deprecated;
  w: integer platform;
  u: integer unimplemented;
  l: integer library;
procedure Old; deprecated;
begin end;
procedure Old2; deprecated 'use New';
begin end;
function F(x: integer): integer; experimental;
begin F := x; end;
procedure TC.M; begin end;
var
  a: TOld; b: TMsg; c: TExp; d: TPlat; o: TC; r: TR;
begin
  v := K + KT;
  w := u + l;
  Old; Old2;
  v := F(1);
  o := TC.Create; o.M; v := o.F + o.P;
  r.a := 1;
end.
