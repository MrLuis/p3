{ ReverseString makes the result once and writes it through a pointer
  (docs/strutils-review.md ST-7): empty, one character, even and odd
  lengths, bytes above 127, a long string, the original left as it was, and
  random strings against the old algorithm. Output identical to Free Pascal
  3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-reversestring.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-reversestring.expected
}
program rtlreversestring;
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

{ the old way: copy, then write by index }
function OldReverse(const s: string): string;
var
  i: longint;
begin
  Result := s;
  for i := 1 to length(s) do
    Result[i] := s[length(s) - i + 1];
end;

procedure Show(const s: string);
begin
  writeln('"', s, '" -> "', ReverseString(s), '"');
end;

var
  i, n, bad: longint;
  s, t: string;
begin
  Show('');
  Show('a');
  Show('ab');
  Show('abc');
  Show('abcd');
  Show('Hello, World!');
  Show('a'#200'b'#255'c');
  s := '';
  for i := 0 to 255 do
    s := s + chr(i);
  t := ReverseString(s);
  writeln('256: ', length(t), ' ', ord(t[1]), ' ', ord(t[256]), ' ', t = OldReverse(s), ' ', ReverseString(t) = s);
  s := 'unchanged';
  t := ReverseString(s);
  t[1] := 'X';
  writeln(s, ' ', t);

  seed := 61;
  bad := 0;
  for i := 1 to 3000 do
  begin
    s := '';
    for n := 1 to Rnd(40) do
      s := s + chr(Rnd(256));
    if ReverseString(s) <> OldReverse(s) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
