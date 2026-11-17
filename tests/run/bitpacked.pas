{ bitpacked records and arrays (tp and objfpc), as in Free Pascal: ordinal
  fields and elements take as many bits as their values need, least
  significant bit first, without alignment (a boolean 1, a subrange 0..15
  4, an enumeration of 3 values 2, -4..3 3 in two's complement); other
  fields and a variant part start at a byte; the size is in whole bytes.
  Free Pascal's TByteOverlay (bits, nibbles and the byte overlaid), fields
  through with and methods, signed and offset subranges, variants, nested
  bitpacked records, multi-dimensional arrays, for ... in, const and value
  arguments. The bytes are dumped through absolute overlays. Output is
  identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/bitpacked.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/bitpacked.expected
  RUN: %p3 --mode=objfpc -O3 -fno-range-checks -fno-overflow-checks %s -o %t.O3
  RUN: %t.O3 | diff - %S/bitpacked.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/bitpacked.expected
}
program bitpacked;
{$H+}
{$modeswitch advancedrecords}
type
  TByteBitIndex = 0..7;
  TByteNibbleIndex = 0..1;
  nibble = 0..15;
  { from Free Pascal's RTL: for fast extraction of bits }
  TByteOverlay = bitpacked record case integer of
    0: (AsBit:    bitpacked array[TByteBitIndex]    of boolean);
    1: (AsNibble: bitpacked array[TByteNibbleIndex] of nibble);
    2: (AsByte:   byte);
  end;
  TColor = (red, green, blue);
  TFlags = bitpacked record
    a: boolean;
    b: 0..7;
    c: TColor;
    d: byte;
    e: 0..1023;
    f: char;
    g: longint;
    h: boolean;
  end;
  TSigned = bitpacked record
    a: -4..3;
    b: -1..0;
    c: shortint;
  end;
  TOffset = bitpacked record
    a: 5..6;
    b: 5..6;
    c: 100..101;
  end;
  TVariant = bitpacked record
    a: 0..7;
    case k: boolean of
      false: (x: 0..3; y: boolean);
      true: (z: 0..31);
  end;
  TNested = bitpacked record
    x: boolean;
    inner: bitpacked record
      a, b: 0..3;
    end;
    y: boolean;
    name: string[3];
    big: int64;
  end;
  TTriples = bitpacked array[0..9] of 0..7;
  TWords = bitpacked array[1..5] of word;
  TPairs = bitpacked array[0..3] of -2..1;
  TGrid = bitpacked array[0..2, 0..2] of 0..3;
  TColors = bitpacked array[TColor] of TColor;
  TBytes16 = array[0..15] of byte;

  { methods see a bitpacked record's fields unqualified }
  TPacked = bitpacked record
    lo, hi: nibble;
    function Sum: longint;
    procedure Swap;
  end;

function TPacked.Sum: longint;
begin
  Sum := lo + hi;
end;

procedure TPacked.Swap;
var
  t: nibble;
begin
  t := lo;
  lo := hi;
  hi := t;
end;

const
  HexDigits: array[0..15] of char = '0123456789ABCDEF';

procedure Dump(const b: TBytes16; n: longint);
var
  i: longint;
begin
  for i := 0 to n - 1 do
    write(HexDigits[b[i] shr 4], HexDigits[b[i] and 15], ' ');
  writeln;
end;

function Total(const t: TTriples): longint;
var
  i, sum: longint;
begin
  sum := 0;
  for i := 0 to 9 do
    sum := sum + t[i];
  Total := sum;
end;

procedure Show(v: nibble; b: boolean);
begin
  write(v, ' ', b, ' ');
end;

procedure ShowConst(const v: longint; const c: char);
begin
  write(v, ' ', c, ' ');
end;

var
  o: TByteOverlay;
  f: TFlags;
  fb: TBytes16 absolute f;
  sg: TSigned;
  sb: TBytes16 absolute sg;
  ofs: TOffset;
  ob: TBytes16 absolute ofs;
  v: TVariant;
  vb: TBytes16 absolute v;
  n: TNested;
  nb: TBytes16 absolute n;
  x, x2: TTriples;
  xb: TBytes16 absolute x;
  w: TWords;
  pr: TPairs;
  prb: TBytes16 absolute pr;
  g: TGrid;
  gb: TBytes16 absolute g;
  cs: TColors;
  p: TPacked;
  i, j: longint;
  c: TColor;

begin
  writeln('sizes');
  writeln(sizeof(TByteOverlay), ' ', sizeof(TFlags), ' ', sizeof(TSigned), ' ',
          sizeof(TOffset), ' ', sizeof(TVariant), ' ', sizeof(TNested), ' ',
          sizeof(TTriples), ' ', sizeof(TWords), ' ', sizeof(TPairs), ' ',
          sizeof(TGrid), ' ', sizeof(TColors), ' ', sizeof(TPacked));

  writeln('overlay');
  o.AsByte := $A5;
  for i := 0 to 7 do
    write(ord(o.AsBit[i]));
  writeln;
  writeln(o.AsNibble[0], ' ', o.AsNibble[1]);
  o.AsBit[1] := true;
  o.AsNibble[1] := 3;
  writeln(o.AsByte);
  with o do
  begin
    AsBit[7] := not AsBit[0];
    writeln(AsByte, ' ', AsBit[7]);
  end;

  writeln('fields');
  fillchar(f, sizeof(f), 0);
  f.a := true; Dump(fb, 9); fillchar(f, sizeof(f), 0);
  f.b := 7; Dump(fb, 9); fillchar(f, sizeof(f), 0);
  f.c := blue; Dump(fb, 9); fillchar(f, sizeof(f), 0);
  f.d := 255; Dump(fb, 9); fillchar(f, sizeof(f), 0);
  f.e := 1023; Dump(fb, 9); fillchar(f, sizeof(f), 0);
  f.f := #255; Dump(fb, 9); fillchar(f, sizeof(f), 0);
  f.g := -1; Dump(fb, 9); fillchar(f, sizeof(f), 0);
  f.h := true; Dump(fb, 9);
  f.a := true; f.b := 5; f.c := green; f.d := 200; f.e := 1000; f.f := 'z';
  f.g := -7; f.h := false;
  writeln(f.a, ' ', f.b, ' ', ord(f.c), ' ', f.d, ' ', f.e, ' ', f.f, ' ', f.g, ' ', f.h);
  with f do
  begin
    e := e - d;
    b := b - 1;
    writeln(e, ' ', b, ' ', c = green);
  end;

  writeln('signed and offset subranges');
  fillchar(sg, sizeof(sg), 0);
  sg.a := -1; Dump(sb, 2);
  sg.a := -4; sg.b := -1; sg.c := -128; Dump(sb, 2);
  writeln(sg.a, ' ', sg.b, ' ', sg.c);
  fillchar(ofs, sizeof(ofs), 0);
  ofs.a := 6; ofs.b := 5; ofs.c := 101; Dump(ob, 2);
  writeln(ofs.a, ofs.b, ofs.c);

  writeln('variants and nesting');
  fillchar(v, sizeof(v), 0); v.k := true; Dump(vb, 2);
  fillchar(v, sizeof(v), 0); v.x := 3; v.y := true; Dump(vb, 2);
  fillchar(v, sizeof(v), 0); v.z := 31; v.a := 5; Dump(vb, 2);
  writeln(v.a, ' ', v.z, ' ', v.x, ' ', v.y);
  fillchar(n, sizeof(n), 0);
  n.inner.b := 3; n.y := true; n.name := 'ab'; n.big := -2;
  Dump(nb, sizeof(n));
  writeln(n.x, ' ', n.inner.a, ' ', n.inner.b, ' ', n.y, ' ', n.name, ' ', n.big);

  writeln('arrays');
  for i := 0 to 9 do
    x[i] := i mod 8;
  Dump(xb, sizeof(x));
  for i := 0 to 9 do
    write(x[i]);
  writeln;
  x[3] := x[3] + 1;
  x[4] := x[4] + x[5] - 6;
  writeln(x[3], x[4], ' ', Total(x));
  x2 := x;
  x2[9] := 7;
  writeln(Total(x2), ' ', Total(x));
  w[2] := 65535;
  w[5] := 1234;
  writeln(w[2], ' ', w[5]);
  for i := 0 to 3 do
    pr[i] := i - 2;
  Dump(prb, 1);
  for i := 0 to 3 do
    write(pr[i], ' ');
  writeln;
  fillchar(g, sizeof(g), 0);
  g[1, 2] := 3;
  g[2][0] := 1;
  Dump(gb, sizeof(g));
  for i := 0 to 2 do
    for j := 0 to 2 do
      write(g[i, j]);
  writeln;
  for c := red to blue do
    cs[c] := TColor((ord(c) + 1) mod 3);
  for c := red to blue do
    write(ord(cs[c]));
  writeln;
  Show(o.AsNibble[0], o.AsBit[2]);
  writeln(o.AsBit[2] and not o.AsBit[3], ' ', o.AsNibble[0] * 2);

  writeln('for in, case, sets');
  for i in x do
    write(i);
  writeln;
  for i in pr do
    write(i, ' ');
  for c in cs do
    write(ord(c));
  writeln;
  ShowConst(f.e, f.f);
  case x[1] of
    1: write('one ');
  else
    write('other ');
  end;
  if f.b in [3..5] then
    write('in ');
  writeln(succ(f.f), ' ', ord(o.AsBit[0]), ' ', pred(cs[green]) = red);

  writeln('methods');
  p.lo := 3;
  p.hi := 12;
  writeln(p.Sum);
  p.Swap;
  writeln(p.lo, ' ', p.hi);
end.
