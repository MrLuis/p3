program tp;
var
  try, raise: integer;
  e: Exception;
  z: EDivByZero;
begin
  try := 1;
  raise := try;
end.
