{ IntToStr and UIntToStr write the digits from the end of a buffer
  (docs/sysutils-review.md SU-2): zero, one digit, powers of ten and the
  numbers either side of them, the extremes of int64 and qword, negative
  values, and a few thousand random values compared with a reference
  written here. Output identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-inttostr.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-inttostr.expected
}
program rtlinttostr;
{$H+}
uses sysutils;

var
  seed: int64;

{$Q-}{$R-}
function Rnd64: int64;
begin
  seed := seed * 6364136223846793005 + 1442695040888963407;
  Result := seed;
end;
{$Q+}{$R+}

{ the old way: a digit in front of the others }
function Reference(value: int64): string;
var
  digit: longint;
  negative: boolean;
begin
  negative := value < 0;
  Result := '';
  repeat
    digit := value mod 10;
    if digit < 0 then
      digit := -digit;
    Result := chr(ord('0') + digit) + Result;
    value := value div 10;
  until value = 0;
  if negative then
    Result := '-' + Result;
end;

function UReference(value: qword): string;
begin
  Result := '';
  repeat
    Result := chr(ord('0') + value mod 10) + Result;
    value := value div 10;
  until value = 0;
end;

var
  i, k, bad: longint;
  v: int64;
  p: int64;
begin
  writeln(IntToStr(int64(0)), ' ', IntToStr(int64(7)), ' ', IntToStr(int64(-7)), ' ', IntToStr(int64(10)), ' ', IntToStr(int64(-10)));
  writeln(IntToStr(int64(9223372036854775807)));
  writeln(IntToStr(int64(-9223372036854775807 - 1)));
  writeln(IntToStr(qword(18446744073709551615)));
  writeln(UIntToStr(qword(0)), ' ', UIntToStr(qword(18446744073709551615)), ' ', UIntToStr(qword(9223372036854775808)));
  p := 1;
  for i := 0 to 18 do
  begin
    writeln(IntToStr(p - 1), ' ', IntToStr(p), ' ', IntToStr(p + 1), ' ', IntToStr(-p));
    if i < 18 then
      p := p * 10;
  end;
  seed := 12345;
  bad := 0;
  for i := 1 to 5000 do
  begin
    v := Rnd64;
    for k := 0 to i mod 64 do
      v := v div 2;
    if (IntToStr(v) <> Reference(v)) or (UIntToStr(qword(v)) <> UReference(qword(v))) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
