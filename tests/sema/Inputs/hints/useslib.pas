program useslib;
uses oldlib;
var
  l: TLegacy;
begin
  Show(1);
  Show('one');
  l.x := Twice(2);
  Later;
end.
