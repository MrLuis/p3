{ IntToBin makes the result once and writes each bit in place
  (docs/strutils-review.md ST-3): 0 and -1 with 1, 8, 32 and 64 digits,
  digits 0, negative and above 64 (the digits past the 64th are '0', as in
  Free Pascal), values wider than digits (high bits dropped), the smallest
  int64, and random calls against the old algorithm (up to 64 digits).
  Output identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-inttobin.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-inttobin.expected
}
program rtlinttobin;
{$H+}
uses sysutils, strutils;

var
  seed: longint;

{$Q-}{$R-}
function Rnd(n: longint): longint;
begin
  seed := (seed * 1103515245 + 12345) and $7FFFFFFF;
  Result := (seed shr 8) mod n;
end;
{$Q+}{$R+}

{ the old way, up to 64 digits: one digit at a time }
function OldBin(value: int64; digits: longint): string;
var
  i: longint;
begin
  Result := '';
  for i := digits - 1 downto 0 do
    if (value shr i) and 1 = 1 then
      Result := Result + '1'
    else
      Result := Result + '0';
end;

procedure Show(v: int64; d: longint);
begin
  writeln('IntToBin(', v, ', ', d, ') = "', IntToBin(v, d), '"');
end;

var
  i, d, bad: longint;
  v: int64;
begin
  Show(0, 1);
  Show(0, 8);
  Show(0, 32);
  Show(0, 64);
  Show(-1, 1);
  Show(-1, 8);
  Show(-1, 32);
  Show(-1, 64);
  Show(5, 0);
  Show(5, -1);
  Show(5, -100);
  Show(5, 65);
  Show(5, 70);
  Show(-1, 66);
  Show(-5, 70);
  Show($123456789, 80);
  Show(255, 4);
  Show($F0F0, 8);
  Show(-9223372036854775807 - 1, 64);
  Show(-9223372036854775807 - 1, 65);
  Show(9223372036854775807, 64);
  writeln('length ', length(IntToBin(1, 200)), ' ', pos('1', IntToBin(1, 200)));

  seed := 31;
  bad := 0;
  for i := 1 to 4000 do
  begin
    v := int64(Rnd(65536)) * Rnd(65536) * Rnd(65536);
    if Rnd(2) = 0 then
      v := -v;
    d := Rnd(66) - 1;
    if IntToBin(v, d) <> OldBin(v, d) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
