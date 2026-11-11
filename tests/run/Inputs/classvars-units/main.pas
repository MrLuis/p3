program main;
{$H+}
{$modeswitch typehelpers}
uses cvunit;
var i: longint;
begin
  TCounter.Name := 'counter';
  TCounter.Add(3);
  TCounter.Add(4);
  i := 5;
  writeln(TCounter.Name, ' ', TCounter.Total, ' ', i.Twice, ' ', i.Twice, ' ', longint.Calls);
end.
