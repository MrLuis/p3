program t;
type PInt = ^integer;
var p: PInt;
begin
  p := nil;
  p^ := 1
end.
