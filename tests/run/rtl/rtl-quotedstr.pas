{ QuotedStr counts the quotes, makes the result at its final length and
  copies the text between them in runs (docs/sysutils-review.md SU-3):
  the empty string, no quote, only quotes, a quote first, last and in the
  middle, adjacent quotes, and random strings against a reference written
  here. Output identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtl-quotedstr.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/rtl-quotedstr.expected
}
program rtlquotedstr;
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

function Reference(const s: string): string;
var
  i: longint;
begin
  Result := '''';
  for i := 1 to length(s) do
  begin
    if s[i] = '''' then
      Result := Result + '''';
    Result := Result + s[i];
  end;
  Result := Result + '''';
end;

procedure Show(const s: string);
begin
  writeln('[', s, '] ', length(s), ' -> [', QuotedStr(s), '] ', length(QuotedStr(s)));
end;

const
  Letters = 'ab''''''c d';

var
  i, k, bad: longint;
  s: string;
begin
  Show('');
  Show('plain');
  Show('''');
  Show('''''');
  Show('''''''');
  Show('it''s');
  Show('''start');
  Show('end''');
  Show('''both''');
  Show('a''''b');
  Show('a''b''c''d');
  Show('one two three four');
  seed := 9;
  bad := 0;
  for i := 1 to 3000 do
  begin
    s := '';
    for k := 1 to Rnd(40) do
      s := s + Letters[1 + Rnd(length(Letters))];
    if QuotedStr(s) <> Reference(s) then
      bad := bad + 1;
  end;
  writeln('random differences: ', bad);
end.
