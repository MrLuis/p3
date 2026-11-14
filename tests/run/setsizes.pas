{ Sets of each size (optimization.md, item 6): a set takes 32 bits when its
  element type's highest value is at most 31, 64 bits when at most 63, and
  256 bits otherwise. Construction, membership and ranges at the edges of
  each size; the operators and comparisons within a size and across sizes
  (the larger representation holds the result); assignments that narrow;
  parameters; enumerations; for-in; sets in records, arrays and a typed
  file. Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=tp %s -o %t
  RUN: %t %t.tmp | diff - %S/setsizes.expected
  RUN: %pcc --mode=tp -O2 %s -o %t.2
  RUN: %t.2 %t.tmp | diff - %S/setsizes.expected
  RUN: %pcc --mode=tp -O2 -fno-range-checks %s -o %t.3
  RUN: %t.3 %t.tmp | diff - %S/setsizes.expected
}
program setsizes;
type
  TColor = (Red, Green, Blue, Cyan, Magenta, Yellow);
  TColors = set of TColor;
  TWarm = set of Red..Blue;
  TBig = (b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15,
          b16, b17, b18, b19, b20, b21, b22, b23, b24, b25, b26, b27, b28, b29,
          b30, b31, b32, b33, b34, b35, b36, b37, b38, b39);
  TBigs = set of TBig;
  TSmall = set of 0..31;
  TMid = set of 0..63;
  TLow = set of 3..10;
  TBytes = set of byte;
  TChars = set of char;
  TFlags = set of boolean;
  TRec = record
    tag: byte;
    small: TSmall;
    mid: TMid;
    wide: TBytes;
    colors: TColors;
  end;
var
  Primary: TColors;
  Evens: TSmall;
  High: TMid;
  Vowels: TChars;
  small, small2: TSmall;
  mid: TMid;
  low: TLow;
  wide: TBytes;
  colors: TColors;
  warm: TWarm;
  bigs: TBigs;
  chars: TChars;
  flags: TFlags;
  r: TRec;
  arr: array[1..3] of TSmall;
  i, n: integer;
  c: TColor;
  b: TBig;
  f: file of TRec;

procedure ShowSmall(const name: string; s: TSmall);
var k: integer;
begin
  write(name, ':');
  for k := 0 to 31 do
    if k in s then write(' ', k);
  writeln;
end;

procedure ShowMid(const name: string; s: TMid);
var k: integer;
begin
  write(name, ':');
  for k := 0 to 63 do
    if k in s then write(' ', k);
  writeln;
end;

procedure ShowWide(const name: string; const s: TBytes);
var k: integer;
begin
  write(name, ':');
  for k := 0 to 255 do
    if k in s then write(' ', k);
  writeln;
end;

procedure ShowColors(const name: string; s: TColors);
var k: TColor;
begin
  write(name, ':');
  for k := Red to Yellow do
    if k in s then write(' ', ord(k));
  writeln;
end;

procedure Union(a, b: TSmall; var res: TSmall);
begin
  res := a + b;
end;

procedure Widen(a: TSmall; var res: TBytes);
begin
  res := a;
end;

procedure AddTo(var s: TSmall; k: integer);
begin
  s := s + [k];
end;

procedure AddMid(var s: TMid; k: integer);
begin
  s := s + [k];
end;

