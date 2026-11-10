program main;
{$H+}
uses store;
var
  n: TNames;
  b: TBox;
begin
  n := Make(2);
  writeln(length(n), ' ', n[1], ' ', Shared[1]);
  b := TBox.Create;
  b.Add('x');
  b.Add('y');
  writeln(length(b.Items), ' ', b.Items[1]);
  b.Free;
  Shared := n;
  n[0] := 'changed';
  writeln(Shared[0]);
end.
