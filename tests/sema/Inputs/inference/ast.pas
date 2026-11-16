program ast;
{$H+}
generic function Largest<T>(a, b: T): T;
begin
  if a > b then Result := a else Result := b;
end;
generic function Make<T>: T;
var none: T;
begin
  Result := none;
end;
var
  r: Real;
begin
  r := Largest(1, 2.5);
  r := Make();
  const k := Largest('a', 'b');
  var xs := [1, 2, 3];
  var c := 'x';
end.
