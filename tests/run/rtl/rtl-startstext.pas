{ StartsText and EndsText compare in place without case
  (docs/strutils-review.md ST-5): equal with and without case, sub longer
  than s, empty sub and s, a difference in the first, a middle and the last
  character, non-letters that differ by 32 (which must not match), bytes
  above 127 (only ASCII letters fold, as before; Free Pascal also folds
  Latin-1 ones, so #200 against #232 is left out), and random mixed-case calls against the old algorithm. Output
  identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-startstext.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-startstext.expected
}
program rtlstartstext;
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
  writeln('"', sub, '" "', s, '": ', StartsText(sub, s), ' ', EndsText(sub, s));
end;

function Mix: char;
begin
  case Rnd(6) of
    0: Mix := 'a';
    1: Mix := 'A';
    2: Mix := 'b';
    3: Mix := 'B';
    4: Mix := '@';
  else
    Mix := '`';
  end;
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

  seed := 59;
  bad := 0;
  for i := 1 to 4000 do
  begin
    s := '';
    for n := 1 to Rnd(12) do
      s := s + Mix;
    sub := '';
    for n := 1 to Rnd(5) do
      sub := sub + Mix;
    if StartsText(sub, s) <> StartsStr(LowerCase(sub), LowerCase(s)) then
      bad := bad + 1;
    if EndsText(sub, s) <> EndsStr(LowerCase(sub), LowerCase(s)) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
