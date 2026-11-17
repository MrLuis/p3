{ QWord, the unsigned 64-bit integer type, as in Free Pascal (64-bit
  targets), with its other names UInt64, NativeUInt, PtrUInt and SizeUInt:
  values up to 2^64-1, decimal literals beyond int64, unsigned div, mod,
  shr and comparisons, the operand rules (qword with a type of 32 bits or
  less, or with an int64 constant of zero or more: qword; with int64:
  int64; -qword: int64), constant folding, conversions to and from
  signed types and reals, High/Low, inc/dec, succ/pred, for loops, case
  labels, typed constants, writeln, overloads (unsigned arguments prefer
  qword, signed ones int64) and SysUtils' IntToStr, UIntToStr and
  StrToQWord. Range and overflow checks are off, as in Free Pascal by
  default: the output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/qword.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/qword.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/qword.expected
}
program qwordtest;
{$H+}
{$R-}
{$Q-}

uses sysutils;

const
  Big = 18446744073709551615;
  Half = 9223372036854775808;
  BelowHalf = 9223372036854775807;
  TQ: QWord = 18446744073709551614;
  { folded at compile time (qword with an int64 constant: qword) }
  CDiv = Big div 7;
  CMod = Big mod 10;
  CHalf = Half div Big;
  COne = Big div Half;
  CBig = Big div 3000000000;
  CPrime = 18446744073709551557 mod 1000000007;
  CReal = Big / 3;
  CGt = Half > 9223372036854775807;
  CLt = (Big div 2) < Half;
  Primes: array[0..2] of UInt64 = (2, 18446744073709551557, 3);

type
  TSmall = 0..10;
  TFour = array[0..(Big div 4611686018427387904)] of byte;

var
  q, r, k: QWord;
  u: UInt64;
  n: NativeUInt;
  pu: PtrUInt;
  sz: SizeUInt;
  i: longint;
  i6: int64;
  c: cardinal;
  w: word;
  b: byte;
  si: shortint;
  d: double;
  sm: TSmall;
  pq: ^QWord;

procedure P(x: int64); overload;
begin
  writeln('  int64 ', x);
end;

procedure P(x: QWord); overload;
begin
  writeln('  qword ', x);
end;

procedure R2(x: longint); overload;
begin
  writeln('  longint ', x);
end;

procedure R2(x: QWord); overload;
begin
  writeln('  qword ', x);
end;

function Twice(x: QWord): QWord;
begin
  Twice := x * 2;
end;

begin
  writeln('-- types');
  writeln(High(QWord), ' ', Low(QWord), ' ', SizeOf(QWord), ' ', SizeOf(UInt64));
  writeln(High(NativeUInt), ' ', High(PtrUInt), ' ', High(SizeUInt));
  writeln(SizeOf(NativeUInt), ' ', SizeOf(PtrUInt), ' ', SizeOf(SizeUInt));
  writeln(Big, ' ', Half, ' ', TQ, ' ', Primes[1]);
  writeln(Big:25, '|', Half:5, '|');

  writeln('-- constants');
  writeln(CDiv, ' ', CMod, ' ', CHalf, ' ', COne, ' ', CBig, ' ', CPrime);
  writeln(CReal / 1e18:0:6, ' ', CGt, ' ', CLt, ' ', SizeOf(TFour));

  writeln('-- arithmetic');
  q := Big;
  r := 10;
  writeln(q div r, ' ', q mod r, ' ', q shr 60, ' ', r shl 62);
  writeln(q - 1, ' ', not r, ' ', r and 3, ' ', r or 5, ' ', r xor q);
  writeln(q + 1, ' ', q * 3, ' ', Half div 3);
  writeln(q > r, ' ', q = High(QWord), ' ', Half > 5, ' ', r < Half);
  writeln(Twice(Half + 7), ' ', Twice(3));
  writeln(q div Half, ' ', q mod Half, ' ', Half - 1);

  writeln('-- mixed');
  r := 2;
  i := -3;
  i6 := -3;
  c := 3;
  w := 3;
  si := -3;
  writeln(r + i, ' ', r - c, ' ', r - w, ' ', r + si, ' ', r - 3);
  writeln(r + i6, ' ', i6 div r, ' ', c + i6);
  writeln(r * i, ' ', r div i);
  writeln(r > i, ' ', r > i6, ' ', r < si, ' ', r = i);
  writeln(-r);
  writeln(SizeOf(r + i), ' ', SizeOf(r + c), ' ', SizeOf(r + i6));

  writeln('-- conversions');
  q := Big;
  i6 := q;
  writeln(i6);
  q := i6;
  writeln(q);
  i := -3;
  q := i;
  writeln(q);
  writeln(QWord(-1), ' ', Int64(Half), ' ', longint(q), ' ', byte(q));
  d := Half;
  writeln(d / 1e18:0:3);
  q := Half + 1;
  d := q;
  writeln(d > 9.2e18, ' ', r / 4:0:2);
  q := Trunc(1e18);
  writeln(q);
  n := 77;
  pu := n;
  sz := pu + 1;
  u := sz;
  writeln(n, ' ', pu, ' ', sz, ' ', u);
  pq := @q;
  pq^ := pq^ + 1;
  writeln(q, ' ', Ord(Half) > 0);

  writeln('-- inc, dec, succ, pred');
  q := Big - 3;
  inc(q, 2);
  writeln(q);
  inc(q);
  writeln(q);
  dec(q, Big);
  writeln(q);
  dec(q);
  writeln(q);
  r := 10;
  writeln(succ(r), ' ', pred(r), ' ', odd(r), ' ', sqr(r));

  writeln('-- loops');
  r := Big - 2;
  for k := r to High(QWord) do
    write(k mod 10, ' ');
  writeln;
  for k := 3 downto 0 do
    write(k, ' ');
  writeln;
  r := Half - 1;
  for k := r to r + 2 do
    write(k - Half + 1, ' ');
  writeln;
  for k := 5 to 4 do
    write('never');
  sm := 3;
  for k := sm to 5 do
    write(k, ' ');
  writeln;

  writeln('-- case');
  r := Big - 3;
  for i := 0 to 4 do
  begin
    case r of
      0..9: writeln('small');
      Half: writeln('half');
      18446744073709551610..18446744073709551613: writeln('big range');
      Big: writeln('max');
    else
      writeln('else ', r);
    end;
    r := r + 1;
  end;
  r := Half;
  case r of
    0..BelowHalf: writeln('low half');
    Half..Big: writeln('high half');
  end;

  writeln('-- overloads');
  q := Big;
  c := 7;
  b := 1;
  w := 1;
  si := -1;
  i := -5;
  P(q);
  P(i);
  P(c);
  P(b);
  P(w);
  P(si);
  P(5);
  P(10000000000);
  P(Big);
  R2(c);
  R2(b);

  writeln('-- sysutils');
  writeln(IntToStr(q), ' ', IntToStr(i), ' ', IntToStr(c), ' ', UIntToStr(Half));
  writeln(StrToQWord('18446744073709551615'), ' ', StrToQWord('$FFFFFFFFFFFFFFFE'));
  writeln(StrToQWordDef('18446744073709551616', 7), ' ', StrToQWordDef('-1', 8));
  writeln(IntToHex(Big, 1), ' ', IntToHex(Half, 16));
end.
