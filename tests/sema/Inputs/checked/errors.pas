program errors;
var
  x: Integer;
  r: Real;
  t: (Integer, Boolean);
begin
  t := AddChecked(x, r);
  t := MulChecked(x);
  t := SubChecked('a', 1);
end.
