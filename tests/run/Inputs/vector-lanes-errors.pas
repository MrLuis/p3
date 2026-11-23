program vectorlaneserrors;
var
  a: vector[16] of Byte;
  b: vector[8] of Byte;
  r: vector[4] of Double;
  m: vector[16] of Boolean;
begin
  m := a = b;
  r := r and r;
  r := not r;
end.
