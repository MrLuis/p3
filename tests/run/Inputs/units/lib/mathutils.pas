unit MathUtils;
interface
const Pi2 = 6.28;
var calls: integer;
function square(x: integer): integer;
function gcd(a, b: integer): integer;
implementation
function helper(x: integer): integer;   { private to the unit }
begin
  helper := x * x
end;
function square(x: integer): integer;
begin
  calls := calls + 1;
  square := helper(x)
end;
function gcd(a, b: integer): integer;
var t: integer;
begin
  while b <> 0 do begin t := a mod b; a := b; b := t end;
  gcd := a
end;
initialization
  calls := 0;
  writeln('MathUtils ready')
end.
