program errors;
{$H+}
type
  TColor = (Red, Green);

generic function Largest<T>(a, b: T): T;
begin
  if a > b then Result := a else Result := b;
end;

generic function Make<T>: T;
var none: T;
begin
  Result := none;
end;

generic function Conv<T, R>(v: T): R;
begin
  Result := v;
end;

generic function Two<T>(a: T): T;
begin
  Result := a;
end;

generic function Two<T, U>(a: T; b: U): T;
begin
  Result := a;
end;

generic function Clash<T>(a: T; b: T; c: T): T;
begin
  Result := a;
end;

generic function Clash<T, U>(a: T; b: U; c: U): T;
begin
  Result := a;
end;

generic procedure Swap2<T>(var a, b: T);
begin
end;

var
  x: Integer;
  s: string;
  b: Byte;

begin
  x := Largest(1, 'a');
  x := Largest(Red, 2);
  x := Make();
  Make();
  var v := Make();
  x := Conv(1);
  s := Conv(1);
  x := Two(1);
  x := Two(1, 's');
  x := Clash(1, 2, 3);
  x := Largest(1);
  x := Largest(1, 2, 3);
  x := Largest(nil, nil);
  Swap2(x, b);
  x := Nope(1);
  const k := 5;
  k := 6;
  inc(k);
  Swap2(k, x);
  const lst := [1, 2];
  lst := nil;
  var n := nil;
  var arr := [1, 'a'];
  var arr2 := [nil];
  var st := [];
  var ch := 'a';
  ch := x;
  x := 3.5;
end.
