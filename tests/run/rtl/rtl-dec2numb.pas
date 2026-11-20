{ Dec2Numb writes its digits and padding into one result
  (docs/strutils-review.md ST-9): bases 2, 7, 10, 16 and 36, len shorter
  than the digits (not cut), equal, longer and 0, the largest longint,
  and random calls (bases 2 to 36) against the old algorithm. Output
  identical to Free Pascal 3.2.2's (fpc -Mobjfpc). Checked against the old
  algorithm only under p3: int64 values (Free Pascal's n is a longint), and
  0 and negative n, where p3 gives only the padding and Free Pascal '0'
  padded and '' (a difference older than this test).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-dec2numb.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-dec2numb.expected
}
program rtldec2numb;
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

{ the old way: each digit in front, then the padding }
function OldDec2Numb(n: int64; len, base: byte): string;
const
  digits: string[36] = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ';
var
  r: string;
begin
  r := '';
  while n > 0 do
  begin
    r := digits[n mod base + 1] + r;
    n := n div base;
  end;
  Result := AddChar('0', r, len);
end;

procedure Show(n: longint; len, base: byte);
begin
  writeln('Dec2Numb(', n, ', ', len, ', ', base, ') = "', Dec2Numb(n, len, base), '"');
end;

var
  i, bad: longint;
  n: int64;
  len, base: byte;
begin
  Show(255, 0, 2);
  Show(255, 8, 2);
  Show(255, 12, 2);
  Show(255, 4, 16);
  Show(255, 1, 16);
  Show(123456, 12, 7);
  Show(123456, 3, 10);
  Show(123456, 6, 10);
  Show(123456789, 0, 36);
  Show(35, 0, 36);
  Show(2147483647, 0, 2);
  Show(2147483647, 40, 2);
  Show(2147483647, 0, 36);
  Show(1, 255, 10);

  seed := 71;
  bad := 0;
  for i := 1 to 4000 do
  begin
    n := int64(Rnd(65536)) * Rnd(32768) + 1;
    len := Rnd(30);
    base := 2 + Rnd(35);
    if Dec2Numb(n, len, base) <> OldDec2Numb(n, len, base) then
      bad := bad + 1;
{$IFDEF P3_OBJFPC}
    n := int64(Rnd(65536)) * Rnd(65536) * Rnd(65536) * Rnd(32767);
    if Dec2Numb(n, len, base) <> OldDec2Numb(n, len, base) then
      bad := bad + 1;
    n := -int64(Rnd(65536)) * Rnd(65536);
    if Dec2Numb(n, len, base) <> OldDec2Numb(n, len, base) then
      bad := bad + 1;
    if Dec2Numb(0, len, base) <> OldDec2Numb(0, len, base) then
      bad := bad + 1;
{$ENDIF}
  end;
{$IFDEF P3_OBJFPC}
  for len := 0 to 70 do
    for base := 2 to 36 do
      if Dec2Numb(9223372036854775807, len, base) <> OldDec2Numb(9223372036854775807, len, base) then
        bad := bad + 1;
{$ENDIF}
  writeln('random differences: ', bad);
end.
