program shop;
{$modeswitch advancedrecords}
uses money;
var
  a, b: TMoney;
begin
  a := TMoney.Create(1, 50);
  b := 75;
  a.Add(5);
  writeln(a.Str, ' ', b.Str, ' ', (a + b).Str, ' ', a.Cents, ' ',
          a = b, ' ', a <> b, ' ', TMoney.Zero.Str);
end.
