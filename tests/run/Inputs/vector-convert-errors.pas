program vectorconverterrors;
type
  T8 = vector[8] of Byte;
  T16 = vector[16] of Byte;
  TD = vector[8] of Double;
var
  a: T8;
  b: T16;
  d: TD;
begin
  a := T8(b);
  a := T8(d);
end.
