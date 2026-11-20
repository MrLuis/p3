{ StringReplace counts the matches, then fills a result of its final length
  (docs/sysutils-review.md SU-1). No match, a match at the start and at
  the end, adjacent matches, overlapping candidates, a replacement longer,
  shorter and empty, an empty pattern, a pattern longer than the string,
  rfIgnoreCase with mixed case (the text kept is the string's own), only
  the first match, and a random comparison with a reference written here.
  Output identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-stringreplace.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-stringreplace.expected
}
program rtlstringreplace;
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

procedure Show(const s, from, into: string; flags: TReplaceFlags);
begin
  writeln('[', s, '] ', from, ' -> ', into, ' ', ord(rfReplaceAll in flags), ord(rfIgnoreCase in flags),
    ': [', StringReplace(s, from, into, flags), ']');
end;

{ the old way: search from the start of what is left, deleting }
function Reference(const s, from, into: string; flags: TReplaceFlags): string;
var
  rest, restCmp, fromCmp, r: string;
  p: longint;
begin
  if from = '' then
    exit(s);
  rest := s;
  if rfIgnoreCase in flags then
  begin
    restCmp := LowerCase(s);
    fromCmp := LowerCase(from);
  end
  else
  begin
    restCmp := s;
    fromCmp := from;
  end;
  r := '';
  repeat
    p := pos(fromCmp, restCmp);
    if p = 0 then
      break;
    r := r + copy(rest, 1, p - 1) + into;
    delete(rest, 1, p - 1 + length(from));
    delete(restCmp, 1, p - 1 + length(from));
  until not (rfReplaceAll in flags);
  Result := r + rest;
end;

function RandStr(maxLen: longint): string;
const
  Letters = 'abAB';
var
  i: longint;
begin
  Result := '';
  for i := 1 to Rnd(maxLen + 1) do
    Result := Result + Letters[1 + Rnd(4)];
end;

var
  i, bad: longint;
  s, f, t: string;
  fl: TReplaceFlags;
begin
  Show('hello world', 'xyz', 'Q', [rfReplaceAll]);
  Show('hello world', 'hello', 'HI', [rfReplaceAll]);
  Show('hello world', 'world', 'there', [rfReplaceAll]);
  Show('aaaa', 'a', 'bb', [rfReplaceAll]);
  Show('aaaa', 'a', 'bb', []);
  Show('aaa', 'aa', 'X', [rfReplaceAll]);
  Show('abababa', 'aba', '-', [rfReplaceAll]);
  Show('abcabc', 'abc', '', [rfReplaceAll]);
  Show('abcabc', 'b', '', []);
  Show('abc', '', 'X', [rfReplaceAll]);
  Show('', 'a', 'X', [rfReplaceAll]);
  Show('ab', 'abc', 'X', [rfReplaceAll]);
  Show('abc', 'abc', 'X', [rfReplaceAll]);
  Show('Hello HELLO hello', 'hello', 'bye', [rfReplaceAll, rfIgnoreCase]);
  Show('Hello HELLO hello', 'hello', 'bye', [rfIgnoreCase]);
  Show('Hello HELLO hello', 'hello', 'bye', [rfReplaceAll]);
  Show('AbC', 'abc', '<>', [rfIgnoreCase]);
  Show('one, two, three', ', ', ' and ', [rfReplaceAll]);

  seed := 5;
  bad := 0;
  for i := 1 to 3000 do
  begin
    s := RandStr(30);
    f := RandStr(3);
    t := RandStr(4);
    fl := [];
    if Rnd(2) = 0 then
      fl := fl + [rfReplaceAll];
    if Rnd(2) = 0 then
      fl := fl + [rfIgnoreCase];
    if StringReplace(s, f, t, fl) <> Reference(s, f, t, fl) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
