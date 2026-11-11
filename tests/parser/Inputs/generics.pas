program generics;
generic function Id<T>(x: T): T;
begin
  result := x;
end;
begin
  writeln(specialize Id<integer>(1));
  specialize Id<char>('c');
end.
