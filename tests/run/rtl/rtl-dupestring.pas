{ DupeString makes the result once and doubles the filled part
  (docs/strutils-review.md ST-4): count 0, 1, 2, 3, negative and large
  (powers of two and not), an empty s, one-character and longer s, a write
  to a one-copy result leaves the original, and random calls against the
  old algorithm. Output identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-dupestring.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-dupestring.expected
}
program rtldupestring;
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

{ the old way: one copy at a time }
function OldDupe(const s: string; count: longint): string;
begin
  Result := '';
  while count > 0 do
  begin
    Result := Result + s;
    count := count - 1;
  end;
end;

procedure Show(const s: string; count: longint);
begin
  writeln('DupeString("', s, '", ', count, ') = "', DupeString(s, count), '"');
end;

var
  i, n, count, bad: longint;
  s, t: string;
begin
  Show('ab', 0);
  Show('ab', -1);
  Show('ab', -100);
  Show('ab', 1);
  Show('ab', 2);
  Show('ab', 3);
  Show('ab', 7);
  Show('x', 8);
  Show('x', 9);
  Show('', 5);
  Show('', 0);
  Show('hello ', 5);
  t := DupeString('ab', 1000);
  writeln('length ', length(t), ' ', t[1], t[2], t[1999], t[2000], ' ', t = OldDupe('ab', 1000));
  t := DupeString('abc', 4097);
  writeln('length ', length(t), ' ', t = OldDupe('abc', 4097));
  writeln('length ', length(DupeString('-', 65536)));

  s := 'hello';
  t := DupeString(s, 1);
  t[1] := 'J';
  writeln(s, ' ', t);

  seed := 41;
  bad := 0;
  for i := 1 to 3000 do
  begin
    s := '';
    for n := 1 to Rnd(6) do
      s := s + chr(Ord('a') + Rnd(26));
    count := Rnd(40) - 3;
    if DupeString(s, count) <> OldDupe(s, count) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
