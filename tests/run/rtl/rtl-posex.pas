{ PosEx searches in place from the offset (docs/strutils-review.md ST-1):
  a match at the offset itself, before it (not found), at the end, a
  substring longer than what is left, the offset 0, 1, length(s) and
  length(s) + 1 (a negative offset is a range error in Free Pascal), an
  empty substring, overlapping candidates, the two-parameter form, a scan that goes on from each match, and random
  calls against a reference written here. Output identical to Free Pascal
  3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-posex.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-posex.expected
}
program rtlposex;
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

{ the old way: search a copy of the rest }
function Reference(const sub, s: string; offset: longint): longint;
var
  p: longint;
begin
  Result := 0;
  if (offset < 1) or (offset > length(s)) or (sub = '') then
    exit;
  p := pos(sub, copy(s, offset, high(longint)));
  if p > 0 then
    Result := p + offset - 1;
end;

procedure Show(const sub, s: string; offset: longint);
begin
  writeln('PosEx(', sub, ', ', s, ', ', offset, ') = ', PosEx(sub, s, offset));
end;

var
  i, n, bad, p, count: longint;
  s, sub: string;
begin
  Show('b', 'abcabc', 1);
  Show('b', 'abcabc', 2);
  Show('b', 'abcabc', 3);
  Show('b', 'abcabc', 5);
  Show('b', 'abcabc', 6);
  Show('c', 'abcabc', 6);
  Show('c', 'abcabc', 7);
  Show('abc', 'abcabc', 4);
  Show('abc', 'abcabc', 5);
  Show('abcd', 'abc', 1);
  Show('abc', 'abc', 1);
  Show('abc', 'abc', 0);
  Show('', 'abc', 1);
  Show('a', '', 1);
  Show('', '', 1);
  Show('aa', 'aaaa', 1);
  Show('aa', 'aaaa', 2);
  Show('aa', 'aaaa', 3);
  Show('aa', 'aaaa', 4);
  Show('ab', 'aab', 1);
  Show('aab', 'aaab', 1);
  writeln(PosEx('lo', 'hello world, hello'));
  writeln(PosEx('xyz', 'hello'));
  writeln(PosEx('', 'hello'));

  { a scan that goes on from each match }
  s := '';
  for i := 1 to 500 do
    s := s + 'ab, cd; ';
  count := 0;
  p := PosEx(',', s, 1);
  while p > 0 do
  begin
    count := count + p;
    p := PosEx(',', s, p + 1);
  end;
  writeln('scan: ', count);

  seed := 17;
  bad := 0;
  for i := 1 to 4000 do
  begin
    s := '';
    for n := 1 to Rnd(25) do
      s := s + chr(Ord('a') + Rnd(3));
    sub := '';
    for n := 1 to Rnd(4) do
      sub := sub + chr(Ord('a') + Rnd(3));
    n := Rnd(30);
    if PosEx(sub, s, n) <> Reference(sub, s, n) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
