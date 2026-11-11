program main;
{$H+}
{$modeswitch typehelpers}
uses hstr;
var s: AnsiString; i: longint;
begin
  s := 'hey';
  s.Twice;
  writeln(s.Shout, ' ', AnsiString.Make(3), ' ', 'xy'.Shout);
  i := 7;
  writeln(i.Square);
end.
