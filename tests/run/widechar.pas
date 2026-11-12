{ widechar (and unicodechar) and character codes above 255 (#$D800,
  #$20AC; tp and objfpc), as in Free Pascal: a lone code above 255 is a
  widechar constant, a 16-bit ordinal that mixes with chars (comparisons,
  case labels, subranges, assignment of a char), and becomes a char or a
  string as FPC converts it without a unicode string manager: a code up to
  255 is that char, anything above is '?' (also in a string literal:
  'a'#$20AC'b' is 'a?b'). Variables, typed constants, fields (packed too),
  pointers, var parameters and results; ord, succ, pred, low, high,
  typecasts, for loops, in, write. Output is identical to Free Pascal
  3.2.2's (fpc -Mtp and -Mobjfpc).
  RUN: %pcc --mode=tp %s -o %t
  RUN: %t | diff - %S/widechar.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.objfpc
  RUN: %t.objfpc | diff - %S/widechar.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/widechar.expected
}
program widechartest;
type
  TSurrogate = #$D800..#$DBFF;
  TArr = array[1..3] of widechar;
  TRec = record
    c: char;
    w: widechar;
    n: byte;
  end;
  TBits = bitpacked record
    a: boolean;
    w: widechar;
  end;
  PWide = ^widechar;

const
  HiSurrogate = #$D800;
  Euro = #$20AC;
  Mixed = 'a'#$20AC'b';
  Pair = #$D83D#$DE00;
  Tc: widechar = #$20AC;
  Ta: TArr = ('a', #$E9, #$FFFF);
  AsChar: char = #$41;

var
  w, w2: widechar;
  u: unicodechar;
  c: char;
  s: ansistring;
  ss: shortstring;
  r: TRec;
  b: TBits;
  p: PWide;
  i: integer;
  counts: array[#$FFFE..#$FFFF] of integer;

procedure Show(x: widechar);
begin
  writeln('  show ', ord(x));
end;

procedure Bump(var v: widechar);
begin
  v := succ(v);
end;

function FromInt(x: integer): widechar;
begin
  FromInt := widechar(x);
end;

function Narrow(x: widechar): char;
begin
  Narrow := x;
end;

begin
  writeln('-- constants');
  writeln(ord(HiSurrogate), ' ', ord(Euro), ' ', ord(Tc), ' ', ord(AsChar));
  writeln(ord(Ta[1]), ' ', ord(Ta[2]), ' ', ord(Ta[3]));
  writeln(Mixed, ' ', length(Mixed), ' ', Pair, ' ', length(Pair));
  writeln(ord(low(TSurrogate)), ' ', ord(high(TSurrogate)));
  writeln(ord(low(widechar)), ' ', ord(high(widechar)), ' ', sizeof(widechar));
  writeln(ord(HiSurrogate) + 1, ' ', HiSurrogate = #$D800, ' ', ord(succ(Euro)));
  writeln(ord(widechar(65)), ' ', ord(widechar(#$20AC)), ' ', ord(pred(#$100)));

  writeln('-- to char and strings');
  w := #$E9;
  s := w;
  writeln(length(s), ' ', ord(s[1]));
  w := #$80;
  c := w;
  writeln(ord(c));
  w := #$20AC;
  c := w;
  ss := w;
  writeln(ord(c), ' ', ord(ss[1]), ' ', ord(char(w)), ' ', ord(Narrow(w)));
  s := Euro;
  writeln(s, ' ', length(s));
  w := 'z';
  s := 'a' + w;
  ss := w + 'b';
  writeln(s, ' ', ss);
  w := 'q';
  writeln(w, ' ', w:3, ' ', #$20AC);

  writeln('-- with chars');
  c := 'x';
  w := c;
  u := w;
  writeln(ord(w), ' ', ord(u));
  w2 := #$E9;
  writeln(w = 'x', ' ', 'x' = w, ' ', w < w2, ' ', w2 > #$100, ' ', w2 = #$E9);
  writeln(c < Euro, ' ', c = HiSurrogate);
  w := 'm';
  writeln(w in ['a'..'z'], ' ', #$100 in ['a'..'z']);
  w := #$100;
  writeln(w in ['a'..'z']);

  writeln('-- calls');
  Show(#$41);
  Show('B');
  Show(HiSurrogate);
  Show(c);
  writeln(ord(FromInt(66)), ' ', ord(FromInt(70000)));

  writeln('-- storage');
  writeln(sizeof(TRec), ' ', sizeof(TBits), ' ', sizeof(TArr));
  r.w := #$3A9;
  r.c := 'z';
  writeln(ord(r.w), ' ', r.c);
  b.w := #$ABCD;
  b.a := true;
  writeln(ord(b.w), ' ', b.a);
  new(p);
  p^ := #$1234;
  Bump(p^);
  writeln(ord(p^));
  dispose(p);
  counts[#$FFFF] := 7;
  writeln(counts[high(widechar)]);

  writeln('-- control');
  i := 0;
  for w := 'x' to #$7A do
    i := i + 1;
  writeln(i);
  for w := #$FFFE to high(widechar) do
    write(ord(w), ' ');
  writeln;
  w := #$263A;
  case w of
    #0..#$7F: writeln('ascii');
    #$2600..#$26FF: writeln('symbols');
  else
    writeln('other');
  end;
  w := #$DC00;
  case w of
    'a'..'p': writeln('letter');
    #$D800..#$DFFF: writeln('surrogate');
  end;
end.
