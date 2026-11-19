program recresult;
type
  TSmall = record a, b: longint; end;
  TTwo = record a, b: int64; end;
  TBig = record a, b, c: int64; end;
function MakeSmall(n: longint): TSmall;
begin
  Result.a := n; Result.b := n;
end;
function MakeTwo(n: longint): TTwo;
begin
  Result.a := n; Result.b := n;
end;
function MakeBig(n: longint): TBig;
begin
  Result.a := n; Result.b := n; Result.c := n;
end;
var s: TSmall; t: TTwo; b: TBig;
begin
  s := MakeSmall(1);
  t := MakeTwo(2);
  b := MakeBig(3);
  writeln(s.a, t.a, b.a);
end.