begin
  Primary := [Red, Green, Blue];
  Evens := [0, 2, 4, 30];
  High := [40, 63];
  Vowels := ['a', 'e', 'i', 'o', 'u'];
  { each size: construction, membership at the edges }
  small := [0, 5, 31];
  mid := [0, 31, 32, 63];
  wide := [0, 31, 32, 63, 64, 200, 255];
  low := [3, 10];
  ShowSmall('small', small);
  ShowMid('mid', mid);
  ShowWide('wide', wide);
  writeln(31 in small, ' ', 32 in small, ' ', 63 in mid, ' ', 64 in mid, ' ',
          255 in wide, ' ', 256 in wide, ' ', -1 in small, ' ', 300 in mid);
  n := 31;
  writeln(n in small, ' ', (n + 1) in small, ' ', (n + 32) in mid, ' ', (n + 33) in mid);

  { ranges, also empty ones and up to the top bit }
  n := 28;
  small := [n..31];
  ShowSmall('range', small);
  small := [n..n - 1];
  ShowSmall('empty', small);
  mid := [60..n + 35];
  mid := mid + [0..2];
  ShowMid('midrange', mid);
  wide := [n..30];
  wide := wide + [250..255];
  ShowWide('widerange', wide);

  { operators within a size }
  small := [1, 2, 3];
  small2 := [3, 4];
  ShowSmall('union', small + small2);
  ShowSmall('inter', small * small2);
  ShowSmall('diff', small - small2);
  writeln(small = small2, ' ', small <> small2, ' ', [3] <= small, ' ',
          small >= [1, 2], ' ', small <= small2, ' ', small * small2 = [3]);

  { operators across sizes: the larger representation holds the result }
  small := [1, 31];
  mid := [2, 40];
  wide := [3, 200];
  ShowMid('small+mid', small + mid);
  ShowMid('mid+small', mid + small);
  ShowWide('small+wide', small + wide);
  ShowWide('wide+mid', wide + mid);
  ShowWide('mid-small+wide', mid - small + wide);
  ShowMid('mid*small', mid * small);
  ShowWide('wide-small', wide - small);
  writeln(small <= mid + small, ' ', mid >= small, ' ', wide = small, ' ',
          small + [] = small, ' ', [] <= mid, ' ', small * wide = [], ' ',
          mid = [2, 40], ' ', [1, 31] = small, ' ', [200, 3] = wide);
  wide := small;
  mid := small;
  writeln(wide = small, ' ', mid = small, ' ', small = wide, ' ', wide = mid);

  { narrowing assignments of values that fit }
  wide := [1, 2, 30];
  mid := [5, 6];
  small := wide * [0..31];
  ShowSmall('narrowed', small);
  small := mid;
  ShowSmall('from mid', small);
  low := small * [3..10];
  ShowSmall('low', low);

  { parameters and results }
  Union([1, 2], small, small2);
  ShowSmall('Union', small2);
  Widen([7, 31], wide);
  ShowWide('Widen', wide);
  small := [];
  for i := 0 to 31 do
    if odd(i) then AddTo(small, i);
  ShowSmall('odd', small);
  mid := [];
  for i := 30 to 63 do
    if i mod 3 = 0 then AddMid(mid, i);
  ShowMid('thirds', mid);

  { enumerations: small ones, a subrange, one with 40 values }
  colors := [Red, Yellow];
  colors := colors + [Green] - [Red];
  ShowColors('colors', colors);
  ShowColors('primary', Primary * (colors + [Blue]));
  warm := [Red, Blue];
  colors := warm + [Cyan];
  ShowColors('warm+', colors);
  warm := colors * [Red..Blue];
  writeln(Red in warm, ' ', Green in warm, ' ', warm <= Primary, ' ', Primary <= warm);
  bigs := [b0, b31, b32, b39];
  n := 0;
  for b := b0 to b39 do
    if b in bigs then n := n * 100 + ord(b);
  writeln(n);
  bigs := bigs - [b31..b32] + [b10];
  n := 0;
  for b in bigs do
    n := n + ord(b);
  writeln(n);
  flags := [true];
  writeln(true in flags, ' ', false in flags, ' ', flags + [false] = [false..true]);

  { for-in over each size }
  n := 0;
  for i in Evens do n := n * 10 + i;
  writeln(n);
  n := 0;
  for i in High do n := n * 100 + i;
  writeln(n);
  for c in Primary do write(ord(c), ' ');
  writeln;
  chars := Vowels + ['x'..'z'];
  n := 0;
  for i := 0 to 255 do
    if chr(i) in chars then n := n + 1;
  writeln(n);

  { in records, arrays and a typed file }
  r.tag := 7;
  r.small := [1, 31];
  r.mid := [33];
  r.wide := [100, 255];
  r.colors := [Magenta];
  arr[1] := r.small;
  arr[2] := arr[1] + [2];
  arr[3] := arr[2] * [2, 3];
  ShowSmall('arr2', arr[2]);
  ShowSmall('arr3', arr[3]);
  assign(f, paramstr(1));
  rewrite(f);
  write(f, r);
  r.small := [];
  r.wide := [];
  write(f, r);
  close(f);
  reset(f);
  read(f, r);
  close(f);
  ShowSmall('r.small', r.small);
  ShowMid('r.mid', r.mid);
  ShowWide('r.wide', r.wide);
  ShowColors('r.colors', r.colors);
  writeln(r.tag);
end.
