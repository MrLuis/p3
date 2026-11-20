{ AddChar, AddCharR, PadLeft and PadRight make the result once
  (docs/strutils-review.md ST-2): n below, equal to and above length(s), n 0
  and negative, an empty s, both sides, other pad characters, Dec2Numb (which
  pads with AddChar), a write to a result that adds nothing leaves the
  original, and random calls against the old algorithm. Output identical to
  Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-padding.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-padding.expected
}
program rtlpadding;
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

{ the old way: one character at a time, then a join }
function OldPad(c: char; n: longint): string;
begin
  Result := '';
  while n > 0 do
  begin
    Result := Result + c;
    n := n - 1;
  end;
end;

procedure Show(c: char; const s: string; n: longint);
begin
  writeln('AddChar(', c, ', "', s, '", ', n, ') = "', AddChar(c, s, n),
    '"  AddCharR = "', AddCharR(c, s, n), '"');
end;

var
  i, n, bad: longint;
  s, t, u: string;
  c: char;
begin
  Show('*', 'abc', 5);
  Show('*', 'abc', 4);
  Show('*', 'abc', 3);
  Show('*', 'abc', 2);
  Show('*', 'abc', 0);
  Show('*', 'abc', -3);
  Show('*', '', 3);
  Show('*', '', 0);
  Show('*', '', -1);
  Show('0', '42', 6);
  Show(#9, 'x', 3);
  writeln('PadLeft  "', PadLeft('abc', 20), '"');
  writeln('PadRight "', PadRight('abc', 20), '"');
  writeln('PadLeft  "', PadLeft('abc', 1), '"');
  writeln('PadRight "', PadRight('', 4), '"');
  writeln('length ', length(AddChar('-', '', 1000)), ' ', length(AddCharR('-', 'ab', 1000)));
  writeln('Dec2Numb ', Dec2Numb(255, 12, 2), ' ', Dec2Numb(255, 1, 16), ' ', Dec2Numb(0, 4, 10));

  s := 'hello';
  t := AddChar('*', s, 3);
  u := AddCharR('*', s, 5);
  t[1] := 'J';
  u[5] := '!';
  writeln(s, ' ', t, ' ', u);

  seed := 23;
  bad := 0;
  for i := 1 to 4000 do
  begin
    s := '';
    for n := 1 to Rnd(12) do
      s := s + chr(Ord('a') + Rnd(26));
    n := Rnd(20) - 3;
    c := chr(Ord('!') + Rnd(10));
    if AddChar(c, s, n) <> OldPad(c, n - length(s)) + s then
      bad := bad + 1;
    if AddCharR(c, s, n) <> s + OldPad(c, n - length(s)) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
