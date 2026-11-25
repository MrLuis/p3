{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
program d01_ints;
{ Dialect check L1 (sized integers, double) and L10 (wrapping arithmetic,
  bit operators). }
const
  key = 'pcc';
var
  b: byte; w: word; si: shortint; sm: smallint;
  li: longint; c: cardinal; i6: int64; d: double;
  h: cardinal; k: longint;
begin
  b := 200; w := 60000; si := -100; sm := -30000;
  li := 2000000000; c := 4000000000; i6 := 9000000000000000000;
  writeln(b, ' ', w, ' ', si, ' ', sm, ' ', li, ' ', c, ' ', i6);
  i6 := int64(li) * 4;
  writeln(i6);
  i6 := li;
  i6 := i6 * li;
  writeln(i6);
  writeln(sizeof(byte), sizeof(word), sizeof(shortint), sizeof(smallint),
          sizeof(longint), sizeof(cardinal), sizeof(int64), sizeof(double));
  writeln(high(byte), ' ', low(shortint), ' ', high(longint), ' ',
          low(int64), ' ', high(int64), ' ', high(cardinal));
  d := 1.5;
  d := d * 3;
  writeln(d:0:2);
  writeln(7 div 2, ' ', -7 div 2, ' ', -7 mod 2, ' ', int64(-7) mod 3);
  writeln(trunc(1.0e12), ' ', round(-2.5e10));
  { FNV-1a: relies on wrap-around inside the Q-/R- region }
  {$Q-}{$R-}
  h := 2166136261;
  for k := 1 to length(key) do
  begin
    h := h xor ord(key[k]);
    h := h * 16777619;
  end;
  {$Q+}{$R+}
  writeln(h);
  writeln(cardinal(1) shl 31, ' ', cardinal($80000000) shr 4, ' ',
          int64(1) shl 40, ' ', $F0 and $3C, ' ', $F0 or $0F, ' ', 5 xor 3);
end.
