program main;
uses fmt;
var
  f: TFormatter;
begin
  f := TFormatter.Create;
  writeln(f.Show(Twice(3)), ' ', f.Show(Twice('ab')));
  f.Free;
end.
