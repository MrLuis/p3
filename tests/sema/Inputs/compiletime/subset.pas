program subset;
{ What a compiletime function may not do (none is evaluated here: no
  constant uses them). }
type
  TT = array[0..3] of longint;
  PInt = ^integer;
var
  g: integer;
const
  tc: integer = 5;
function Helper(x: integer): integer;
begin
  Helper := x;
end;
function Sq(x: integer): integer; compiletime;
begin
  Sq := x * x;
end;
function Bad(n: integer): TT; compiletime;
var
  i: integer;
  p: PInt;
  o: TObject;
  function Inner(k: integer): integer;
  begin
    Inner := k + i + Sq(k);
  end;
begin
  for i := 0 to 3 do
    Result[i] := Inner(i) + tc + length('abc');
  writeln('x');
  g := 1;
  i := Helper(2);
  tc := 3;
  p := @i;
  o := TObject.Create;
  o.Free;
  raise Exception.Create('no');
end;
begin
  writeln(Sq(3), Bad(1)[2]);
end.
