program parse;
generic function NoParams(x: integer): integer; begin NoParams := x end;
generic function Constrained<T: class>(x: T): T; begin result := x end;
begin
end.
