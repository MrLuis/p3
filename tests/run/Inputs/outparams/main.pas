program main;
{$H+}
uses getter;
var
  s: ansistring;
  n: longint;
  o: TC;
begin
  s := 'old';
  Fill(s, n);
  writeln(s, ' ', n);
  o := TC.Create;
  o.Get(s);
  writeln(s);
  o.Free;
end.
