{ PopCnt, CountTrailingZeros, CountLeadingZeros: the bits of an integer in
  the width of its type (all of them for 0), at -O0 and -O2.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/bits.expected
  RUN: %p3 --mode=tp -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/bits.expected
}
program bits;
var
  b: Byte;
  w: Word;
  q: QWord;
  i: Int64;
  l: LongInt;
  s: ShortInt;
  n: Integer;
begin
  b := 12;
  w := 256;
  q := QWord(1) shl 40;
  i := -1;
  l := 0;
  s := -128;
  writeln(PopCnt(b), ' ', PopCnt(i), ' ', PopCnt(q), ' ', PopCnt(l), ' ',
          PopCnt(s), ' ', PopCnt(255));
  writeln(CountTrailingZeros(b), ' ', CountTrailingZeros(w), ' ',
          CountTrailingZeros(q), ' ', CountTrailingZeros(l), ' ',
          CountTrailingZeros(s), ' ', CountTrailingZeros(i));
  writeln(CountLeadingZeros(b), ' ', CountLeadingZeros(w), ' ',
          CountLeadingZeros(q), ' ', CountLeadingZeros(l), ' ',
          CountLeadingZeros(i), ' ', CountLeadingZeros(s));
  { the first set bit of each value }
  q := QWord($8040201008040201);
  n := 0;
  while q <> 0 do
  begin
    write(CountTrailingZeros(q), ' ');
    q := q and (q - 1);
    Inc(n);
  end;
  writeln('(', n, ')');
end.
