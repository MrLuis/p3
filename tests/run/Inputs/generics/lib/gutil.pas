unit gutil;
{$H+}
interface
const
  Sep = ', ';
generic function Twice<T>(x: T): T;
generic procedure WriteAll<T>(const items: array of T);
implementation
generic function Twice<T>(x: T): T;
begin
  result := x + x;
end;
generic procedure WriteAll<T>(const items: array of T);
var
  i: longint;
begin
  for i := 0 to high(items) do
  begin
    if i > 0 then
      write(Sep);
    write(items[i]);
  end;
  writeln;
end;
end.
