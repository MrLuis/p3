{ UpperCase and LowerCase return the string itself when no character
  changes, and otherwise copy it once and convert the rest through a
  pointer (docs/sysutils-review.md SU-7): empty, no letters, already in
  the target case, all letters, the first change at the first, a middle
  and the last position, every byte value from 0 to 255 (only ASCII
  letters change), the original left alone when the result changes, and
  random strings against a reference written here. Output identical to
  Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-casefold.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-casefold.expected
}
program rtlcasefold;
{$H+}
uses sysutils;

var
  seed: longint;

{$Q-}{$R-}
function Rnd(n: longint): longint;
begin
  seed := (seed * 1103515245 + 12345) and $7FFFFFFF;
  Result := (seed shr 8) mod n;
end;
{$Q+}{$R+}

function RefUpper(const s: string): string;
var
  i: longint;
begin
  Result := s;
  for i := 1 to length(Result) do
    if (Result[i] >= 'a') and (Result[i] <= 'z') then
      Result[i] := chr(ord(Result[i]) - 32);
end;

function RefLower(const s: string): string;
var
  i: longint;
begin
  Result := s;
  for i := 1 to length(Result) do
    if (Result[i] >= 'A') and (Result[i] <= 'Z') then
      Result[i] := chr(ord(Result[i]) + 32);
end;

procedure Show(const s: string);
begin
  writeln('[', s, '] -> [', UpperCase(s), '] [', LowerCase(s), ']');
end;

var
  i, k, bad: longint;
  s, t, all: string;
begin
  Show('');
  Show('1234 !?-_');
  Show('lower only');
  Show('UPPER ONLY');
  Show('MiXeD cAsE 123 wOrDs');
  Show('a');
  Show('Z');
  Show('1234567890a');
  Show('1234567890A');
  Show('abcdefghijklmnopqrstuvwxyz');
  Show('ABCDEFGHIJKLMNOPQRSTUVWXYZ');
  Show('first Letter then long tail of lowercase and UPPERCASE text, 42 chars+');
  Show('long digits 1234567890 1234567890 1234567890 and finally X');
  Show('long digits 1234567890 1234567890 1234567890 and finally x');
  Show('@[`{ the characters next to the letters ranges: AZ az');

  { every byte value }
  all := '';
  for i := 0 to 255 do
    all := all + chr(i);
  t := UpperCase(all);
  s := LowerCase(all);
  k := 0;
  bad := 0;
  for i := 0 to 255 do
  begin
    if t[i + 1] <> chr(i) then
      k := k + 1;
    if s[i + 1] <> chr(i) then
      bad := bad + 1;
  end;
  writeln('bytes changed by UpperCase: ', k, ' by LowerCase: ', bad);
  writeln(t = RefUpper(all), ' ', s = RefLower(all));

  { the original is not changed by a result that differs }
  s := 'Hello World';
  t := UpperCase(s);
  writeln(s, ' ', t);
  t := LowerCase(s);
  writeln(s, ' ', t);
  s := 'already lower';
  t := LowerCase(s);
  t[1] := 'A';
  writeln(s, ' ', t);
  s := 'ALREADY UPPER';
  t := UpperCase(s);
  t[1] := 'a';
  writeln(s, ' ', t);

  seed := 21;
  bad := 0;
  for i := 1 to 3000 do
  begin
    s := '';
    for k := 1 to Rnd(30) do
      s := s + chr(Rnd(256));
    if (UpperCase(s) <> RefUpper(s)) or (LowerCase(s) <> RefLower(s)) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
