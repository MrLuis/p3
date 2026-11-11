program errors;
generic function Bad<T>(x: T): T;
begin
  result := x + 1;
end;
generic function Dup<T, T>(x: T): T;
begin
  result := x;
end;
generic function Twice<T>(x: T): T;
begin
  result := x;
end;
generic function Twice<T>(x: T): T;
begin
  result := x;
end;
generic function Later<T>(x: T): T; forward;
procedure Outer;
  generic function Local<T>(x: T): T; begin result := x end;
begin
end;
function Plain(x: integer): integer; begin Plain := x end;
var
  i: integer;
  b: boolean;
begin
  i := specialize Bad<integer>(1);
  b := specialize Bad<boolean>(true);
  i := specialize Plain<integer>(1);
  i := specialize Twice<integer, integer>(1);
  i := specialize Nope<integer>(1);
  i := specialize Later<integer>(1);
  i := specialize Twice<TNope>(1);
end.
