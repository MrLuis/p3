{ Vector conversions: T(v) between vectors of one length, lane by lane:
  a mask's booleans to 0 and 1 (case conversion of ASCII letters), wider
  and narrower integer lanes (by the source's sign), integers to reals,
  nonzero lanes to true.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/vector-convert.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/vector-convert.expected
  RUN: not %p3 --mode=objfpc -c %S/Inputs/vector-convert-errors.pas -o %t.o 2>&1 | FileCheck %s
  CHECK: invalid typecast from 'T16' to 'T8'
  CHECK: invalid typecast from 'TD' to 'T8'
}
program conv;
type
  TB = vector[8] of Byte;
  TS = vector[8] of ShortInt;
  TW = vector[8] of Word;
  TI = vector[8] of Integer;
  TM = vector[8] of Boolean;
  TD = vector[8] of Double;
var
  b: TB; s: TS; w: TW; i: TI; m: TM; d: TD; k: Integer;
begin
  for k := 1 to 8 do begin b[k] := k * 30; s[k] := k * 15 - 60; end;
  m := b >= 120;
  b := b - TB(m) * 32;
  for k := 1 to 8 do write(b[k], ' '); writeln;
  w := TW(b) * 300; for k := 1 to 8 do write(w[k], ' '); writeln;
  i := TI(s); for k := 1 to 8 do write(i[k], ' '); writeln;
  d := TD(s) * 0.5; for k := 1 to 8 do write(d[k]:0:1, ' '); writeln;
  b := TB(w); for k := 1 to 8 do write(b[k], ' '); writeln;
  m := TM(s); for k := 1 to 8 do write(Ord(m[k])); writeln;
end.
