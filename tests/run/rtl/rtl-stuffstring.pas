{ StuffString makes its result once (docs/strutils-review.md ST-8): start
  1, inside, at the end and past it; count 0 and past the end; an empty
  sub and an empty s; and random calls against the old algorithm (copy +
  sub + copy). Output identical to Free Pascal 3.2.2's (fpc -Mobjfpc), whose
  start and count are cardinals: start 0 (a wrong result there) and a
  negative start or count (runtime error 217) are checked against the old
  algorithm only under p3.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-stuffstring.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-stuffstring.expected
}
program rtlstuffstring;
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

{ the old way }
function OldStuff(const s: string; start, count: longint; const sub: string): string;
begin
  Result := copy(s, 1, start - 1) + sub + copy(s, start + count, high(longint));
end;

procedure Show(const s: string; start, count: longint; const sub: string);
begin
  writeln('StuffString("', s, '", ', start, ', ', count, ', "', sub, '") = "',
    StuffString(s, start, count, sub), '"');
end;

var
  i, n, start, count, bad: longint;
  s, sub: string;
begin
  Show('abcdef', 1, 0, 'XY');
  Show('abcdef', 1, 2, 'XY');
  Show('abcdef', 3, 2, 'XY');
  Show('abcdef', 3, 0, 'XY');
  Show('abcdef', 6, 1, 'XY');
  Show('abcdef', 6, 5, 'XY');
  Show('abcdef', 7, 0, 'XY');
  Show('abcdef', 7, 3, 'XY');
  Show('abcdef', 10, 3, 'XY');
  Show('abcdef', 2, 100, 'XY');
  Show('abcdef', 1, 6, '');
  Show('abcdef', 2, 3, '');
  Show('abcdef', 2, 0, '');
  Show('', 1, 0, 'XY');
  Show('', 1, 3, '');

  seed := 67;
  bad := 0;
  for i := 1 to 4000 do
  begin
    s := '';
    for n := 1 to Rnd(12) do
      s := s + chr(Ord('a') + Rnd(26));
    sub := '';
    for n := 1 to Rnd(5) do
      sub := sub + chr(Ord('A') + Rnd(26));
    start := 1 + Rnd(15);
    count := Rnd(16);
    if StuffString(s, start, count, sub) <> OldStuff(s, start, count, sub) then
      bad := bad + 1;
{$IFDEF P3_OBJFPC}
    start := Rnd(6) - 4;
    count := Rnd(20) - 4;
    if StuffString(s, start, count, sub) <> OldStuff(s, start, count, sub) then
      bad := bad + 1;
    start := Rnd(20);
    count := Rnd(8) - 7;
    if StuffString(s, start, count, sub) <> OldStuff(s, start, count, sub) then
      bad := bad + 1;
{$ENDIF}
  end;
  writeln('random differences: ', bad);
end.
