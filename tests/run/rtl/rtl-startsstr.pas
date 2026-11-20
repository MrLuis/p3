{ StartsStr and EndsStr compare in place (docs/strutils-review.md ST-6):
  equal with and without case, sub longer than s, empty sub and s, a
  difference in the first, a middle and the last character, non-letters
  that differ by 32, bytes above 127, the Ansi forms, and random calls
  against the old algorithm. Output identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-startsstr.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-startsstr.expected
}
program rtlstartsstr;
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

procedure Show(const sub, s: string);
begin
  writeln('"', sub, '" "', s, '": ', StartsStr(sub, s), ' ', EndsStr(sub, s),
    ' ', AnsiStartsStr(sub, s), ' ', AnsiEndsStr(sub, s));
end;

var
  i, n, bad: longint;
  s, sub: string;
begin
  Show('abc', 'abcdef');
  Show('def', 'abcdef');
  Show('abcdef', 'abcdef');
  Show('ABC', 'abcdef');
  Show('DEF', 'abcdef');
  Show('aBcDeF', 'AbCdEf');
  Show('abcdefg', 'abcdef');
  Show('xabcdef', 'abcdef');
  Show('', 'abcdef');
  Show('abc', '');
  Show('', '');
  Show('xbc', 'abcdef');
  Show('axc', 'abcdef');
  Show('abx', 'abcdef');
  Show('xef', 'abcdef');
  Show('dxf', 'abcdef');
  Show('dex', 'abcdef');
  Show('@', '`bc');
  Show('a@', 'a`');
  Show('`', '@bc');
  Show('[', '{');
  Show('Z', 'z');
  Show('the quick brown', 'The Quick Brown Fox');
  Show('BROWN FOX', 'The Quick Brown Fox');
  Show('The Quick Brown', 'The Quick Brown Fox');
  Show('Brown Fox', 'The Quick Brown Fox');
  Show(#200'a', #200'abc');
  Show(#200, #232);

  seed := 53;
  bad := 0;
  for i := 1 to 4000 do
  begin
    s := '';
    for n := 1 to Rnd(12) do
      s := s + chr(Ord('a') + Rnd(3));
    sub := '';
    for n := 1 to Rnd(5) do
      sub := sub + chr(Ord('a') + Rnd(3));
    if StartsStr(sub, s) <> ((length(sub) <= length(s)) and (copy(s, 1, length(sub)) = sub)) then
      bad := bad + 1;
    if EndsStr(sub, s) <> ((length(sub) <= length(s)) and (RightStr(s, length(sub)) = sub)) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
