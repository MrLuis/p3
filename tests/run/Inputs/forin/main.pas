program main;
{$H+}
uses words;
var
  w: TWords;
  s: string;
  n: TNumber;
  d: longint;
begin
  w := TWords.Create;
  w.Add('unit');
  w.Add('enumerators');
  for s in w do
    write(s, ' ');
  writeln;
  n.value := 4072;
  for d in n do
    write(d);
  writeln;
  writeln(Total([1, 2, 3]));
  w.Free;
end.
