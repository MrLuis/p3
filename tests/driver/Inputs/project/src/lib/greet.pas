unit greet;
{ a unit found through the project's unitPaths }
interface
procedure Greet(const who: string);
implementation
procedure Greet(const who: string);
begin
  writeln('hello, ', who);
end;
end.
