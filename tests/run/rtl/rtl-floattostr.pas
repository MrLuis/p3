{ FloatToStr reads the C text in its buffer and writes its result into a
  second one (docs/sysutils-review.md SU-4). Fixed notation and exponent
  notation on either side of the limits (below 1E-5 and from 1E15), signs,
  values with trailing zeros, whole numbers, three-digit exponents, the
  largest and smallest doubles, 0, and 3,000 random values over every
  exponent (Nan and the infinities are in run/rtl-sysutils.pas). The
  operands are variables: Free Pascal folds constant divisions in single
  precision. Output identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-floattostr.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-floattostr.expected
}
program rtlfloattostr;
{$H+}
uses sysutils;

var
  seed: int64;
  ten, zero, one, two, three, seven, hundred, big, tiny: double;

{$Q-}{$R-}
function Rnd(n: longint): longint;
begin
  seed := seed * 6364136223846793005 + 1442695040888963407;
  Result := longint((seed shr 33) mod n);
end;
{$Q+}{$R+}

procedure P(v: double);
begin
  writeln(FloatToStr(v));
end;

var
  i, k, e: longint;
  v, m: double;
begin
  zero := 0.0;
  ten := 10.0;
  one := 1.0; two := 2.0; three := 3.0; seven := 7.0; hundred := 100.0; big := 1e9; tiny := 1e-5;
  P(0.0); P(1.0); P(-1.0); P(0.5); P(-0.5); P(1.5); P(10.0); P(100.0); P(123456789.0);
  P(0.1); P(0.25); P(0.000123); P(0.00001); P(0.000001); P(0.0000123456);
  P(123456789012345.0); P(999999999999999.0); P(1000000000000000.0); P(1234567890123456.0);
  P(1e15); P(1e16); P(1.5e20); P(-2.5e-7); P(1e-5); P(9.99999e-6); P(1.0e100); P(1.5e-100);
  P(1.7976931348623157e308); P(-1.7976931348623157e308); P(5e-324); P(2.2250738585072014e-308);
  P(1e300); P(1e-300); P(1.23e-310);
  P(3.14159265358979); P(2.718281828459045); P(one / three); P(two / three); P(hundred / seven); P(big * 10 / three);
  P(12345.678); P(0.30000000000000004); P(1e22); P(1e21); P(4.35); P(99.99); P(tiny / three);
  seed := 2024;
  for i := 1 to 3000 do
  begin
    m := (Rnd(1000000000) + 1) / big + Rnd(9);
    e := Rnd(40) - 20;
    v := m;
    for k := 1 to abs(e) do
      if e < 0 then
        v := v / ten
      else
        v := v * ten;
    if Rnd(2) = 0 then
      v := -v;
    if i mod 100 = 0 then
    begin
      v := m;
      for k := 1 to 150 do
        if i mod 200 = 0 then
          v := v / ten
        else
          v := v * ten;
    end;
    writeln(FloatToStr(v));
  end;
end.
