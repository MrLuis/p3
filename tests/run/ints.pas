{ S1 / L1 (Turbo/FPC): sized integer types, promotion and mixed
  signed/unsigned arithmetic, wrap-around under Q-/R-, int64 trunc/round,
  banker's rounding. The expected output was produced by Free Pascal 3.2.2
  (fpc -Mobjfpc -Sh- -Cr -Co).
  RUN: %pcc --mode=tp %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/ints.expected %t.out
  RUN: %pcc --mode=tp -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/ints.expected %t.O2.out
}
program ints;
var b: byte; w: word; si: shortint; c, h: cardinal; i6: int64; li: longint; k: longint; d: double;
begin
  b := 200; w := 60000; si := -100; c := 4000000000; li := 2000000000;
  writeln(b + b, ' ', w * 2, ' ', si * si, ' ', c, ' ', c + 1, ' ', c - li);
  i6 := li; i6 := i6 * li; writeln(i6);
  writeln(-c, ' ', not b, ' ', c div 7, ' ', c mod 7, ' ', -7 div 2, ' ', -7 mod 2);
  {$Q-}{$R-}
  h := 2166136261;
  for k := 1 to 3 do begin h := h xor ord('p'); h := h * 16777619; end;
  {$Q+}{$R+}
  writeln(h);
  writeln(trunc(1.0e12), ' ', round(-2.5e10), ' ', round(2.5), ' ', round(3.5), ' ', round(-2.5));
  d := c; writeln(d:0:1);
  writeln(c > li, ' ', si < b, ' ', 9000000000000000000);
  for b := 250 to 255 do write(b, ' '); writeln;
  i6 := 9223372036854775807; inc(i6, -1); writeln(i6);
  writeln(abs(si), ' ', sqr(si), ' ', ord(c));
end.
