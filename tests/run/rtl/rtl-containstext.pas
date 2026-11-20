{ ContainsText scans s in place (docs/strutils-review.md ST-10): a match
  differing only in case at the start, the end and the middle; sub empty
  (False, as ContainsStr) and longer than s; an empty s; a near miss with a
  non-letter that differs by 32 ('@' against '`'); a first character that
  occurs often before the match; equal bytes above 127; and random
  mixed-case calls against the old algorithm. Output identical to Free
  Pascal 3.2.2's (fpc -Mobjfpc), with only ASCII letters differing in case
  (Free Pascal also folds Latin-1 ones, p3 does not, as before).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-containstext.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-containstext.expected
}
program rtlcontainstext;
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

{ the old way: lowercase both, then search }
function OldContains(const s, sub: string): boolean;
begin
  Result := ContainsStr(LowerCase(s), LowerCase(sub));
end;

procedure Show(const s, sub: string);
begin
  writeln('"', s, '" "', sub, '": ', ContainsText(s, sub));
end;

function Mix: char;
begin
  case Rnd(7) of
    0: Mix := 'a';
    1: Mix := 'A';
    2: Mix := 'b';
    3: Mix := 'B';
    4: Mix := '@';
    5: Mix := '`';
  else
    Mix := '1';
  end;
end;

var
  i, n, bad: longint;
  s, sub: string;
begin
  Show('The Quick Brown Fox', 'the');
  Show('The Quick Brown Fox', 'FOX');
  Show('The Quick Brown Fox', 'qUICK bROWN');
  Show('The Quick Brown Fox', 'The Quick Brown Fox');
  Show('The Quick Brown Fox', 'THE QUICK BROWN FOXES');
  Show('The Quick Brown Fox', 'Foxy');
  Show('The Quick Brown Fox', 'quick  brown');
  Show('The Quick Brown Fox', '');
  Show('', '');
  Show('', 'a');
  Show('a', 'A');
  Show('A', 'a');
  Show('@bc', '`BC');
  Show('`bc', '@BC');
  Show('x@y', 'X`Y');
  Show('[z]', '{Z}');
  Show('1234', '23');
  Show('aaaaaaaaab', 'AAB');
  Show('aaaaaaaaaa', 'AAB');
  Show('abababababc', 'ABC');
  Show('a'#200'b', #200'B');
  Show('a'#200'b', #201'B');

  seed := 73;
  bad := 0;
  for i := 1 to 5000 do
  begin
    s := '';
    for n := 1 to Rnd(16) do
      s := s + Mix;
    sub := '';
    for n := 1 to Rnd(5) do
      sub := sub + Mix;
    if ContainsText(s, sub) <> OldContains(s, sub) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
