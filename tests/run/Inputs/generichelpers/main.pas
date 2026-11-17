program main;
{$H+}
uses SysUtils, arrays;
type
  TInts = array of Integer;
var
  a: TInts;
  s: specialize TArray<string>;
begin
  a := [3, 4];
  s := a.Map(function(x) => '#' + IntToStr(x));
  Writeln(a.Count, ' ', s[0], ' ', s[1], ' ', s.Count);
end.
