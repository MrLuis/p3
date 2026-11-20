{ IntToHex makes its result once at its final length and fills it from the
  end (docs/sysutils-review.md SU-5): zero with 0, 1 and 16 digits,
  values of every length with several minimum widths (also more than 16),
  negative values (16 digits, two's complement), the longint overload, and
  random values against a reference written here. Output identical to Free
  Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-inttohex.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-inttohex.expected
}
program rtlinttohex;
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

{ the old way: one digit appended at a time }
function Reference(value: int64; digits: longint): string;
const
  hex = '0123456789ABCDEF';
var
  i, used: longint;
begin
  used := 16;
  while (used > 1) and (((value shr (4 * (used - 1))) and 15) = 0) do
    used := used - 1;
  Result := '';
  for i := used + 1 to digits do
    Result := Result + '0';
  for i := used - 1 downto 0 do
    Result := Result + hex[((value shr (4 * i)) and 15) + 1];
end;

var
  i, w, bad: longint;
  v: int64;
begin
  writeln(IntToHex(int64(0), 0), ' ', IntToHex(int64(0), 1), ' ', IntToHex(int64(0), 4), ' ', IntToHex(int64(0), 16));
  writeln(IntToHex(int64(255), 1), ' ', IntToHex(int64(255), 2), ' ', IntToHex(int64(255), 8), ' ', IntToHex(int64(256), 2));
  writeln(IntToHex(int64(-1), 1), ' ', IntToHex(int64(-1), 20), ' ', IntToHex(int64(-256), 4));
  writeln(IntToHex(int64(9223372036854775807), 1), ' ', IntToHex(int64(-9223372036854775807 - 1), 1));
  writeln(IntToHex(int64(48879), 20));
  writeln(IntToHex(longint(0), 4), ' ', IntToHex(longint(-1), 1), ' ', IntToHex(longint(48879), 8), ' ', IntToHex(longint(-2147483647 - 1), 2));
  for i := 0 to 15 do
  begin
    v := int64(1) shl (4 * i);
    writeln(IntToHex(v - 1, 1), ' ', IntToHex(v, 3), ' ', IntToHex(v, 20));
  end;
  seed := 777;
  bad := 0;
  for i := 1 to 5000 do
  begin
    v := Rnd64;
    for w := 0 to i mod 64 do
      v := v div 2;
    w := i mod 24;
    if IntToHex(v, w) <> Reference(v, w) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
