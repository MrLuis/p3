program recforward;
type TBig = record a, b, c, d: int64; end;
function Make(n: longint): TBig;
begin
  Result.a := n; Result.b := n; Result.c := n; Result.d := n;
end;
function Rot(b: TBig): TBig;
begin
  Result.a := b.d; Result.b := b.c; Result.c := b.b; Result.d := b.a;
end;
function Forward(n: longint): TBig;
begin
  Result := Make(n);
end;
function Mentions(n: longint): TBig;
begin
  Result := Rot(Result);
end;
function Guarded(n: longint): TBig;
begin
  try
    Result := Make(n);
  except
  end;
end;
var x: TBig;
begin
  x := Forward(1);
  x := Mentions(2);
  x := Guarded(3);
  writeln(x.a);
end.
